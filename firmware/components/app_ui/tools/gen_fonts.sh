#!/bin/sh
# 生成 UI 字库（lv_font_conv，4bpp，LVGL 格式）。字体均为开源可商用，许可证见 ../fonts/licenses/。
#
#   正文 ui_font_16 / ui_font_22：源圆黑体 Medium（Resource Han Rounded CN，MIT+OFL）
#     字符集 = UI 源码文案 + 文档措辞 + 高频常用字 + GB2312 一级 3755 字（AI 标题可显示）
#   大字 ui_kid_24 / ui_kid_32 / ui_kid_44：站酷快乐体（ZCOOL KuaiLe，OFL）
#     字符集 = tools/kid_texts.txt 中的固定文案
#
# 用法：gen_fonts.sh <ResourceHanRoundedCN-Medium.ttf> <ZCOOLKuaiLe-Regular.ttf>
# 字体文件约 15MB，不入库；下载地址见 ../fonts/README.md。依赖：npm i -g lv_font_conv
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
BODY="${1:?需要源圆黑体 TTF 路径}"
KID="${2:?需要站酷快乐体 TTF 路径}"
OUT="$HERE/../fonts"

CHARSET="$(python3 "$HERE/sc_charset.py")"
for SIZE in 16 22; do
  lv_font_conv --font "$BODY" --symbols "$CHARSET" --size "$SIZE" --bpp 4 \
    --format lvgl --lv-include lvgl.h --lv-font-name "ui_font_$SIZE" --no-kerning \
    --no-compress --lv-fallback lv_font_montserrat_14 \
    -o "$OUT/ui_font_$SIZE.c"
done

KIDSET="$(python3 -c "
import sys
s=set()
for line in open(sys.argv[1], encoding='utf-8'):
    if line.startswith('#'): continue
    s.update(ch for ch in line if not ch.isspace())
print(''.join(sorted(s)) + ' ')" "$HERE/kid_texts.txt")"
for SIZE in 24 32 44; do
  lv_font_conv --font "$KID" --symbols "$KIDSET" --size "$SIZE" --bpp 4 \
    --format lvgl --lv-include lvgl.h --lv-font-name "ui_kid_$SIZE" --no-kerning \
    --no-compress --lv-fallback ui_font_22 \
    -o "$OUT/ui_kid_$SIZE.c"
done
echo "fonts regenerated under components/app_ui/fonts/"
