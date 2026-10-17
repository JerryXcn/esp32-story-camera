"""设备可显示字符集：标题会在 480×320 屏上以内置字库渲染。

固件字库（firmware/components/app_ui/tools/sc_charset.py）至少包含 GB2312 一级
3755 字 + ASCII 字母数字 + 下列标点；本表与之保持一致。标题出现字库外的字时
设备只能显示方框，因此整体换成固定兜底标题（正文不受影响，正文只用于朗读）。
"""
from __future__ import annotations

import logging

log = logging.getLogger(__name__)

FALLBACK_TITLE = "温暖的小故事"

_PUNCT = " ·•…、。，！？：；（）()「」《》—/-%#@*+=:,.[]{}<>~"


def _build() -> frozenset[str]:
    chars = set("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz")
    chars.update(_PUNCT)
    for hi in range(0xB0, 0xD8):
        for lo in range(0xA1, 0xFF):
            try:
                chars.add(bytes((hi, lo)).decode("gb2312"))
            except UnicodeDecodeError:
                pass
    return frozenset(chars)


DISPLAYABLE = _build()


def display_safe_title(title: str) -> str:
    missing = {ch for ch in title if ch not in DISPLAYABLE}
    if not missing:
        return title
    log.warning("title has %d glyph(s) outside device font; using fallback", len(missing))
    return FALLBACK_TITLE
