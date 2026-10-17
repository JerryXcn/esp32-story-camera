"""按配置装配供应商实例。密钥只在服务端；mock 模式用于测试/离线开发。"""
from __future__ import annotations

from ..config import Settings
from .bigmodel_vision import BigmodelVision
from .mimo_tts import MimoTTS
from .mock import MockTTS, MockVision


class Providers:
    def __init__(self, vision, tts, mode: str):
        self.vision = vision
        self.tts = tts
        self.mode = mode


def build_providers(s: Settings) -> Providers:
    if s.provider_mode == "mock":
        return Providers(MockVision(), MockTTS(), "mock")
    return Providers(
        vision=BigmodelVision(
            api_key=s.bigmodel_api_key,
            base_url=s.bigmodel_base_url,
            vision_model=s.bigmodel_vision_model,
            story_model=s.bigmodel_story_model,
            timeout_s=s.provider_timeout_s,
            stage_retries=s.provider_stage_retries,
            story_include_image=s.bigmodel_story_include_image,
        ),
        tts=MimoTTS(
            api_key=s.mimo_api_key,
            base_url=s.mimo_base_url,
            model=s.mimo_model,
            timeout_s=s.provider_timeout_s,
            stage_retries=s.provider_stage_retries,
        ),
        mode="real",
    )
