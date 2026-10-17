"""图片校验（实施计划 7.2）：

- 必须 JPEG；≤1 MiB；解码后长边 ≤1280；总像素 ≤1280×1280。
- SHA-256 与请求字段一致（服务端重算验证）。
任何失败都属于 IMAGE_INVALID，阻止进入生成管线。
"""
from __future__ import annotations

import hashlib
import io

from PIL import Image, UnidentifiedImageError

from ..errors import ApiError


def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def validate_image(data: bytes, claimed_sha256: str, max_bytes: int, max_edge: int) -> str:
    """校验通过返回实际 sha256；失败抛 ApiError(422, IMAGE_INVALID)。"""
    if len(data) == 0:
        raise ApiError(422, "IMAGE_INVALID", False, "queued")
    if len(data) > max_bytes:
        raise ApiError(413, "IMAGE_INVALID", False, "queued")
    if claimed_sha256 and claimed_sha256.lower() != sha256_hex(data):
        raise ApiError(422, "IMAGE_INVALID", False, "queued")
    try:
        img = Image.open(io.BytesIO(data))
        img.load()
    except (UnidentifiedImageError, OSError, ValueError):
        raise ApiError(422, "IMAGE_INVALID", False, "queued")
    if img.format != "JPEG":
        raise ApiError(422, "IMAGE_INVALID", False, "queued")
    w, h = img.size
    if max(w, h) > max_edge or w * h > max_edge * max_edge:
        raise ApiError(422, "IMAGE_INVALID", False, "queued")
    return sha256_hex(data)
