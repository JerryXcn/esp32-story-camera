#!/usr/bin/env python3
"""生成共享测试素材（实施计划 2.2：tests/fixtures/）。

- 合成“绘本风格”测试照片（非真实绘本，仅用于管线测试）
- 坏文件：截断 JPEG、空文件、非 JPEG、超大图、超大尺寸
- 契约响应样本（docs 与契约测试引用）

仅依赖 Pillow。重复运行会覆盖同名输出。
"""
from __future__ import annotations

import io
import json
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent.parent
FIX = ROOT / "tests" / "fixtures"
IMG = FIX / "images"
RESP = FIX / "responses"


def make_picturebook(path: Path, size=(900, 675)) -> None:
    """合成一张色彩明亮的“绘本页面”：天空、草地、太阳、动物形状。"""
    w, h = size
    img = Image.new("RGB", size, (255, 246, 220))
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, w, h * 2 // 3], fill=(168, 219, 245))          # 天空
    d.rectangle([0, h * 2 // 3, w, h], fill=(150, 205, 130))          # 草地
    d.ellipse([w - 140, 30, w - 40, 130], fill=(255, 214, 90))        # 太阳
    d.ellipse([40, 40, 130, 90], fill=(255, 255, 255))                # 云
    d.ellipse([90, 60, 180, 110], fill=(255, 255, 255))
    # 小熊（棕）
    bx, by = w * 0.28, h * 0.62
    d.ellipse([bx - 55, by - 55, bx + 55, by + 55], fill=(184, 125, 78))
    d.ellipse([bx - 45, by - 105, bx - 15, by - 75], fill=(184, 125, 78))
    d.ellipse([bx + 15, by - 105, bx + 45, by - 75], fill=(184, 125, 78))
    d.ellipse([bx - 22, by - 30, bx - 8, by - 16], fill=(60, 40, 30))
    d.ellipse([bx + 8, by - 30, bx + 22, by - 16], fill=(60, 40, 30))
    d.ellipse([bx - 14, by - 6, bx + 14, by + 18], fill=(240, 224, 200))
    # 小兔（白）
    rx, ry = w * 0.66, h * 0.66
    d.ellipse([rx - 45, ry - 45, rx + 45, ry + 45], fill=(250, 248, 244))
    d.ellipse([rx - 30, ry - 110, rx - 10, ry - 50], fill=(250, 248, 244))
    d.ellipse([rx + 10, ry - 110, rx + 30, ry - 50], fill=(250, 248, 244))
    d.ellipse([rx - 18, ry - 26, rx - 6, ry - 14], fill=(40, 35, 35))
    d.ellipse([rx + 6, ry - 26, rx + 18, ry - 14], fill=(40, 35, 35))
    # 花
    for i, fx in enumerate(range(30, w - 20, 90)):
        fy = h - 34 + (i % 2) * 10
        d.ellipse([fx, fy, fx + 16, fy + 16], fill=(240, 120, 130))
        d.ellipse([fx + 5, fy + 5, fx + 11, fy + 11], fill=(255, 220, 120))
    img.save(path, "JPEG", quality=88)


def make_oversized(path: Path, target_bytes=1_300_000) -> None:
    """噪声图，保证 JPEG 压缩后仍超过 1 MiB。"""
    import random

    random.seed(7)
    img = Image.new("RGB", (1600, 1200))
    img.putdata([(random.randrange(256), random.randrange(256), random.randrange(256))
                 for _ in range(1600 * 1200)])
    buf = io.BytesIO()
    img.save(buf, "JPEG", quality=100, subsampling=0)
    path.write_bytes(buf.getvalue())


def make_oversize_dimensions(path: Path) -> None:
    """长边超过 1280 的合法 JPEG。"""
    img = Image.new("RGB", (1400, 1050), (120, 180, 220))
    img.save(path, "JPEG", quality=85)


def make_truncated(path: Path, src: Path) -> None:
    data = src.read_bytes()
    path.write_bytes(data[: len(data) // 2])


def make_empty(path: Path) -> None:
    path.write_bytes(b"")


def make_not_jpeg(path: Path) -> None:
    Image.new("RGB", (320, 240), (200, 120, 90)).save(path, "PNG")


def write_response_samples() -> None:
    """契约响应样本（与 docs/api-contract-v1.md 保持一致；hash 为占位，
    联调 fixture 必须换成真实文件 hash）。"""
    samples = {
        "create_202.json": {
            "schema_version": 1,
            "operation_id": "22222222222222222222222222222222",
            "job_id": "job_example_001",
            "status": "queued",
            "stage": "queued",
            "attempt": 1,
            "revision": 1,
            "poll_after_ms": 2000,
            "error": None,
            "expires_at": None,
        },
        "status_running.json": {
            "schema_version": 1,
            "job_id": "job_example_001",
            "operation_id": "22222222222222222222222222222222",
            "status": "running",
            "stage": "story_generating",
            "attempt": 1,
            "revision": 3,
            "poll_after_ms": 2000,
            "error": None,
            "expires_at": None,
        },
        "status_failed_tts.json": {
            "schema_version": 1,
            "job_id": "job_example_001",
            "operation_id": "22222222222222222222222222222222",
            "status": "failed",
            "stage": "tts_generating",
            "attempt": 1,
            "revision": 4,
            "poll_after_ms": 2000,
            "error": {"code": "TTS_FAILED", "retryable": True,
                      "stage": "tts_generating", "job_id": "job_example_001"},
            "expires_at": "2026-09-13T08:00:00Z",
        },
        "result_ready.json": {
            "schema_version": 1,
            "job_id": "job_example_001",
            "operation_id": "22222222222222222222222222222222",
            "title": "勇敢的小兔子",
            "story": "小兔子沿着森林小路，遇到了正在找朋友的小熊。",
            "audio": {
                "path": "/v1/jobs/job_example_001/audio",
                "format": "mp3",
                "sample_rate_hz": 24000,
                "channels": 1,
                "bit_rate_bps": 64000,
                "duration_ms": 118000,
                "bytes": 944000,
                "sha256": "0000000000000000000000000000000000000000000000000000000000000000",
            },
            "expires_at": "2026-09-13T08:00:00Z",
        },
        "error_idempotency_conflict.json": {
            "error": {"code": "IDEMPOTENCY_CONFLICT", "retryable": False,
                      "stage": None, "job_id": "job_example_001"},
        },
        "error_content_rejected.json": {
            "error": {"code": "CONTENT_REJECTED", "retryable": False,
                      "stage": "understanding", "job_id": "job_example_001"},
        },
        "error_provider_unknown.json": {
            "error": {"code": "PROVIDER_RESULT_UNKNOWN", "retryable": False,
                      "stage": "tts_generating", "job_id": "job_example_001"},
        },
    }
    for name, payload in samples.items():
        (RESP / name).write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n",
                                 encoding="utf-8")


def main() -> None:
    IMG.mkdir(parents=True, exist_ok=True)
    RESP.mkdir(parents=True, exist_ok=True)
    make_picturebook(IMG / "picturebook_valid.jpg")
    make_truncated(IMG / "truncated.jpg", IMG / "picturebook_valid.jpg")
    make_empty(IMG / "empty.jpg")
    make_not_jpeg(IMG / "not_jpeg.jpg")
    make_oversize_dimensions(IMG / "oversize_dimensions.jpg")
    make_oversized(IMG / "oversized.jpg")
    write_response_samples()
    print(f"fixtures written under {FIX}")


if __name__ == "__main__":
    main()
