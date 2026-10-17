"""小米 MiMo TTS 适配（mimo-v2.5-tts）。

接口形态：POST /v1/chat/completions，audio 模态。
- 待合成的文本放在 assistant 消息；
- 风格指导（温暖讲故事的语气）放在 user 消息；
- audio.voice 指定音色，audio.format=mp3。
响应 choices[0].message.audio.data 为 base64 MP3（24kHz 单声道 ~64kbps）。

产品逻辑 voice_id（如 warm_female_01）到供应商音色的映射由 config 的
SC_VOICE_MAP 维护，本模块只接受已映射的供应商音色名。
"""
from __future__ import annotations

import base64

from .base import (
    PermanentHTTP,
    ProviderCallFailed,
    RetryableHTTP,
    http_post_json,
    with_retries,
    remaining_timeout,
)

STYLE_PROMPT = "温暖、亲切、讲睡前故事的语气，语速平缓，吐字清晰，适合 3-10 岁儿童收听。"


class MimoTTS:
    def __init__(self, api_key: str, base_url: str, model: str,
                 timeout_s: float, stage_retries: int = 2):
        self.api_key = api_key
        self.base_url = base_url.rstrip("/")
        self.model = model
        self.timeout_s = timeout_s
        self.stage_retries = stage_retries

    def synthesize(self, text: str, voice: str, deadline=None) -> bytes:
        payload = {
            "model": self.model,
            "messages": [
                {"role": "user", "content": STYLE_PROMPT},
                {"role": "assistant", "content": text},
            ],
            "audio": {"voice": voice, "format": "mp3"},
        }

        def call() -> dict:
            return http_post_json(
                f"{self.base_url}/chat/completions",
                {"Authorization": f"Bearer {self.api_key}", "Content-Type": "application/json"},
                payload,
                timeout_s=remaining_timeout(self.timeout_s, deadline),
            )

        try:
            resp = with_retries(
                call, retries=self.stage_retries, base_delay_s=1.0, deadline_monotonic=deadline
            )
        except RetryableHTTP as e:
            raise ProviderCallFailed(str(e)) from e
        except PermanentHTTP as e:
            raise TTSPermanent(str(e)) from e
        except ProviderCallFailed:
            raise

        try:
            audio_b64 = resp["choices"][0]["message"]["audio"]["data"]
        except (KeyError, IndexError, TypeError) as e:
            raise ProviderCallFailed("missing audio in response") from e
        if not audio_b64:
            raise ProviderCallFailed("empty audio in response")
        return base64.b64decode(audio_b64)


class TTSPermanent(Exception):
    """确定性 TTS 失败（不可重试）。"""
