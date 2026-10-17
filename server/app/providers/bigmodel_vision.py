"""智谱 bigmodel 视觉与故事生成适配。

- 理解阶段：glm-4.5v（关闭 thinking，要求只输出 JSON）
- 故事阶段：可配独立文本/视觉模型；默认把原图与画面描述一起给模型，
  保证故事紧扣画面（实施计划 8：Prompt 在服务器维护）。
"""
from __future__ import annotations

from ..errors import JobFailure
from .base import remaining_timeout
from .base import (
    ProviderCallFailed,
    RetryableHTTP,
    PermanentHTTP,
    data_url_jpeg,
    extract_message_content,
    http_post_json,
    parse_json_object,
    story_prompt,
    UNDERSTAND_PROMPT,
    with_retries,
)


class BigmodelVision:
    def __init__(self, api_key: str, base_url: str, vision_model: str, story_model: str,
                 timeout_s: float, stage_retries: int = 2, story_include_image: bool = True):
        self.api_key = api_key
        self.base_url = base_url.rstrip("/")
        self.vision_model = vision_model
        self.story_model = story_model
        self.timeout_s = timeout_s
        self.stage_retries = stage_retries
        self.story_include_image = story_include_image

    # ---- 内部 ----
    def _chat(self, model: str, prompt: str, image_jpeg: bytes | None, deadline=None) -> dict:
        content: list = []
        if image_jpeg is not None:
            content.append({"type": "image_url", "image_url": {"url": data_url_jpeg(image_jpeg)}})
        content.append({"type": "text", "text": prompt})
        payload = {
            "model": model,
            "thinking": {"type": "disabled"},
            "messages": [{"role": "user", "content": content}],
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
            raise JobFailure("AI_FAILED", False, "story_generating") from e
        except ProviderCallFailed:
            raise
        return parse_json_object(extract_message_content(resp))

    # ---- 协议 ----
    def understand(self, image_jpeg: bytes, age_group: str, deadline=None) -> dict:
        obj = self._chat(self.vision_model, UNDERSTAND_PROMPT, image_jpeg, deadline=deadline)
        # SRV-01：suitable 必须是 JSON 布尔值。bool("false") 曾为 True，
        # 把不适宜内容误放行；类型异常按失败处理。
        suitable = obj.get("suitable")
        if not isinstance(suitable, bool):
            raise JobFailure("AI_FAILED", True, "understanding")
        reason = obj.get("reason")
        description = obj.get("description")
        if reason is not None and not isinstance(reason, str):
            raise JobFailure("AI_FAILED", True, "understanding")
        if description is not None and not isinstance(description, str):
            raise JobFailure("AI_FAILED", True, "understanding")
        return {"suitable": suitable,
                "reason": (reason or "")[:200],
                "description": (description or "")[:300]}

    def write_story(self, image_jpeg: bytes, description: str, age_group: str,
                    story_length: str, language: str, deadline=None) -> dict:
        obj = self._chat(
            self.story_model, story_prompt(age_group, story_length, language, description),
            image_jpeg if self.story_include_image else None, deadline=deadline,
        )
        # SRV-01：标题/正文用严格 schema 校验（类型必须为字符串，禁止自由文本透传）
        title = obj.get("title")
        story = obj.get("story")
        if not isinstance(title, str) or not isinstance(story, str):
            raise JobFailure("AI_FAILED", True, "story_generating")
        title = title.strip()
        story = story.strip()
        if not title or not story:
            raise JobFailure("AI_FAILED", True, "story_generating")
        return {"title": title, "story": story}
