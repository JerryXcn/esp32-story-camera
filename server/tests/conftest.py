"""测试公共设施：mock 供应商模式 + 独立数据目录 + TestClient。"""
from __future__ import annotations

import hashlib
import os
import sys
import tempfile
import time
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]   # 仓库根目录
sys.path.insert(0, str(ROOT / "server"))
FIXTURES = ROOT / "tests" / "fixtures"

TOKEN_A = "dev-token-001"
TOKEN_B = "test-token-b"
DEVICE_A = "dev-device-001"
DEVICE_B = "test-device-b"


def _ensure_generated_fixtures() -> None:
    """oversized.jpg（约 1.3 MB）不入库，首次运行测试时生成。"""
    if not (FIXTURES / "images" / "oversized.jpg").exists():
        import importlib.util
        spec = importlib.util.spec_from_file_location("make_fixtures", FIXTURES / "make_fixtures.py")
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        mod.make_oversized(FIXTURES / "images" / "oversized.jpg")


_ensure_generated_fixtures()


def _img_bytes(name: str) -> bytes:
    return (FIXTURES / "images" / name).read_bytes()


@pytest.fixture()
def env_settings(tmp_path, monkeypatch):
    monkeypatch.setenv("SC_PROVIDER_MODE", "mock")
    monkeypatch.setenv("SC_DATA_DIR", str(tmp_path / "data"))
    monkeypatch.setenv("SC_DEVICE_TOKENS", f"{TOKEN_A}:{DEVICE_A},{TOKEN_B}:{DEVICE_B}")
    monkeypatch.setenv("SC_QUOTA_PER_DEVICE_DAILY", "50")
    from importlib import import_module

    config = import_module("app.config")
    config.reset_settings_for_tests()
    yield config.get_settings()
    config.reset_settings_for_tests()


@pytest.fixture()
def client(env_settings):
    from fastapi.testclient import TestClient
    from app.main import create_app

    app = create_app()
    with TestClient(app) as c:   # with 语句触发 lifespan（启动调度器）
        yield c


def auth(token: str = TOKEN_A) -> dict:
    return {"Authorization": f"Bearer {token}"}


def sha256(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def post_job(client, *, image: bytes | None = None, operation_id: str | None = None,
             token: str = TOKEN_A, story_id: str | None = None, mode: str = "generate",
             base_version: str = "0", age_group: str = "5-6", story_length: str = "standard",
             voice_id: str = "warm_female_01", language: str = "zh-CN",
             image_sha256: str | None = None, headers: dict | None = None,
             omit_image: bool = False):
    image = _img_bytes("picturebook_valid.jpg") if image is None else image
    story_id = story_id or "11111111111111111111111111111111"
    operation_id = operation_id or hashlib.sha256(f"{time.time()}{token}{story_id}".encode()).hexdigest()[:32]
    data = {
        "operation_id": operation_id,
        "story_id": story_id,
        "mode": mode,
        "base_version": base_version,
        "age_group": age_group,
        "story_length": story_length,
        "voice_id": voice_id,
        "language": language,
        "image_sha256": sha256(image) if image_sha256 is None else image_sha256,
    }
    files = {} if omit_image else {"image": ("page.jpg", image, "image/jpeg")}
    h = dict(auth(token))
    h["Idempotency-Key"] = operation_id
    if headers:
        h.update(headers)
    return client.post("/v1/jobs", data=data, files=files, headers=h), operation_id


def wait_terminal(client, job_id: str, timeout_s: float = 15.0) -> dict:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        r = client.get(f"/v1/jobs/{job_id}", headers=auth())
        body = r.json()
        if body["status"] in ("succeeded", "failed", "rejected", "expired"):
            return body
        time.sleep(0.05)
    raise AssertionError(f"job {job_id} not terminal within {timeout_s}s")


class CountingVision:
    """包装 vision，统计各阶段调用次数。"""

    def __init__(self, inner):
        self.inner = inner
        self.understand_calls = 0
        self.story_calls = 0

    def understand(self, image_jpeg, age_group, deadline=None):
        self.understand_calls += 1
        return self.inner.understand(image_jpeg, age_group, deadline=deadline)

    def write_story(self, image_jpeg, description, age_group, story_length, language, deadline=None):
        self.story_calls += 1
        return self.inner.write_story(image_jpeg, description, age_group,
                                      story_length, language, deadline=deadline)
