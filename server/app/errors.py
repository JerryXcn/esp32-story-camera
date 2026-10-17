"""统一错误信封（实施计划 7.1 / 10）：

    { "error": { "code": "...", "retryable": bool, "stage": "...", "job_id": ... } }

错误码尽量取自第 10 节业务码表；少数服务端必须表达、
表中没有的码（如 RESULT_NOT_READY / STALE_ATTEMPT / AUDIO_TOO_LONG）
作为扩展码在 docs/api-contract-v1.md 中单独列出。
"""
from __future__ import annotations

RETRYABLE_TRUE = True
RETRYABLE_FALSE = False

# 可自动重试的业务码（设备端自动退避重试）；PROVIDER_RESULT_UNKNOWN
# 与 retryable=false 的码只允许用户显式重试。
AUTO_RETRYABLE_CODES = {"AI_FAILED", "TTS_FAILED", "NETWORK_TIMEOUT"}


class ApiError(Exception):
    """直接映射为一个 HTTP 错误响应。"""

    def __init__(self, status: int, code: str, retryable: bool, stage: str | None, job_id: str | None = None):
        self.status = status
        self.code = code
        self.retryable = retryable
        self.stage = stage
        self.job_id = job_id
        super().__init__(f"{code} at {stage}: status={status}")

    def payload(self) -> dict:
        return {
            "error": {
                "code": self.code,
                "retryable": self.retryable,
                "stage": self.stage,
                "job_id": self.job_id,
            }
        }


class JobFailure(Exception):
    """worker 管线内部失败：写入任务状态而非直接抛给 HTTP 层。"""

    def __init__(self, code: str, retryable: bool, stage: str):
        self.code = code
        self.retryable = retryable
        self.stage = stage
        super().__init__(code)

    def payload(self, job_id: str | None) -> dict:
        return {
            "error": {
                "code": self.code,
                "retryable": self.retryable,
                "stage": self.stage,
                "job_id": job_id,
            }
        }


def error_body(code: str, retryable: bool, stage: str | None, job_id: str | None = None) -> dict:
    return {"error": {"code": code, "retryable": retryable, "stage": stage, "job_id": job_id}}
