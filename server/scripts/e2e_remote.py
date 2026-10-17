#!/usr/bin/env python3
"""对已部署的后端走一遍真实生成：提交 → 轮询 → 结果 → 音频下载与校验。
用法：SC_DEV_TOKEN=... python3 server/scripts/e2e_remote.py https://host:port [图片]
（令牌即设备令牌，见 server/dev-token.env；图片默认 tests/fixtures/images/picturebook_valid.jpg）"""
import hashlib, os, sys, time, uuid
from pathlib import Path
import httpx

base = sys.argv[1].rstrip("/")
img = Path(sys.argv[2] if len(sys.argv) > 2 else
           Path(__file__).resolve().parents[2] / "tests/fixtures/images/picturebook_valid.jpg").read_bytes()
auth = {"Authorization": f"Bearer {os.environ['SC_DEV_TOKEN']}"}
op = uuid.uuid4().hex
t0 = time.time()
with httpx.Client(timeout=60) as c:
    r = c.post(f"{base}/v1/jobs", headers={**auth, "Idempotency-Key": op},
               data={"operation_id": op, "story_id": uuid.uuid4().hex, "mode": "generate", "base_version": "0",
                     "image_sha256": hashlib.sha256(img).hexdigest(), "age_group": "5-6",
                     "story_length": "short", "voice_id": "warm_female_01", "language": "zh-CN"},
               files={"image": ("page.jpg", img, "image/jpeg")})
    print("POST", r.status_code, r.json().get("status"))
    job = r.json()["job_id"]
    while True:
        j = c.get(f"{base}/v1/jobs/{job}", headers=auth).json()
        if j["status"] not in ("queued", "running"):
            break
        time.sleep(2)
    print("job", j["status"], f"{time.time() - t0:.1f}s", j.get("error_code") or "")
    if j["status"] != "succeeded":
        sys.exit(1)
    res = c.get(f"{base}/v1/jobs/{job}/result", headers=auth).json()
    print("title:", res["title"], "| chars:", len(res["story"]), "| duration_ms:", res["audio"]["duration_ms"])
    t1 = time.time()
    a = c.get(f"{base}/v1/jobs/{job}/audio", headers=auth)
    ok = hashlib.sha256(a.content).hexdigest() == res["audio"]["sha256"]
    print("audio", a.status_code, len(a.content), "bytes", f"{time.time() - t1:.1f}s", "sha ok" if ok else "SHA MISMATCH")
    sys.exit(0 if ok else 1)
