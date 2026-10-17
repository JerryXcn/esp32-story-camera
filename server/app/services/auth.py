"""设备鉴权：Bearer token -> device_id（实施计划 7.1：后端从令牌确定身份，
不相信请求体传入的 device_id）。令牌只存 SHA-256。"""
from __future__ import annotations

import hashlib

from fastapi import Request

from ..errors import ApiError


def token_sha256(token: str) -> str:
    return hashlib.sha256(token.encode("utf-8")).hexdigest()


def require_device(request: Request) -> str:
    auth = request.headers.get("authorization", "")
    if not auth.lower().startswith("bearer "):
        raise ApiError(401, "AUTH_FAILED", False, None)
    token = auth[7:].strip()
    device_id = request.app.state.db.device_for_token(token_sha256(token))
    if device_id is None:
        raise ApiError(401, "AUTH_FAILED", False, None)
    return device_id
