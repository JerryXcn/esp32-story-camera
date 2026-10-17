"""契约测试：路由、字段校验、鉴权、错误信封（实施计划 12.1 契约组）。"""
from __future__ import annotations

import json
from pathlib import Path

import pytest

from conftest import FIXTURES, TOKEN_B, auth, post_job, sha256, wait_terminal


RESP = FIXTURES / "responses"


def test_health_no_internal_config(client):
    r = client.get("/v1/health")
    assert r.status_code == 200
    body = r.json()
    assert body == {"status": "ok", "schema_version": 1}


def test_auth_required(client):
    r = client.get("/v1/jobs/job_x")
    assert r.status_code == 401
    assert r.json()["error"]["code"] == "AUTH_FAILED"
    r = client.get("/v1/jobs/job_x", headers={"Authorization": "Bearer wrong"})
    assert r.status_code == 401


def test_create_contract_shape(client):
    (r, op) = post_job(client)
    assert r.status_code == 202
    body = r.json()
    # 与 tests/fixtures/responses/create_202.json 同构
    sample = json.loads((RESP / "create_202.json").read_text())
    assert set(body.keys()) == set(sample.keys())
    assert body["status"] == "queued"
    assert body["stage"] == "queued"
    assert body["attempt"] == 1
    assert body["revision"] == 1
    assert body["error"] is None
    assert body["operation_id"] == op


def test_by_operation_recovery_query(client):
    (r, op) = post_job(client)
    job_id = r.json()["job_id"]
    r2 = client.get(f"/v1/jobs/by-operation/{op}", headers=auth())
    assert r2.status_code == 200
    assert r2.json()["job_id"] == job_id
    # 未知 operation -> 404
    r3 = client.get("/v1/jobs/by-operation/" + "f" * 32, headers=auth())
    assert r3.status_code == 404


def test_image_invalid_cases(client):
    op_base = "a" * 32
    # 非 JPEG（PNG 字节）
    r, _ = post_job(client, image=(FIXTURES / "images" / "not_jpeg.jpg").read_bytes(),
                    operation_id=op_base)
    assert r.status_code == 422 and r.json()["error"]["code"] == "IMAGE_INVALID"
    # 空文件
    r, _ = post_job(client, image=b"", operation_id="b" * 32)
    assert r.status_code == 422
    # 截断 JPEG
    r, _ = post_job(client, image=(FIXTURES / "images" / "truncated.jpg").read_bytes(),
                    operation_id="c" * 32)
    assert r.status_code == 422
    # 超过 1MiB
    r, _ = post_job(client, image=(FIXTURES / "images" / "oversized.jpg").read_bytes(),
                    operation_id="d" * 32)
    assert r.status_code == 413 and r.json()["error"]["code"] == "IMAGE_INVALID"
    # 长边超过 1280
    r, _ = post_job(client, image=(FIXTURES / "images" / "oversize_dimensions.jpg").read_bytes(),
                    operation_id="e" * 32)
    assert r.status_code == 422
    # sha256 与实际不符
    r, _ = post_job(client, image_sha256="0" * 64, operation_id="f" * 32)
    assert r.status_code == 422


def test_invalid_fields(client):
    cases = [
        {"operation_id": "short"},                       # 非 32 hex
        {"story_length": "ultra"},                       # 非法长度
        {"age_group": "2-3"},                            # 非法年龄组
        {"voice_id": "cool_male_99"},                    # P0 未提供的声音
        {"language": "en-US"},                           # P0 仅 zh-CN
        {"mode": "regen"},                               # 非法 mode
        {"base_version": "-1"},
    ]
    for i, kwargs in enumerate(cases):
        r, _ = post_job(client, **kwargs)
        assert r.status_code == 422, f"case {kwargs} -> {r.status_code} {r.text}"


def test_operation_id_header_mismatch(client):
    r, _ = post_job(client, operation_id="1" * 32, headers={"Idempotency-Key": "2" * 32})
    assert r.status_code == 422


def test_missing_image_field(client):
    r, _ = post_job(client, omit_image=True)
    assert r.status_code == 422


def test_cross_device_access_is_404(client):
    (r, _) = post_job(client)
    job_id = r.json()["job_id"]
    r2 = client.get(f"/v1/jobs/{job_id}", headers=auth(TOKEN_B))
    assert r2.status_code == 404
    r3 = client.get(f"/v1/jobs/{job_id}/result", headers=auth(TOKEN_B))
    assert r3.status_code == 404
    r4 = client.get(f"/v1/jobs/{job_id}/audio", headers=auth(TOKEN_B))
    assert r4.status_code == 404


def test_full_pipeline_and_audio_download(client):
    (r, op) = post_job(client)
    job_id = r.json()["job_id"]
    snap = wait_terminal(client, job_id)
    assert snap["status"] == "succeeded", snap
    assert snap["stage"] == "ready"

    res = client.get(f"/v1/jobs/{job_id}/result", headers=auth()).json()
    sample = json.loads((RESP / "result_ready.json").read_text())
    assert set(res.keys()) == set(sample.keys())
    assert 1 <= len(res["title"]) <= 20
    assert res["story"]
    audio = res["audio"]
    assert audio["path"] == f"/v1/jobs/{job_id}/audio"
    assert audio["format"] == "mp3" and audio["sample_rate_hz"] == 24000
    assert audio["channels"] == 1 and audio["bit_rate_bps"] == 64000
    assert audio["bytes"] <= 2 * 1024 * 1024

    dl = client.get(f"/v1/jobs/{job_id}/audio", headers=auth())
    assert dl.status_code == 200
    assert sha256(dl.content) == audio["sha256"]
    assert len(dl.content) == audio["bytes"]
