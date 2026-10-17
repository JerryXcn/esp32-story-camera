"""有界任务执行：每设备并发 1、全局 worker 上限、单 attempt 120s 截止。

语义对照实施计划：
- 7.5  服务端单 attempt 最长 120 秒；自动重试每阶段最多额外 2 次（1s/3s 退避加抖动）。
- 7.4  重启后无法确认供应商结果的统一标 PROVIDER_RESULT_UNKNOWN，
       等待用户显式重试；不自动重发。
- 8    媒体保留至任务终止后 24h，幂等摘要 7 天；由 CleanupLoop 周期执行。
"""
from __future__ import annotations

import hashlib
import threading
import time
from concurrent.futures import ThreadPoolExecutor

from ..config import Settings
from ..db import Database
from ..errors import JobFailure
from ..providers.base import ProviderCallFailed
from ..providers.factory import Providers
from ..services.audio_spec import finalize_audio
from ..services.media_store import MediaStore
from ..services.moderation import check_story_text

TERMINAL = {"succeeded", "failed", "rejected", "expired"}


def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def iso_ts(epoch: float | None) -> str | None:
    if epoch is None:
        return None
    return datetime_iso(epoch)


def datetime_iso(epoch: float) -> str:
    from datetime import datetime, timezone

    return datetime.fromtimestamp(epoch, tz=timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


class JobExecutor:
    def __init__(self, db: Database, media: MediaStore, providers: Providers, settings: Settings):
        self.db = db
        self.media = media
        self.providers = providers
        self.settings = settings
        self._pool = ThreadPoolExecutor(max_workers=max(1, settings.max_workers),
                                        thread_name_prefix="job-worker")
        self._active_devices: set[str] = set()
        self._lock = threading.Lock()
        self._wake = threading.Event()
        self._stop = threading.Event()
        self._dispatcher: threading.Thread | None = None

    # ---- 生命周期 ----
    def start(self) -> None:
        self.recover_interrupted_jobs()
        self._dispatcher = threading.Thread(target=self._dispatch_loop, name="job-dispatcher", daemon=True)
        self._dispatcher.start()

    def stop(self) -> None:
        self._stop.set()
        self._wake.set()
        self._pool.shutdown(wait=False, cancel_futures=True)

    def notify(self) -> None:
        self._wake.set()

    # ---- 启动恢复（实施计划 7.4 / 8） ----
    def recover_interrupted_jobs(self) -> None:
        now = time.time()
        for row in self.db.recoverable_jobs():
            # 供应商调用结果未知：统一标 failed + PROVIDER_RESULT_UNKNOWN，
            # retryable=false 阻止自动重试；用户显式 retry 走恢复点。
            self.db.update_job(row["job_id"], {
                "status": "failed",
                "error_code": "PROVIDER_RESULT_UNKNOWN",
                "error_retryable": 0,
                "error_stage": row["stage"],
                "finished_at": now,
                "expires_at": now + self.settings.media_ttl_h * 3600,
            })

    # ---- 调度 ----
    def _dispatch_loop(self) -> None:
        while not self._stop.is_set():
            try:
                self._wake.wait(timeout=0.5)
                self._wake.clear()
                self._dispatch_once()
            except Exception:
                if self._stop.is_set():
                    return  # 关闭窗口期数据库可能已释放，安静退出
                time.sleep(0.1)

    def _dispatch_once(self) -> None:
        # SRV-02：有空闲槽才认领（先把任务批量标 running 会让等待队列无界，
        # 线程池工作数不等于队列有界）。
        for row in self.db.query_all("SELECT * FROM jobs WHERE status='queued' ORDER BY created_at"):
            device = row["device_id"]
            with self._lock:
                if device in self._active_devices:
                    continue
                if len(self._active_devices) >= max(1, self.settings.max_workers):
                    break   # 无空闲槽：其余任务留在 queued，下轮唤醒再认领
                self._active_devices.add(device)
            self.db.update_job(row["job_id"], {
                "status": "running",
                "started_at": time.time(),
                "revision": row["revision"] + 1,
                "poll_after_ms": self.settings.poll_after_running_ms,
            })
            try:
                self._pool.submit(self._run_guarded, row["job_id"])
            except RuntimeError:
                with self._lock:
                    self._active_devices.discard(device)
                return

    def retry(self, job_id: str, resume_stage: str) -> None:
        """显式重试：attempt+1、revision 继续增加、阶段回到恢复点（实施计划 7.3/7.4）。"""
        row = self.db.get_job(job_id)
        if row is None:
            return
        self.db.update_job(job_id, {
            "status": "queued",
            "stage": resume_stage,
            "attempt": row["attempt"] + 1,
            "revision": row["revision"] + 1,
            "error_code": None,
            "error_retryable": None,
            "error_stage": None,
            "finished_at": None,
            "expires_at": None,
        })
        self.notify()

    # ---- 管线 ----
    def _run_guarded(self, job_id: str) -> None:
        try:
            self._run_job(job_id)
        except Exception:
            # 防御：管线 bug 不允许静默挂住设备并发槽
            now = time.time()
            row = self.db.get_job(job_id)
            self.db.update_job(job_id, {
                "status": "failed",
                "error_code": "AI_FAILED",
                "error_retryable": 1,
                "error_stage": row["stage"] if row else "queued",
                "finished_at": now,
                "expires_at": now + self.settings.media_ttl_h * 3600,
            })
        finally:
            row = self.db.get_job(job_id)
            if row is not None:
                with self._lock:
                    self._active_devices.discard(row["device_id"])
            self.notify()

    def _set_stage(self, job_id: str, stage: str, revision: int) -> int:
        self.db.update_job(job_id, {"stage": stage, "revision": revision + 1})
        return revision + 1

    def _fail(self, job_id: str, code: str, retryable: bool, stage: str,
              status: str = "failed") -> None:
        now = time.time()
        current = self.db.get_job(job_id)
        self.db.update_job(job_id, {
            "revision": (current["revision"] if current else 0) + 1,
            "status": status,
            "error_code": code,
            "error_retryable": 1 if retryable else 0,
            "error_stage": stage,
            "finished_at": now,
            "expires_at": now + self.settings.media_ttl_h * 3600,
        })

    def _run_job(self, job_id: str) -> None:
        job = self.db.get_job(job_id)
        if job is None or job["status"] != "running":
            return
        deadline = time.monotonic() + self.settings.attempt_deadline_s
        revision = job["revision"]
        try:
            image = self.media.image_path(job_id).read_bytes()

            # 恢复点：正文已持久化（TTS 失败后的重试）则跳过理解与故事阶段
            if not job["story"]:
                revision = self._set_stage(job_id, "understanding", revision)
                understood = self.providers.vision.understand(
                    image, job["age_group"],
                    deadline=min(deadline, time.monotonic() + self.settings.provider_timeout_s))
                if not understood.get("suitable"):
                    # 内容不适宜：停止该次生成，不继续 TTS（产品文档 6.2）
                    self._fail(job_id, "CONTENT_REJECTED", False, "understanding",
                               status="rejected")
                    return

                revision = self._set_stage(job_id, "story_generating", revision)
                drafted = self.providers.vision.write_story(
                    image, understood["description"], job["age_group"],
                    job["story_length"], job["language"],
                    deadline=min(deadline, time.monotonic() + self.settings.provider_timeout_s))
                title, story = check_story_text(
                    drafted["title"], drafted["story"],
                    self.settings.story_max_chars, self.settings.story_max_utf8,
                    self.settings.story_tts_max_chars)
                # 正文先持久化：TTS 失败后 result 仍可返回文本，retry 只做 TTS
                self.db.update_job(job_id, {"title": title, "story": story})
                self.media.save_story(job_id, title, story)
            else:
                title, story = job["title"], job["story"]

            revision = self._set_stage(job_id, "tts_generating", revision)
            voice = self.settings.voice_for(job["voice_id"])
            if not voice:
                raise JobFailure("TTS_FAILED", False, "tts_generating")
            raw = self.providers.tts.synthesize(
                story, voice,
                deadline=min(deadline, time.monotonic() + self.settings.provider_timeout_s))
            final_bytes, spec = finalize_audio(
                raw, self.settings.audio_max_bytes, self.settings.audio_max_duration_ms,
                sha256_fn=sha256_hex, deadline=deadline)
            audio_path = self.media.save_audio(job_id, final_bytes)

            now = time.time()
            self.db.update_job(job_id, {
                "status": "succeeded",
                "stage": "ready",
                "revision": revision + 1,
                "audio_path": str(audio_path),
                "audio_sha256": spec.sha256,
                "audio_bytes": spec.bytes_len,
                "duration_ms": spec.duration_ms,
                "finished_at": now,
                "expires_at": now + self.settings.media_ttl_h * 3600,
            })
        except JobFailure as jf:
            self._fail(job_id, jf.code, jf.retryable, jf.stage,
                       status="rejected" if jf.code == "CONTENT_REJECTED" else "failed")
        except ProviderCallFailed:
            # 阶段相关错误码：视觉/故事 AI_FAILED，TTS TTS_FAILED（均可重试）
            now_stage = self.db.get_job(job_id)["stage"]
            code = "TTS_FAILED" if now_stage == "tts_generating" else "AI_FAILED"
            self._fail(job_id, code, True, now_stage)


class CleanupLoop:
    """TTL 清理：终止 24h 后删媒体并标 expired；摘要 7 天后删除。"""

    def __init__(self, db: Database, media: MediaStore, settings: Settings):
        self.db = db
        self.media = media
        self.settings = settings
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None

    def start(self) -> None:
        self._thread = threading.Thread(target=self._loop, name="ttl-cleaner", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()

    def _loop(self) -> None:
        while not self._stop.wait(timeout=60.0):
            try:
                self.run_once()
            except Exception:
                pass  # 清理失败不阻塞业务；下个周期重试

    def run_once(self) -> None:
        now = time.time()
        digest_ttl_s = self.settings.digest_ttl_d * 86400
        for row in self.db.expired_terminal_jobs(now):
            self.media.delete_job_media(row["job_id"])
            self.db.update_job(row["job_id"], {"status": "expired"})
        for row in self.db.purgeable_jobs(now, digest_ttl_s):
            self.media.delete_job_media(row["job_id"])
            self.db.execute("DELETE FROM jobs WHERE job_id = ?", (row["job_id"],))
            self.db.execute(
                "DELETE FROM idempotency WHERE device_id = ? AND operation_id = ?",
                (row["device_id"], row["operation_id"]))
        self.db.execute(
            "DELETE FROM idempotency WHERE created_at + ? <= ?", (digest_ttl_s, now))
        self.db.execute(
            "DELETE FROM recovery_commands WHERE created_at + ? <= ?", (digest_ttl_s, now))
