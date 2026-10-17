"""幂等与并发测试（实施计划 12.1 幂等组）。"""
from __future__ import annotations

import threading
import time

import base64

from conftest import TOKEN_A, post_job, wait_terminal
from app.providers.mock_audio_b64 import _MOCK_MP3_B64

# 契约规格真实 MP3（SRV-01 修复后：探测失败的坏音频一律拒绝，测试不能再用
# 19 字节假音频冒充供应商输出）
VALID_MP3 = base64.b64decode(_MOCK_MP3_B64)


def test_same_key_same_content_returns_same_job(client):
    (r1, op) = post_job(client, operation_id="3" * 32)
    (r2, _) = post_job(client, operation_id="3" * 32)
    assert r1.json()["job_id"] == r2.json()["job_id"]
    # 首个还在 queued/running 时返回 202；终态后重复查询返回 200
    assert r2.status_code in (200, 202)


def test_same_key_different_content_conflict(client):
    import io

    from PIL import Image
    buf = io.BytesIO()
    Image.new("RGB", (640, 480), (90, 140, 200)).save(buf, "JPEG", quality=85)
    other_image = buf.getvalue()          # 合法但内容不同的图

    (r1, op) = post_job(client, operation_id="4" * 32)
    (r2, _) = post_job(client, operation_id="4" * 32, image=other_image)
    assert r2.status_code == 409
    err = r2.json()["error"]
    assert err["code"] == "IDEMPOTENCY_CONFLICT"
    assert err["job_id"] == r1.json()["job_id"]


def test_device_busy_when_job_inflight(client):
    gate = threading.Event()

    class SlowVision:
        def understand(self, image_jpeg, age_group, deadline=None):
            gate.wait(timeout=10)
            return {"suitable": True, "reason": "", "description": "测试"}

        def write_story(self, image_jpeg, description, age_group, story_length, language, deadline=None):
            return {"title": "忙碌测试", "story": "等待中的故事。"}

    class InstantTTS:
        def synthesize(self, text, voice, deadline=None):
            return VALID_MP3

    app = client.app
    app.state.providers.vision = SlowVision()
    app.state.providers.tts = InstantTTS()

    (r1, op) = post_job(client, operation_id="5" * 32)
    job_id = r1.json()["job_id"]
    deadline = time.time() + 5
    while time.time() < deadline:
        if client.get(f"/v1/jobs/{job_id}", headers={"Authorization": "Bearer " + TOKEN_A}).json()["status"] == "running":
            break
        time.sleep(0.02)

    (r2, _) = post_job(client, operation_id="6" * 32)
    assert r2.status_code == 409
    err = r2.json()["error"]
    assert err["code"] == "DEVICE_BUSY"
    assert err["job_id"] == job_id          # 携带本人可查询的 job_id

    gate.set()
    snap = wait_terminal(client, job_id)
    assert snap["status"] == "succeeded"


def test_duplicated_post_does_not_double_count_quota(client, env_settings):
    (r1, op) = post_job(client, operation_id="7" * 32)
    wait_terminal(client, r1.json()["job_id"])
    (r2, _) = post_job(client, operation_id="7" * 32)   # 重复幂等请求
    assert r2.status_code == 200
    from datetime import datetime, timezone
    day = datetime.now(timezone.utc).strftime("%Y-%m-%d")
    assert client.app.state.db.quota_count("dev-device-001", day) == 1
