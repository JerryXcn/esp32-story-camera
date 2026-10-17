#!/usr/bin/env python3
"""真实供应商端到端冒烟：上传测试绘本照片 → glm-4.5v 理解 → glm-4.6/4.5v 写故事 →
mimo-v2.5-tts 合成 → 下载并校验契约 MP3。

用法（不要把密钥写进代码仓库）：
    SC_BIGMODEL_API_KEY=... SC_MIMO_API_KEY=... \
        python3 scripts/smoke_real.py [图片路径]

默认使用仓库 tests/fixtures/images/picturebook_valid.jpg。
"""
from __future__ import annotations

import hashlib
import os
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "server"))


def main() -> int:
    image_path = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "tests/fixtures/images/picturebook_valid.jpg"
    if not os.environ.get("SC_BIGMODEL_API_KEY") or not os.environ.get("SC_MIMO_API_KEY"):
        print("需要 SC_BIGMODEL_API_KEY 与 SC_MIMO_API_KEY 环境变量", file=sys.stderr)
        return 2

    data_dir = tempfile.mkdtemp(prefix="storycam-smoke-")
    os.environ.setdefault("SC_DATA_DIR", data_dir)
    os.environ.setdefault("SC_DEVICE_TOKENS", "smoke-token:smoke-device")

    from fastapi.testclient import TestClient
    from app.main import create_app

    app = create_app()
    image = image_path.read_bytes()
    op = hashlib.sha256(b"smoke-" + image).hexdigest()[:32]

    t0 = time.time()
    with TestClient(app) as c:
        r = c.post(
            "/v1/jobs",
            data={
                "operation_id": op, "story_id": "a" * 32, "mode": "generate",
                "base_version": "0", "age_group": "5-6", "story_length": "standard",
                "voice_id": "warm_female_01", "language": "zh-CN",
                "image_sha256": hashlib.sha256(image).hexdigest(),
            },
            files={"image": ("page.jpg", image, "image/jpeg")},
            headers={"Authorization": "Bearer smoke-token", "Idempotency-Key": op},
        )
        print("create:", r.status_code, r.json())
        if r.status_code != 202:
            return 1
        job_id = r.json()["job_id"]

        last = None
        while True:
            snap = c.get(f"/v1/jobs/{job_id}", headers={"Authorization": "Bearer smoke-token"}).json()
            cur = (snap["status"], snap["stage"])
            if cur != last:
                print(f"[{time.time()-t0:6.1f}s] {snap['status']} / {snap['stage']} "
                      f"(attempt={snap['attempt']} revision={snap['revision']})")
                last = cur
            if snap["status"] in ("succeeded", "failed", "rejected"):
                break
            time.sleep(1.0)

        if snap["status"] != "succeeded":
            print("最终状态:", snap)
            return 1

        res = c.get(f"/v1/jobs/{job_id}/result", headers={"Authorization": "Bearer smoke-token"}).json()
        print(f"\n标题: {res['title']}")
        print(f"正文({len(res['story'])}字): {res['story'][:120]}{'…' if len(res['story'])>120 else ''}")
        audio = res["audio"]
        print(f"音频: {audio['format']} {audio['sample_rate_hz']}Hz {audio['channels']}ch "
              f"{audio['bit_rate_bps']}bps {audio['duration_ms']/1000:.1f}s {audio['bytes']}B")

        dl = c.get(f"http://test{audio['path']}", headers={"Authorization": "Bearer smoke-token"})
        out = Path(data_dir) / "smoke_story.mp3"
        out.write_bytes(dl.content)
        assert hashlib.sha256(dl.content).hexdigest() == audio["sha256"], "下载 hash 与清单不一致"
        print(f"下载校验通过 -> {out}")
    print(f"端到端耗时 {time.time()-t0:.1f}s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
