/**
 * @file ui_theme.c
 * @brief 主题与公共样式。
 */

#include "ui_theme.h"
#include "lvgl.h"

lv_style_t ui_style_card;
lv_style_t ui_style_btn_primary;
lv_style_t ui_style_text_dim;

const lv_font_t *ui_font_cn(int px)
{
    /* 自生成字库：16px（正文/按钮）与 22px（页标题），覆盖 UI 文案+高频常用字 */
    return (px >= 20) ? &ui_font_22 : &ui_font_16;
}

const lv_font_t *ui_font_kid(int px)
{
    return px < 28 ? &ui_kid_24 : (px < 40 ? &ui_kid_32 : &ui_kid_44);
}

void ui_theme_init(void)
{
    /* 所有 label 默认使用中文字体：中文标签不需要逐个设字体，
     * 避免遗漏导致方块（M0 实测）。数值/ASCII 也能显示。 */
    lv_obj_set_style_text_font(lv_screen_active(), ui_font_cn(16), 0);
    lv_style_init(&ui_style_card);
    lv_style_set_bg_color(&ui_style_card, lv_color_hex(UI_COL_WHITE));
    lv_style_set_bg_opa(&ui_style_card, LV_OPA_COVER);
    lv_style_set_radius(&ui_style_card, 12);
    lv_style_set_border_color(&ui_style_card, lv_color_hex(UI_COL_BORDER));
    lv_style_set_border_width(&ui_style_card, 1);
    lv_style_set_pad_all(&ui_style_card, 6);

    lv_style_init(&ui_style_btn_primary);
    lv_style_set_bg_color(&ui_style_btn_primary, lv_color_hex(UI_COL_GREEN));
    lv_style_set_text_color(&ui_style_btn_primary, lv_color_hex(UI_COL_TEXT));
    lv_style_set_radius(&ui_style_btn_primary, 22);
    lv_style_set_min_height(&ui_style_btn_primary, UI_TOUCH_MIN);

    lv_style_init(&ui_style_text_dim);
    lv_style_set_text_color(&ui_style_text_dim, lv_color_hex(UI_COL_TEXT_DIM));
}

lv_obj_t *ui_button_create(lv_obj_t *parent)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_style_bg_color(button, lv_color_hex(UI_COL_BORDER), 0);
    lv_obj_set_style_text_color(button, lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_set_style_text_font(button, ui_font_cn(16), 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_radius(button, 12, 0);
    lv_obj_set_style_pad_all(button, 4, 0);
    lv_obj_set_style_opa(button, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    return button;
}
