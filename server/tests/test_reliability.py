"""可靠性回归测试（优化建议说明 V1.0 §2.2 复现缺陷的修复验证）。

每个用例对应审查复现的一个确定缺陷：
- 永久 HTTP 错误曾进入通用重试（401 被调用 3 次）；
- suitable="false" 字符串曾被 bool() 转 True（内容审核误放行）；
- 探测失败的坏音频曾被"默认规格"放行。
"""
from __future__ import annotations

import base64
import threading

import pytest

from conftest import TOKEN_A, post_job, wait_terminal
from app.errors import JobFailure
from app.providers.base import PermanentHTTP, RetryableHTTP, http_post_json, parse_json_object
from app.providers.bigmodel_vision import BigmodelVision
from app.providers.mock_audio_b64 import _MOCK_MP3_B64

VALID_MP3 = base64.b64decode(_MOCK_MP3_B64)
AUTH = {"Authorization": "Bearer " + TOKEN_A}


class _Resp:
    def __init__(self, status_code, text=""):
        self.status_code = status_code
        self.text = text

    def json(self):
        return {}


def test_permanent_http_called_once(monkeypatch):
    """SRV-01：确定性 4xx 不再重试（曾复现 401 调用 3 次）。"""
    import httpx

    import httpx

    import app.providers.base as base
    calls = {"n": 0}

    class FakeResp:
        status_code = 401
        text = '{"error":"unauthorized"}'

        def json(self):
            return {}

    def fake_post(*a, **k):
        calls["n"] += 1
        return FakeResp()

    monkeypatch.setattr(httpx, "post", fake_post)

    def fn():
        return base.http_post_json("http://example.invalid", {}, {}, 1)

    with pytest.raises(PermanentHTTP):
        base.with_retries(fn, retries=2, base_delay_s=0.01, deadline_monotonic=None)
    assert calls["n"] == 1


def test_retryable_http_still_retries(monkeypatch):
    """可重试传输错误保留退避重试语义。"""
    import app.providers.base as base
    calls = {"n": 0}

    def fn():
        calls["n"] += 1
        raise RetryableHTTP("HTTP 500")
    with pytest.raises(ProviderCallFailed := base.ProviderCallFailed):
        base.with_retries(fn, retries=2, base_delay_s=0.01, deadline_monotonic=None)
    assert calls["n"] == 3


def test_suitable_string_false_is_rejected():
    """SRV-01：suitable 必须是 JSON 布尔；"false" 字符串不得转 True 放行。"""
    v = BigmodelVision(api_key="k", base_url="http://example.invalid",
                       vision_model="m", story_model="m", timeout_s=1)

    class FakeV(BigmodelVision):
        def _chat(self, model, prompt, image_jpeg, deadline=None):
            return {"suitable": "false", "reason": "", "description": "synthetic"}

    v2 = FakeV(api_key="k", base_url="http://example.invalid",
               vision_model="m", story_model="m", timeout_s=1)
    with pytest.raises(JobFailure):
        v2.understand(b"jpeg", "5-6")


def test_suitable_bool_false_blocks_tts(client):
    """端到端：模型返回 suitable=false 时任务 rejected，不做 TTS。"""
    class RejectVision:
        def understand(self, image_jpeg, age_group, deadline=None):
            return {"suitable": False, "reason": "测试拒绝", "description": ""}

        def write_story(self, image_jpeg, description, age_group, story_length, language, deadline=None):
            raise AssertionError("拒绝后不得继续写故事")

    app = client.app
    old = app.state.providers.vision
    app.state.providers.vision = RejectVision()
    app.state.providers.tts = _CountingTTS()
    try:
        (r, op) = post_job(client, operation_id="a" * 32)
        snap = wait_terminal(client, r.json()["job_id"])
        assert snap["status"] == "rejected"
        assert snap["error"]["code"] == "CONTENT_REJECTED"
        assert app.state.providers.tts.calls == 0   # 审核不通过不进 TTS
    finally:
        app.state.providers.vision = old


class _CountingTTS:
    def __init__(self):
        self.calls = 0

    def synthesize(self, text, voice, deadline=None):
        self.calls += 1
        return VALID_MP3


def test_invalid_audio_rejected(client):
    """SRV-01：探测失败的坏音频不再按默认规格放行（任务 failed，非 succeeded）。"""
    class BadTTS:
        def synthesize(self, text, voice, deadline=None):
            return b"\x00" * 18        # 18 字节非 MP3 数据

    app = client.app
    old = app.state.providers.tts
    app.state.providers.tts = BadTTS()
    try:
        (r, op) = post_job(client, operation_id="b" * 32)
        snap = wait_terminal(client, r.json()["job_id"])
        assert snap["status"] == "failed"
        assert snap["error"]["code"] == "TTS_FAILED"
    finally:
        app.state.providers.tts = old


def test_contract_mp3_succeeds(client):
    """契约规格 MP3 正常成功（音频验证正路径不回归）。"""
    class OkTTS(_CountingTTS):
        def synthesize(self, text, voice, deadline=None):
            self.calls += 1
            return VALID_MP3

    app = client.app
    old = app.state.providers.tts
    tts = OkTTS()
    app.state.providers.tts = tts
    try:
        (r, op) = post_job(client, operation_id="c" * 32)
        snap = wait_terminal(client, r.json()["job_id"])
        assert snap["status"] == "succeeded"
        assert tts.calls == 1
    finally:
        app.state.providers.tts = old
