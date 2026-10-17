"""V1 任务契约路由（实施计划第 7 节）。

- POST /v1/jobs                          multipart 创建，202；重复请求返回原任务
- GET  /v1/jobs/by-operation/{op_id}     POST 响应丢失/断电恢复查询
- GET  /v1/jobs/{job_id}                 状态、attempt、revision、stage、error
- GET  /v1/jobs/{job_id}/result          通过检查的 title/story 与 audio 描述
- POST /v1/jobs/{job_id}/retry           已失败且允许的显式恢复
- GET  /v1/jobs/{job_id}/audio           契约 MP3 下载（鉴权）
- GET  /v1/health                        服务探测，不泄露内部配置
"""
from __future__ import annotations

import hashlib
import re
import time
from datetime import datetime, timezone

from fastapi import APIRouter, Request
from fastapi.responses import JSONResponse, FileResponse

from ..config import get_settings
from ..db import canonical_digest, new_job_id
from ..errors import ApiError, error_body
from ..providers.factory import build_providers
from ..services.auth import require_device
from ..services.image_check import sha256_hex, validate_image
from ..services.media_store import MediaStore
from ..workers.executor import JobExecutor, CleanupLoop, datetime_iso

router = APIRouter()

HEX32_RE = re.compile(r"^[0-9a-f]{32}$")
AGE_GROUPS = {"3-4", "5-6", "7-10"}
STORY_LENGTHS = {"short", "standard", "long"}
VOICE_IDS = {"warm_female_01"}      # P0 仅一种逻辑声音
LANGUAGES = {"zh-CN"}               # P0 仅简体中文
MODES = {"generate", "regenerate"}


# ---------- 序列化 ----------

def job_snapshot(row) -> dict:
    err = None
    if row["error_code"]:
        err = {
            "code": row["error_code"],
            "retryable": bool(row["error_retryable"]),
            "stage": row["error_stage"],
            "job_id": row["job_id"],
        }
    return {
        "schema_version": 1,
        "job_id": row["job_id"],
        "operation_id": row["operation_id"],
        "status": row["status"],
        "stage": row["stage"],
        "attempt": row["attempt"],
        "revision": row["revision"],
        "poll_after_ms": row["poll_after_ms"],
        "error": err,
        "expires_at": datetime_iso(row["expires_at"]) if row["expires_at"] else None,
    }


def _err_response(e: ApiError) -> JSONResponse:
    return JSONResponse(status_code=e.status, content=e.payload())


def _get_owned_job(request: Request, job_id: str, device_id: str):
    row = request.app.state.db.get_job(job_id)
    if row is None or row["device_id"] != device_id:
        # 无权读取其他设备任务按 404 处理（实施计划 7.1）
        raise ApiError(404, "JOB_NOT_FOUND", False, None, job_id)
    return row


# ---------- 健康检查 ----------

@router.get("/v1/health")
def health():
    # 不得向未认证调用返回内部配置
    return {"status": "ok", "schema_version": 1}


# ---------- 创建任务 ----------

@router.post("/v1/jobs")
async def create_job(request: Request):
    s = get_settings()
    device_id = require_device(request)

    content_length = request.headers.get("content-length")
    if content_length and int(content_length) > s.multipart_max_bytes + 4096:
        raise ApiError(413, "IMAGE_INVALID", False, "queued")

    idempotency_key = request.headers.get("idempotency-key", "").strip()
    if not HEX32_RE.match(idempotency_key):
        raise ApiError(422, "INVALID_REQUEST", False, None)

    form = await request.form()
    try:
        operation_id = str(form.get("operation_id", ""))
        story_id = str(form.get("story_id", ""))
        mode = str(form.get("mode", ""))
        base_version_raw = str(form.get("base_version", ""))
        image_sha256_claim = str(form.get("image_sha256", "")).lower()
        age_group = str(form.get("age_group", ""))
        story_length = str(form.get("story_length", ""))
        voice_id = str(form.get("voice_id", ""))
        language = str(form.get("language", ""))
        upload = form.get("image")
        image_bytes = await upload.read() if upload is not None else b""
    except Exception:
        raise ApiError(422, "INVALID_REQUEST", False, None)

    # ---- 字段校验（实施计划 7.2） ----
    if operation_id != idempotency_key or not HEX32_RE.match(operation_id):
        raise ApiError(422, "INVALID_REQUEST", False, None)
    if not HEX32_RE.match(story_id):
        raise ApiError(422, "INVALID_REQUEST", False, None)
    if mode not in MODES or age_group not in AGE_GROUPS or story_length not in STORY_LENGTHS:
        raise ApiError(422, "INVALID_REQUEST", False, None)
    if voice_id not in VOICE_IDS or language not in LANGUAGES:
        raise ApiError(422, "INVALID_REQUEST", False, None)
    try:
        base_version = int(base_version_raw)
        if base_version < 0:
            raise ValueError
    except ValueError:
        raise ApiError(422, "INVALID_REQUEST", False, None)

    actual_sha = validate_image(image_bytes, image_sha256_claim,
                                s.image_max_bytes, s.image_max_edge)
    db = request.app.state.db

    digest = canonical_digest([operation_id, story_id, mode, str(base_version),
                               actual_sha, age_group, story_length, voice_id, language])

    # ---- 幂等（实施计划 7.2/7.4） ----
    existing = db.get_idempotency(device_id, operation_id)
    if existing is not None:
        if existing["request_digest"] != digest:
            raise ApiError(409, "IDEMPOTENCY_CONFLICT", False, None, existing["job_id"])
        job = db.get_job(existing["job_id"])
        if job is not None:
            status = job["status"]
            return JSONResponse(status_code=202 if status in ("queued", "running") else 200,
                                content=job_snapshot(job))
        # 摘要在但任务行已清除（超过保留期）：视为新请求

    # ---- 每设备并发 1（实施计划 7.4） ----
    busy = db.running_job_for_device(device_id)
    if busy is not None:
        raise ApiError(409, "DEVICE_BUSY", False, busy["stage"], busy["job_id"])

    # ---- 额度（产品文档 10：额度耗尽不影响离线播放） ----
    day = datetime.now(timezone.utc).strftime("%Y-%m-%d")
    if db.quota_count(device_id, day) >= s.quota_per_device_daily:
        raise ApiError(429, "QUOTA_EXCEEDED", False, None)
    db.bump_quota(device_id, day)
    quota_committed = True

    job_id = new_job_id()
    now = time.time()
    db.insert_job({
        "job_id": job_id,
        "operation_id": operation_id,
        "device_id": device_id,
        "story_id": story_id,
        "mode": mode,
        "base_version": base_version,
        "age_group": age_group,
        "story_length": story_length,
        "voice_id": voice_id,
        "language": language,
        "image_path": str(request.app.state.media.save_image(job_id, image_bytes)),
        "image_sha256": actual_sha,
        "status": "queued",
        "stage": "queued",
        "attempt": 1,
        "revision": 1,
        "poll_after_ms": s.poll_after_running_ms,
        "created_at": now,
    })
    db.put_idempotency(device_id, operation_id, digest, job_id)
    request.app.state.executor.notify()

    row = db.get_job(job_id)
    return JSONResponse(status_code=202, content=job_snapshot(row))


# ---------- 按 operation 查询（断电/响应丢失恢复） ----------

@router.get("/v1/jobs/by-operation/{operation_id}")
def get_by_operation(operation_id: str, request: Request):
    device_id = require_device(request)
    row = request.app.state.db.get_job_by_operation(device_id, operation_id)
    if row is None:
        raise ApiError(404, "JOB_NOT_FOUND", False, None)
    return job_snapshot(row)


# ---------- 状态查询 ----------

@router.get("/v1/jobs/{job_id}")
def get_job(job_id: str, request: Request):
    device_id = require_device(request)
    row = _get_owned_job(request, job_id, device_id)
    return job_snapshot(row)


# ---------- 结果 ----------

@router.get("/v1/jobs/{job_id}/result")
def get_result(job_id: str, request: Request):
    device_id = require_device(request)
    row = _get_owned_job(request, job_id, device_id)
    if row["status"] == "expired":
        raise ApiError(410, "RESULT_EXPIRED", False, row["stage"], job_id)
    if row["title"] is None:
        # 尚无通过检查的文本：结果不可用（实施计划 7.1 RESULT_NOT_READY）
        raise ApiError(409, "RESULT_NOT_READY", False, row["stage"], job_id)

    audio = None
    if row["audio_sha256"]:
        audio = {
            "path": f"/v1/jobs/{job_id}/audio",
            "format": "mp3",
            "sample_rate_hz": 24000,
            "channels": 1,
            "bit_rate_bps": 64000,
            "duration_ms": row["duration_ms"],
            "bytes": row["audio_bytes"],
            "sha256": row["audio_sha256"],
        }
    return {
        "schema_version": 1,
        "job_id": job_id,
        "operation_id": row["operation_id"],
        "title": row["title"],
        "story": row["story"],
        "audio": audio,
        "expires_at": datetime_iso(row["expires_at"]) if row["expires_at"] else None,
    }


# ---------- 显式重试（实施计划 7.4） ----------

@router.post("/v1/jobs/{job_id}/retry")
async def retry_job(job_id: str, request: Request):
    device_id = require_device(request)
    row = _get_owned_job(request, job_id, device_id)

    command_key = request.headers.get("idempotency-key", "").strip()
    if not command_key:
        raise ApiError(422, "INVALID_REQUEST", False, row["stage"], job_id)

    db = request.app.state.db
    try:
        body = await request.json()
    except (ValueError, TypeError):
        raise ApiError(422, "INVALID_REQUEST", False, row["stage"], job_id) from None
    if not isinstance(body, dict):
        raise ApiError(422, "INVALID_REQUEST", False, row["stage"], job_id)
    expected_attempt = body.get("expected_attempt")
    if expected_attempt is not None and (type(expected_attempt) is not int or expected_attempt < 1):
        raise ApiError(422, "INVALID_REQUEST", False, row["stage"], job_id)
    # JSON parsing yields control. Re-read before checking/changing this attempt.
    row = _get_owned_job(request, job_id, device_id)

    seen = db.get_recovery(device_id, command_key)
    if seen is not None:
        if seen["job_id"] != job_id:
            raise ApiError(409, "IDEMPOTENCY_CONFLICT", False, row["stage"], job_id)
        cur = db.get_job(seen["job_id"])
        if cur is not None:
            status = cur["status"]
            return JSONResponse(status_code=202 if status in ("queued", "running") else 200,
                                content=job_snapshot(cur))

    if row["status"] == "expired":
        raise ApiError(410, "RESULT_EXPIRED", False, row["stage"], job_id)
    if row["status"] != "failed":
        raise ApiError(409, "NOT_RETRYABLE", False, row["stage"], job_id)
    # SRV-02：重试入口同样执行设备并发约束（该设备已有其他运行任务 -> 409）
    busy = db.running_job_for_device(device_id)
    if busy is not None and busy["job_id"] != job_id:
        raise ApiError(409, "DEVICE_BUSY", False, busy["stage"], busy["job_id"])
    code = row["error_code"] or ""
    retryable = bool(row["error_retryable"])
    if not retryable and code != "PROVIDER_RESULT_UNKNOWN":
        # 内容拒绝、非法输入、幂等冲突、认证失败等不自动重试（实施计划 7.4）
        raise ApiError(409, "NOT_RETRYABLE", False, row["stage"], job_id)
    if expected_attempt is not None and expected_attempt != row["attempt"]:
        # 过期 expected_attempt：返回当前状态并拒绝重复执行
        raise ApiError(409, "STALE_ATTEMPT", False, row["stage"], job_id)

    db.put_recovery(device_id, command_key, job_id)
    resume_stage = "tts_generating" if row["title"] is not None else "queued"
    request.app.state.executor.retry(job_id, resume_stage)
    cur = db.get_job(job_id)
    return JSONResponse(status_code=202, content=job_snapshot(cur))


# ---------- 音频下载 ----------

@router.get("/v1/jobs/{job_id}/audio")
def get_audio(job_id: str, request: Request):
    device_id = require_device(request)
    row = _get_owned_job(request, job_id, device_id)
    if row["status"] in ("queued", "running"):
        raise ApiError(409, "RESULT_NOT_READY", False, row["stage"], job_id)
    if row["status"] == "expired" or not row["audio_sha256"]:
        raise ApiError(410, "RESULT_EXPIRED", False, row["stage"], job_id)
    path = request.app.state.media.audio_path(job_id)
    if not path.exists():
        raise ApiError(410, "RESULT_EXPIRED", False, row["stage"], job_id)
    return FileResponse(path, media_type="audio/mpeg", filename="audio.mp3")
