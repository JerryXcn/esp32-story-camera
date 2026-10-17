"""内容检查（实施计划 8：结构校验与内容复核）。

第一层在视觉理解输出 suitable=false 时拒绝（CONTENT_REJECTED）；
本模块是第二层文本复核：关键词黑名单 + 长度/字符校验。
黑名单保持小而明确，放在独立常量里便于评审与扩展；
未命中黑名单不等于内容保证适龄，正式放行标准见产品文档第 11 节内容评估。
"""
from __future__ import annotations

import re

from ..errors import JobFailure
from .charset import display_safe_title

# 仅作机器初筛的示例黑名单，按内容评审结果维护。
_BLOCKLIST = [
    "色情", "暴力", "赌博", "自杀", "毒品", "枪战", "血腥",
    "杀掉", "杀死", "恐怖", "淫秽",
]

_CONTROL_RE = re.compile(r"[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]")


def strip_control_chars(text: str) -> str:
    return _CONTROL_RE.sub("", text)


_SENTENCE_END = "。！？!?…"


def trim_to_sentence(story: str, limit: int) -> str:
    """正文超过 TTS 安全长度时在最后一个句末截断（契约音频上限 240 s，
    超出会以不可重试的 AUDIO_TOO_LONG 失败）。找不到句末时硬截断。"""
    if len(story) <= limit:
        return story
    head = story[:limit]
    cut = max(head.rfind(ch) for ch in _SENTENCE_END)
    return head[:cut + 1] if cut >= limit // 2 else head


def check_story_text(title: str, story: str, max_chars: int, max_utf8: int,
                     tts_max_chars: int = 900) -> tuple[str, str]:
    """返回 (清洗后的 title, story)；违规抛 JobFailure(CONTENT_REJECTED / AI_FAILED)。"""
    title = strip_control_chars(title).strip()
    story = strip_control_chars(story).strip()
    if not (1 <= len(title) <= 20):
        raise JobFailure("AI_FAILED", True, "story_generating")
    if len(story) > max_chars or len(story.encode("utf-8")) > max_utf8 or len(story) == 0:
        raise JobFailure("AI_FAILED", True, "story_generating")
    joined = title + "\n" + story
    for word in _BLOCKLIST:
        if word in joined:
            raise JobFailure("CONTENT_REJECTED", False, "story_generating")
    return display_safe_title(title), trim_to_sentence(story, tts_max_chars)
