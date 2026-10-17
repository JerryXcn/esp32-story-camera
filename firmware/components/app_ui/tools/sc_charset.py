#!/usr/bin/env python3
"""输出 UI 字库字符集：UI 源码中的汉字 + 文档界面措辞 + 高频常用字 + ASCII/标点。

新增界面文案后重新运行 gen_fonts.sh 即可覆盖新字符。
"""
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
UI_SRC = os.path.join(HERE, "..", "src")


def main() -> None:
    chars = set()
    for path in glob.glob(os.path.join(UI_SRC, "*.c")):
        for ch in open(path, encoding="utf-8").read():
            if "\u4e00" <= ch <= "\u9fff":
                chars.add(ch)
    # 文档界面措辞（保证覆盖产品文案）
    for doc in ("AI 绘本故事机产品设计文档 V0.1.md",
                "AI 绘本故事机详细实施计划方案 V1.0.md"):
        p = os.path.join(HERE, "..", "..", "..", doc)
        try:
            for ch in open(p, encoding="utf-8").read():
                if "\u4e00" <= ch <= "\u9fff":
                    chars.add(ch)
        except OSError:
            pass
    # 高频常用字（覆盖 AI 动态标题/SSID）
    extra = open(os.path.join(HERE, "sc_charset_raw.txt"), encoding="utf-8").read()
    for ch in extra:
        if "\u4e00" <= ch <= "\u9fff":
            chars.add(ch)
    # GB2312 \u4e00\u7ea7\u5b57\u5e93 3755 \u5b57\uff1aAI \u6807\u9898\u7528\u5b57\u4e0d\u53ef\u9884\u77e5\uff0c\u540e\u7aef\u6309\u540c\u4e00\u5b57\u8868\u6821\u9a8c\u6807\u9898
    # \uff08server/app/services/charset.py\uff09\uff0c\u4e24\u4fa7\u987b\u4fdd\u6301\u4e00\u81f4\u3002
    for hi in range(0xB0, 0xD8):
        for lo in range(0xA1, 0xFF):
            try:
                chars.add(bytes((hi, lo)).decode("gb2312"))
            except UnicodeDecodeError:
                pass
    # 全部可打印 ASCII：SSID 与 AI 标题可能含任意半角符号（字库缺字时的回退字体实测不可靠）
    ascii_chars = "".join(chr(c) for c in range(0x21, 0x7F))
    punct = " \u00b7\u2022\u2026\u3001\u3002\uff0c\uff01\uff1f\uff1a\uff1b\uff08\uff09()\u300c\u300d\u300a\u300b\u2014/-%#@*+=:,.[]{}<>~"
    sys.stdout.write("".join(sorted(chars)) + ascii_chars + punct)


if __name__ == "__main__":
    main()
