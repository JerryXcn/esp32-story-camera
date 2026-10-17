"""SQLite 持久层：jobs、幂等摘要、恢复命令、设备、额度。

单写者模型：所有访问经过进程内锁；WAL 模式提升读并发。
绝不在此层记录照片、正文、密码或令牌明文。
"""
from __future__ import annotations

import json
import sqlite3
import threading
import time
import uuid
from pathlib import Path

_SCHEMA = """
CREATE TABLE IF NOT EXISTS jobs (
  job_id         TEXT PRIMARY KEY,
  operation_id   TEXT NOT NULL,
  device_id      TEXT NOT NULL,
  story_id       TEXT NOT NULL,
  mode           TEXT NOT NULL,
  base_version   INTEGER NOT NULL,
  age_group      TEXT NOT NULL,
  story_length   TEXT NOT NULL,
  voice_id       TEXT NOT NULL,
  language       TEXT NOT NULL,
  image_path     TEXT,
  image_sha256   TEXT NOT NULL,
  status         TEXT NOT NULL,
  stage          TEXT NOT NULL,
  attempt        INTEGER NOT NULL DEFAULT 1,
  revision       INTEGER NOT NULL DEFAULT 1,
  poll_after_ms  INTEGER NOT NULL DEFAULT 2000,
  error_code     TEXT,
  error_retryable INTEGER,
  error_stage    TEXT,
  title          TEXT,
  story          TEXT,
  audio_path     TEXT,
  audio_sha256   TEXT,
  audio_bytes    INTEGER,
  duration_ms    INTEGER,
  created_at     REAL NOT NULL,
  started_at     REAL,
  finished_at    REAL,
  expires_at     REAL,
  UNIQUE (device_id, operation_id)
);
CREATE INDEX IF NOT EXISTS idx_jobs_device_status ON jobs (device_id, status);

CREATE TABLE IF NOT EXISTS idempotency (
  device_id       TEXT NOT NULL,
  operation_id    TEXT NOT NULL,
  request_digest  TEXT NOT NULL,
  job_id          TEXT NOT NULL,
  created_at      REAL NOT NULL,
  PRIMARY KEY (device_id, operation_id)
);

CREATE TABLE IF NOT EXISTS recovery_commands (
  device_id    TEXT NOT NULL,
  command_key  TEXT NOT NULL,
  job_id       TEXT NOT NULL,
  created_at   REAL NOT NULL,
  PRIMARY KEY (device_id, command_key)
);

CREATE TABLE IF NOT EXISTS quota_usage (
  device_id  TEXT NOT NULL,
  day        TEXT NOT NULL,
  count      INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (device_id, day)
);

CREATE TABLE IF NOT EXISTS devices (
  token_sha256 TEXT PRIMARY KEY,
  device_id    TEXT NOT NULL UNIQUE,
  created_at   REAL NOT NULL
);
"""


def utcnow() -> float:
    return time.time()


def new_job_id() -> str:
    return "job_" + uuid.uuid4().hex


def canonical_digest(parts: list[str]) -> str:
    """确定性摘要：固定分隔拼接后 SHA-256。参与字段不含任何密钥。"""
    import hashlib

    h = hashlib.sha256()
    for p in parts:
        h.update(p.encode("utf-8"))
        h.update(b"\x1f")
    return h.hexdigest()


class Database:
    def __init__(self, path: str | Path):
        self.path = str(path)
        Path(self.path).parent.mkdir(parents=True, exist_ok=True)
        self._lock = threading.RLock()
        self._closed = False
        self._conn = sqlite3.connect(self.path, check_same_thread=False)
        self._conn.row_factory = sqlite3.Row
        with self._lock:
            self._conn.execute("PRAGMA journal_mode=WAL")
            self._conn.execute("PRAGMA synchronous=NORMAL")
            self._conn.executescript(_SCHEMA)
            self._conn.commit()

    def close(self) -> None:
        with self._lock:
            self._closed = True
            self._conn.close()

    # ---- 基础执行（关闭后安静降级：后台线程可能在关闭窗口期访问） ----
    def execute(self, sql: str, params: tuple = ()) -> None:
        if self._closed:
            return
        with self._lock:
            self._conn.execute(sql, params)
            self._conn.commit()

    def query_one(self, sql: str, params: tuple = ()) -> sqlite3.Row | None:
        if self._closed:
            return None
        with self._lock:
            return self._conn.execute(sql, params).fetchone()

    def query_all(self, sql: str, params: tuple = ()) -> list[sqlite3.Row]:
        if self._closed:
            return []
        with self._lock:
            return self._conn.execute(sql, params).fetchall()

    # ---- 设备 ----
    def register_device(self, token_sha256: str, device_id: str) -> None:
        self.execute(
            "INSERT OR REPLACE INTO devices (token_sha256, device_id, created_at) VALUES (?,?,?)",
            (token_sha256, device_id, utcnow()),
        )

    def device_for_token(self, token_sha256: str) -> str | None:
        row = self.query_one("SELECT device_id FROM devices WHERE token_sha256 = ?", (token_sha256,))
        return row["device_id"] if row else None

    # ---- 任务 ----
    def insert_job(self, job: dict) -> None:
        cols = ",".join(job.keys())
        marks = ",".join("?" for _ in job)
        self.execute(f"INSERT INTO jobs ({cols}) VALUES ({marks})", tuple(job.values()))

    def get_job(self, job_id: str) -> sqlite3.Row | None:
        return self.query_one("SELECT * FROM jobs WHERE job_id = ?", (job_id,))

    def get_job_by_operation(self, device_id: str, operation_id: str) -> sqlite3.Row | None:
        return self.query_one(
            "SELECT * FROM jobs WHERE device_id = ? AND operation_id = ?", (device_id, operation_id)
        )

    def update_job(self, job_id: str, fields: dict) -> None:
        if not fields:
            return
        sets = ",".join(f"{k} = ?" for k in fields)
        self.execute(f"UPDATE jobs SET {sets} WHERE job_id = ?", (*fields.values(), job_id))

    def running_job_for_device(self, device_id: str) -> sqlite3.Row | None:
        return self.query_one(
            "SELECT * FROM jobs WHERE device_id = ? AND status IN ('queued','running') ORDER BY created_at LIMIT 1",
            (device_id,),
        )

    def recoverable_jobs(self) -> list[sqlite3.Row]:
        return self.query_all("SELECT * FROM jobs WHERE status IN ('queued','running')")

    def expired_terminal_jobs(self, now: float) -> list[sqlite3.Row]:
        """已到期但尚未标记 expired 的终止态任务。"""
        return self.query_all(
            "SELECT * FROM jobs WHERE status IN ('succeeded','failed','rejected') AND expires_at IS NOT NULL AND expires_at <= ?",
            (now,),
        )

    def purgeable_jobs(self, now: float, digest_ttl_s: float) -> list[sqlite3.Row]:
        """媒体已清理且摘要也过期的任务行。"""
        return self.query_all(
            "SELECT * FROM jobs WHERE status = 'expired' AND expires_at IS NOT NULL AND expires_at + ? <= ?",
            (digest_ttl_s, now),
        )

    # ---- 幂等 ----
    def get_idempotency(self, device_id: str, operation_id: str) -> sqlite3.Row | None:
        return self.query_one(
            "SELECT * FROM idempotency WHERE device_id = ? AND operation_id = ?", (device_id, operation_id)
        )

    def put_idempotency(self, device_id: str, operation_id: str, digest: str, job_id: str) -> None:
        self.execute(
            "INSERT OR REPLACE INTO idempotency (device_id, operation_id, request_digest, job_id, created_at) VALUES (?,?,?,?,?)",
            (device_id, operation_id, digest, job_id, utcnow()),
        )

    # ---- 恢复命令（retry 幂等） ----
    def get_recovery(self, device_id: str, command_key: str) -> sqlite3.Row | None:
        return self.query_one(
            "SELECT * FROM recovery_commands WHERE device_id = ? AND command_key = ?", (device_id, command_key)
        )

    def put_recovery(self, device_id: str, command_key: str, job_id: str) -> None:
        self.execute(
            "INSERT OR REPLACE INTO recovery_commands (device_id, command_key, job_id, created_at) VALUES (?,?,?,?)",
            (device_id, command_key, job_id, utcnow()),
        )

    # ---- 额度 ----
    def bump_quota(self, device_id: str, day: str) -> int:
        if self._closed:
            return 0
        with self._lock:
            row = self._conn.execute(
                "SELECT count FROM quota_usage WHERE device_id = ? AND day = ?", (device_id, day)
            ).fetchone()
            new_count = (row["count"] if row else 0) + 1
            self._conn.execute(
                "INSERT INTO quota_usage (device_id, day, count) VALUES (?,?,?) "
                "ON CONFLICT (device_id, day) DO UPDATE SET count = ?",
                (device_id, day, new_count, new_count),
            )
            self._conn.commit()
            return new_count

    def quota_count(self, device_id: str, day: str) -> int:
        row = self.query_one(
            "SELECT count FROM quota_usage WHERE device_id = ? AND day = ?", (device_id, day)
        )
        return row["count"] if row else 0


def job_to_dict(row: sqlite3.Row) -> dict:
    return {k: row[k] for k in row.keys()}


def dumps(obj) -> str:
    return json.dumps(obj, ensure_ascii=False, sort_keys=True)
