#pragma once
/**
 * @file ui_theme.h
 * @brief 主题常量（实施计划 4.1）：颜色、字号、布局预算。
 * 浅色按钮配深色文字；实际对比度在实机检查（不得复制原稿白字到所有浅底）。
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

/* 公共样式（ui_theme.c 定义） */
extern lv_style_t ui_style_card;         /* 白底圆角卡片 */
extern lv_style_t ui_style_btn_primary;  /* 绿色主按钮 */
extern lv_style_t ui_style_text_dim;     /* 次要文字 */

void ui_theme_init(void);

/* 主色（实施计划 4.1） */
#define UI_COL_BG        0xFFF9ED  /* 奶油底 */
#define UI_COL_ORANGE    0xFF985F  /* 拍照 */
#define UI_COL_GREEN     0x4DBE78  /* 播放 */
#define UI_COL_BLUE_LT   0xA9DFF5  /* 状态/选中 */
#define UI_COL_YELLOW    0xFFD76A
#define UI_COL_TEXT      0x3A2925  /* 正文 */
#define UI_COL_TEXT_DIM  0x806D65  /* 次要 */
#define UI_COL_BORDER    0xEFE3D4
#define UI_COL_DANGER    0xEC6B63
#define UI_COL_WHITE     0xFFFFFF
#define UI_COL_BLUE      0x4A90E2  /* 主按钮蓝（Wi-Fi 下一步） */

/* v0.2 视觉规范：功能色 + 按钮"厚度"深色 */
#define UI_COL_ORANGE_DK 0xE2753C
#define UI_COL_GREEN_DK  0x349E5F
#define UI_COL_SKY       0x6BB6F0  /* 故事库 */
#define UI_COL_SKY_DK    0x4795D3
#define UI_COL_PINK      0xFF8FA3  /* 收藏 */
#define UI_COL_PINK_DK   0xE06C83
#define UI_COL_VIOLET    0x9B8AE6  /* 设置 */
#define UI_COL_VIOLET_DK 0x7B69CC
#define UI_COL_SMOKE     0x241A14  /* 叠在照片上的深色半透明底 */
#define UI_COL_MUTE      0xC9BBAE  /* 不可用/未完成 */

/* 中文字体（开源可商用，生成方法见 tools/gen_fonts.sh，许可证见 fonts/licenses/）：
 * - 正文/AI 标题：源圆黑体 Medium，16/22px，覆盖 GB2312 一级字
 * - 固定大字：站酷快乐体，24/32/44px，只含 tools/kid_texts.txt 里的字（缺字回退到 22px 正文） */
LV_FONT_DECLARE(ui_font_16)
LV_FONT_DECLARE(ui_font_22)
LV_FONT_DECLARE(ui_kid_24)
LV_FONT_DECLARE(ui_kid_32)
LV_FONT_DECLARE(ui_kid_44)
const lv_font_t *ui_font_cn(int px);   /* 14/15/16 -> 16px，>=20 -> 22px */
const lv_font_t *ui_font_kid(int px);  /* <28 -> 24px，<40 -> 32px，其余 44px */

/* 字号（px，LVGL 内置字体起点；动态中文标题方案 M1 冻结） */
#define UI_FONT_PAGE_TITLE  22
#define UI_FONT_STORY_TITLE 18
#define UI_FONT_BUTTON      16
#define UI_FONT_BODY        14

/* 触控目标（产品文档 5.1）：主要目标 ≥44px，快门 80px */
#define UI_TOUCH_MIN   44
#define UI_SHUTTER     80

/* 布局预算（实施计划 4.1） */
#define UI_PREVIEW_X 0      /* v0.2：全屏 3:2 预览 */
#define UI_PREVIEW_Y 0
#define UI_PREVIEW_W 480
#define UI_PREVIEW_H 320
#define UI_RIGHT_COL_W 104
#define UI_CARD_W 100
#define UI_CARD_H 128
#define UI_CARD_GAP 12
#define UI_CARD_MARGIN 22
#define UI_PHOTO_BOX 216

/** Consistent button baseline, including transparent back buttons. */
lv_obj_t *ui_button_create(lv_obj_t *parent);

#ifdef __cplusplus
}
#endif
