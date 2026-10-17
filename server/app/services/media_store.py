"""任务媒体文件布局与生命周期。

    DATA_DIR/media/{job_id}/image.jpg    上传图（原样保存，校验后落盘）
    DATA_DIR/media/{job_id}/story.txt    通过检查的故事文本
    DATA_DIR/media/{job_id}/audio.mp3    契约 MP3（mono/24kHz/64kbps CBR）

保留策略：任务终止 24h 后由清理器删除文件；任务行再保留 digest_ttl 后清除。
路径只由本模块按 job_id 构造，job_id 由服务端生成，绝不使用
客户端提供的任何字符串拼接路径。
"""
from __future__ import annotations

import shutil
from pathlib import Path


class MediaStore:
    def __init__(self, root: str | Path):
        self.root = Path(root)
        (self.root / "media").mkdir(parents=True, exist_ok=True)

    def job_dir(self, job_id: str) -> Path:
        d = self.root / "media" / job_id
        d.mkdir(parents=True, exist_ok=True)
        return d

    def image_path(self, job_id: str) -> Path:
        return self.job_dir(job_id) / "image.jpg"

    def story_path(self, job_id: str) -> Path:
        return self.job_dir(job_id) / "story.txt"

    def audio_path(self, job_id: str) -> Path:
        return self.job_dir(job_id) / "audio.mp3"

    def save_image(self, job_id: str, data: bytes) -> Path:
        p = self.image_path(job_id)
        p.write_bytes(data)
        return p

    def save_story(self, job_id: str, title: str, story: str) -> Path:
        p = self.story_path(job_id)
        p.write_text(f"{title}\n\n{story}", encoding="utf-8")
        return p

    def save_audio(self, job_id: str, data: bytes) -> Path:
        p = self.audio_path(job_id)
        p.write_bytes(data)
        return p

    def delete_job_media(self, job_id: str) -> None:
        d = self.root / "media" / job_id
        if d.exists():
            shutil.rmtree(d, ignore_errors=True)
