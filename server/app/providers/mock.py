"""确定性 mock 供应商：测试与离线开发用，不访问网络。

- MockVision：按图片字节哈希决定 suitable；标题/正文确定性生成，
  便于幂等与状态机测试断言。
- MockTTS：返回内嵌的契约规格 MP3（见 mock_audio_b64.py）。
可注入故障：`fail_stages` 集合里可放 "understanding"/"story_generating"/
"tts_generating"，用于 TTS_FAILED 等状态机用例。
"""
from __future__ import annotations

import hashlib

from ..errors import JobFailure
from .mock_audio_b64 import _MOCK_MP3_B64

import base64


def _h(text: str, modulus: int) -> int:
    return int(hashlib.sha256(text.encode()).hexdigest(), 16) % modulus


class MockVision:
    def __init__(self, fail_stages: set[str] | None = None, reject_hashes: set[str] | None = None):
        self.fail_stages = fail_stages or set()
        self.reject_hashes = reject_hashes or set()

    def understand(self, image_jpeg: bytes, age_group: str, deadline=None) -> dict:
        if "understanding" in self.fail_stages:
            raise JobFailure("AI_FAILED", True, "understanding")
        digest = hashlib.sha256(image_jpeg).hexdigest()
        if digest in self.reject_hashes:
            return {"suitable": False, "reason": "画面不适合儿童", "description": ""}
        animals = ["小兔子", "小熊", "小狐狸", "小象", "小刺猬"]
        places = ["森林小路", "开满花的草地", "月亮下的山坡", "小溪边", "纸箱里"]
        a = animals[_h(digest, len(animals))]
        p = places[_h(digest + "p", len(places))]
        return {"suitable": True, "reason": "", "description": f"{a}在{p}上开心地玩耍"}

    def write_story(self, image_jpeg: bytes, description: str, age_group: str,
                    story_length: str, language: str, deadline=None) -> dict:
        if "story_generating" in self.fail_stages:
            raise JobFailure("AI_FAILED", True, "story_generating")
        digest = hashlib.sha256(image_jpeg).hexdigest()[:8]
        title = f"测试故事{digest[:4]}"
        story = (
            f"{description}。小故事{digest[:4]}讲的是：太阳暖暖的，风轻轻的，"
            "小伙伴们一起做游戏，分享点心，还帮助了需要帮助的朋友。"
            "天黑之前，大家约好明天再见，然后各自回家，带着满满的快乐睡着了。"
        )
        return {"title": title, "story": story}


class MockTTS:
    def __init__(self, fail_stages: set[str] | None = None):
        self.fail_stages = fail_stages or set()

    def synthesize(self, text: str, voice: str, deadline=None) -> bytes:
        if "tts_generating" in self.fail_stages:
            raise JobFailure("TTS_FAILED", True, "tts_generating")
        return base64.b64decode(_MOCK_MP3_B64)
