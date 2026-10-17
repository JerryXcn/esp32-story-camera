"""运行配置。全部来自环境变量，密钥不进入代码与日志。"""
from __future__ import annotations

import os
from dataclasses import dataclass, field


def _env(key: str, default: str) -> str:
    return os.environ.get(key, default)


def _env_int(key: str, default: int) -> int:
    try:
        return int(os.environ.get(key, str(default)))
    except ValueError:
        return default


@dataclass
class Settings:
    # 存储根目录：SQLite 与任务媒体文件都放在这里
    data_dir: str = field(default_factory=lambda: _env("SC_DATA_DIR", os.path.join(os.getcwd(), "data")))

    # 设备令牌："token:device_id,token:device_id"。仅 SHA-256 落库。
    device_tokens_raw: str = field(default_factory=lambda: _env("SC_DEVICE_TOKENS", "dev-token-001:dev-device-001"))

    # real = 调用真实供应商；mock = 确定性假实现（测试/离线开发）
    provider_mode: str = field(default_factory=lambda: _env("SC_PROVIDER_MODE", "real"))

    bigmodel_api_key: str = field(default_factory=lambda: _env("SC_BIGMODEL_API_KEY", ""))
    bigmodel_base_url: str = field(default_factory=lambda: _env("SC_BIGMODEL_BASE_URL", "https://open.bigmodel.cn/api/paas/v4"))
    bigmodel_vision_model: str = field(default_factory=lambda: _env("SC_BIGMODEL_VISION_MODEL", "glm-4.5v"))
    # 故事阶段默认也用视觉模型（图文同源，保证紧扣画面）；若换成纯文本模型，
    # 必须同时设 SC_BIGMODEL_STORY_INCLUDE_IMAGE=0
    bigmodel_story_model: str = field(default_factory=lambda: _env("SC_BIGMODEL_STORY_MODEL", "glm-4.5v"))
    bigmodel_story_include_image: bool = field(default_factory=lambda: _env("SC_BIGMODEL_STORY_INCLUDE_IMAGE", "1") not in ("0", "false", "no"))

    mimo_api_key: str = field(default_factory=lambda: _env("SC_MIMO_API_KEY", ""))
    mimo_base_url: str = field(default_factory=lambda: _env("SC_MIMO_BASE_URL", "https://api.xiaomimimo.com/v1"))
    mimo_model: str = field(default_factory=lambda: _env("SC_MIMO_MODEL", "mimo-v2.5-tts"))
    # 产品逻辑 voice_id -> 供应商 voice 映射，如 "warm_female_01:mimo_default"
    voice_map_raw: str = field(default_factory=lambda: _env("SC_VOICE_MAP", "warm_female_01:mimo_default"))

    # 契约常量（实施计划 5.1 / 7.x / 8）
    image_max_bytes: int = field(default_factory=lambda: _env_int("SC_IMAGE_MAX_BYTES", 1024 * 1024))
    multipart_max_bytes: int = field(default_factory=lambda: _env_int("SC_MULTIPART_MAX_BYTES", 1024 * 1024 + 16 * 1024))
    image_max_edge: int = field(default_factory=lambda: _env_int("SC_IMAGE_MAX_EDGE", 1280))
    audio_max_bytes: int = field(default_factory=lambda: _env_int("SC_AUDIO_MAX_BYTES", 2 * 1024 * 1024))
    audio_max_duration_ms: int = field(default_factory=lambda: _env_int("SC_AUDIO_MAX_DURATION_MS", 240 * 1000))
    story_max_chars: int = field(default_factory=lambda: _env_int("SC_STORY_MAX_CHARS", 3000))
    # 朗读正文上限：最慢实测语速 3.9 字/秒下 900 字 ≈ 231 s，留在 240 s 音频上限内
    story_tts_max_chars: int = field(default_factory=lambda: _env_int("SC_STORY_TTS_MAX_CHARS", 900))
    story_max_utf8: int = field(default_factory=lambda: _env_int("SC_STORY_MAX_UTF8", 12 * 1024))

    # 每个 attempt 的服务端总时长上限（实施计划 7.5）
    attempt_deadline_s: int = field(default_factory=lambda: _env_int("SC_ATTEMPT_DEADLINE_S", 120))
    # 供应商调用超时与自动重试（每阶段最多额外 2 次，1s/3s 退避）
    provider_timeout_s: int = field(default_factory=lambda: _env_int("SC_PROVIDER_TIMEOUT_S", 60))
    provider_stage_retries: int = field(default_factory=lambda: _env_int("SC_PROVIDER_STAGE_RETRIES", 2))

    # 全局并发 worker 数；每设备并发固定为 1（实施计划 7.4 / 8）
    max_workers: int = field(default_factory=lambda: _env_int("SC_MAX_WORKERS", 2))

    # 每设备每日生成额度（耗尽返回 429 QUOTA_EXCEEDED，不影响离线播放）
    quota_per_device_daily: int = field(default_factory=lambda: _env_int("SC_QUOTA_PER_DEVICE_DAILY", 50))

    # 媒体保留：任务终止后 24h；幂等摘要保留 7 天（实施计划 8）
    media_ttl_h: int = field(default_factory=lambda: _env_int("SC_MEDIA_TTL_H", 24))
    digest_ttl_d: int = field(default_factory=lambda: _env_int("SC_DIGEST_TTL_D", 7))

    # 状态轮询建议（实施计划 7.5：2 秒起步，慢任务 5 秒）
    poll_after_running_ms: int = field(default_factory=lambda: _env_int("SC_POLL_AFTER_RUNNING_MS", 2000))

    def voice_for(self, voice_id: str) -> str | None:
        mapping = {}
        for pair in self.voice_map_raw.split(","):
            if ":" in pair:
                k, v = pair.split(":", 1)
                mapping[k.strip()] = v.strip()
        return mapping.get(voice_id)


_settings: Settings | None = None


def get_settings() -> Settings:
    global _settings
    if _settings is None:
        _settings = Settings()
    return _settings


def reset_settings_for_tests() -> None:
    global _settings
    _settings = None
