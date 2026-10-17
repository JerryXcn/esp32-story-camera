"""状态机测试（实施计划 12.1 状态机组）：

- 阶段只前进、attempt/revision 防回退
- TTS 失败后正文仍可取回，retry 只补声音，不重做视觉
- CONTENT_REJECTED 不可重试
- 重启恢复标 PROVIDER_RESULT_UNKNOWN，显式重试可完成
- TTL 到期：result/audio 返回 410，媒体删除
"""
from __future__ import annotations

import time

import pytest

from conftest import CountingVision, post_job, wait_terminal


def test_stage_progression_monotonic(client):
    (r, op) = post_job(client, operation_id="8" * 32)
    job_id = r.json()["job_id"]
    seen = []
    deadline = time.time() + 10
    while time.time() < deadline:
        snap = client.get(f"/v1/jobs/{job_id}", headers={"Authorization": "Bearer dev-token-001"}).json()
        seen.append((snap["attempt"], snap["revision"], snap["stage"]))
        if snap["status"] in ("succeeded", "failed", "rejected"):
            break
        time.sleep(0.01)
    assert snap["status"] == "succeeded"
    # 同一 attempt 内阶段只前进
    stages_in_attempt = [s for (a, _, s) in seen if a == 1]
    order = ["queued", "understanding", "story_generating", "tts_generating", "ready"]
    idx = [order.index(s) for s in stages_in_attempt if s in order]
    assert idx == sorted(idx)


def test_tts_failure_keeps_story_and_retry_tts_only(client):
    class FailingTTS:
        def synthesize(self, text, voice, deadline=None):
            from app.errors import JobFailure
            raise JobFailure("TTS_FAILED", True, "tts_generating")

    app = client.app
    counting = CountingVision(app.state.providers.vision)
    original_tts = app.state.providers.tts
    app.state.providers.vision = counting
    app.state.providers.tts = FailingTTS()

    (r, op) = post_job(client, operation_id="9" * 32)
    job_id = r.json()["job_id"]
    snap = wait_terminal(client, job_id)
    assert snap["status"] == "failed"
    assert snap["error"]["code"] == "TTS_FAILED"
    assert snap["error"]["retryable"] is True
    assert snap["attempt"] == 1

    # 正文已通过检查：result 可返回，audio 为 null（实施计划 7.3）
    res = client.get(f"/v1/jobs/{job_id}/result", headers={"Authorization": "Bearer dev-token-001"})
    assert res.status_code == 200
    body = res.json()
    assert body["title"] and body["story"]
    assert body["audio"] is None

    # 恢复声音供应后显式重试：只做 TTS
    app.state.providers.tts = original_tts
    retry = client.post(
        f"/v1/jobs/{job_id}/retry",
        json={"expected_attempt": 1},
        headers={"Authorization": "Bearer dev-token-001", "Idempotency-Key": "rc" + "1" * 30},
    )
    assert retry.status_code == 202
    snap2 = wait_terminal(client, job_id)
    assert snap2["status"] == "succeeded"
    assert snap2["attempt"] == 2
    assert snap2["revision"] > snap["revision"]
    # 视觉只调用了一次（TTS 重试不重做识图）
    assert counting.understand_calls == 1
    assert counting.story_calls == 1


def test_stale_attempt_rejected(client):
    class FailingTTS:
        def synthesize(self, text, voice, deadline=None):
            from app.errors import JobFailure
            raise JobFailure("TTS_FAILED", True, "tts_generating")

    app = client.app
    app.state.providers.tts = FailingTTS()
    (r, op) = post_job(client, operation_id="aa" * 16)
    job_id = r.json()["job_id"]
    snap = wait_terminal(client, job_id)
    assert snap["status"] == "failed"

    r1 = client.post(f"/v1/jobs/{job_id}/retry", json={"expected_attempt": 99},
                     headers={"Authorization": "Bearer dev-token-001", "Idempotency-Key": "rc" + "2" * 30})
    assert r1.status_code == 409
    assert r1.json()["error"]["code"] == "STALE_ATTEMPT"


def test_content_rejected_not_retryable(client):
    class Rejecting:
        def understand(self, image_jpeg, age_group, deadline=None):
            return {"suitable": False, "reason": "不适合", "description": ""}

        def write_story(self, *a, **k):
            raise AssertionError("should not be called")

    client.app.state.providers.vision = Rejecting()
    (r, op) = post_job(client, operation_id="bb" * 16)
    job_id = r.json()["job_id"]
    snap = wait_terminal(client, job_id)
    assert snap["status"] == "rejected"
    assert snap["error"]["code"] == "CONTENT_REJECTED"
    assert snap["error"]["retryable"] is False

    res = client.get(f"/v1/jobs/{job_id}/result", headers={"Authorization": "Bearer dev-token-001"})
    assert res.status_code == 409
    assert res.json()["error"]["code"] == "RESULT_NOT_READY"

    retry = client.post(f"/v1/jobs/{job_id}/retry", json={"expected_attempt": 1},
                        headers={"Authorization": "Bearer dev-token-001", "Idempotency-Key": "rc" + "3" * 30})
    assert retry.status_code == 409
    assert retry.json()["error"]["code"] == "NOT_RETRYABLE"


def test_result_not_ready_before_story(client):
    class FailEarly:
        def understand(self, image_jpeg, age_group, deadline=None):
            raise TimeoutError("simulated outage")

        def write_story(self, *a, **k):
            raise AssertionError

    client.app.state.providers.vision = FailEarly()
    (r, op) = post_job(client, operation_id="cc" * 16)
    job_id = r.json()["job_id"]
    snap = wait_terminal(client, job_id)
    assert snap["status"] == "failed"
    assert snap["error"]["code"] == "AI_FAILED"
    assert snap["error"]["retryable"] is True
    res = client.get(f"/v1/jobs/{job_id}/result", headers={"Authorization": "Bearer dev-token-001"})
    assert res.status_code == 409


def test_restart_recovery_provider_unknown_then_explicit_retry(tmp_path, env_settings):
    """服务重启：未完成任务标 PROVIDER_RESULT_UNKNOWN；显式重试后完成。"""
    import threading

    from fastapi.testclient import TestClient
    from app.main import create_app

    gate = threading.Event()

    class BlockedVision:
        def understand(self, image_jpeg, age_group, deadline=None):
            gate.wait(timeout=10)
            return {"suitable": True, "reason": "", "description": "测试"}

        def write_story(self, image_jpeg, description, age_group, story_length, language, deadline=None):
            return {"title": "重启恢复", "story": "重启后完成的测试故事。"}

    app1 = create_app()
    with TestClient(app1) as c1:
        c1.app.state.providers.vision = BlockedVision()
        (r, op) = post_job(c1, operation_id="dd" * 16)
        job_id = r.json()["job_id"]
        deadline = time.time() + 5
        while time.time() < deadline:
            if c1.get(f"/v1/jobs/{job_id}", headers={"Authorization": "Bearer dev-token-001"}).json()["status"] == "running":
                break
            time.sleep(0.02)
        # 模拟崩溃时任务正在 TTS 阶段执行：直接写成运行中状态后“断电”
        c1.app.state.db.update_job(job_id, {"stage": "tts_generating"})

    # 崩溃时间线：worker 线程仍在 gate 上（数据库已关闭→安静降级），放行即可
    gate.set()

    # “重启”：同一数据目录新实例，启动恢复应接管
    from importlib import import_module
    config = import_module("app.config")
    config.reset_settings_for_tests()
    app2 = create_app()
    with TestClient(app2) as c2:
        snap = c2.get(f"/v1/jobs/{job_id}", headers={"Authorization": "Bearer dev-token-001"}).json()
        assert snap["status"] == "failed"
        assert snap["error"]["code"] == "PROVIDER_RESULT_UNKNOWN"
        assert snap["error"]["retryable"] is False

        retry = c2.post(f"/v1/jobs/{job_id}/retry", json={"expected_attempt": snap["attempt"]},
                        headers={"Authorization": "Bearer dev-token-001", "Idempotency-Key": "rc" + "4" * 30})
        assert retry.status_code == 202
        deadline = time.time() + 15
        while time.time() < deadline:
            snap2 = c2.get(f"/v1/jobs/{job_id}", headers={"Authorization": "Bearer dev-token-001"}).json()
            if snap2["status"] in ("succeeded", "failed"):
                break
            time.sleep(0.05)
        assert snap2["status"] == "succeeded"


def test_ttl_expiry_and_cleanup(client, env_settings):
    (r, op) = post_job(client, operation_id="ee" * 16)
    job_id = r.json()["job_id"]
    snap = wait_terminal(client, job_id)
    assert snap["status"] == "succeeded"

    # 回拨到期时间，触发清理
    client.app.state.db.update_job(job_id, {"expires_at": time.time() - 1})
    client.app.state.cleanup.run_once()

    assert client.get(f"/v1/jobs/{job_id}/result", headers={"Authorization": "Bearer dev-token-001"}).status_code == 410
    assert client.get(f"/v1/jobs/{job_id}/audio", headers={"Authorization": "Bearer dev-token-001"}).status_code == 410
    media_dir = client.app.state.media.root / "media" / job_id
    assert not media_dir.exists()
