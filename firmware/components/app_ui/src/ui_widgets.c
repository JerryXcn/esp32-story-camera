/**
 * @file ui_widgets.c
 * @brief v0.2 公共组件实现（见 ui_widgets.h）。
 */

#include "ui_widgets.h"
#include "ui_theme.h"
#include "ui_sound.h"
#include "ui_assets.h"
#include "assets.h"
#include "network_service.h"
#include "system_service.h"

#include <stdint.h>
#include <time.h>

/* ---------------- 图标 ---------------- */

#define ICS(n) { &img_ic_##n##_20, &img_ic_##n##_24, &img_ic_##n##_28, &img_ic_##n##_40 }
static const lv_image_dsc_t *const k_icons[UI_IC_COUNT][4] = {
    ICS(home), ICS(camera), ICS(books), ICS(back), ICS(next), ICS(play), ICS(pause),
    ICS(replay), ICS(more), ICS(volume), ICS(wifi), ICS(wifi_off), ICS(spark),
    ICS(check), ICS(trash), ICS(refresh), ICS(close), ICS(heart), ICS(skip_prev), ICS(skip_next), ICS(heart_f),
};

static const lv_image_dsc_t *icon_dsc(ui_icon_t ic, int size)
{
    if (ic < 0 || ic >= UI_IC_COUNT) return NULL;
    int idx = size <= 20 ? 0 : size <= 24 ? 1 : size <= 28 ? 2 : 3;
    return k_icons[ic][idx];
}

lv_obj_t *ui_icon_create(lv_obj_t *parent, ui_icon_t ic, int size, uint32_t color)
{
    lv_obj_t *img = lv_image_create(parent);
    lv_obj_set_style_image_recolor(img, lv_color_hex(color), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    ui_icon_set(img, ic, size);
    return img;
}

void ui_icon_set(lv_obj_t *icon, ui_icon_t ic, int size)
{
    const lv_image_dsc_t *d = icon_dsc(ic, size);
    if (d) {
        lv_image_set_src(icon, d);
        lv_obj_remove_flag(icon, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(icon, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_image_cover(lv_obj_t *img, int32_t box_w, int32_t box_h)
{
    const lv_image_dsc_t *d = lv_image_get_src(img);
    lv_obj_set_size(img, box_w, box_h);
    lv_obj_set_pos(img, 0, 0);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CENTER);
    if (!d || !d->header.w || !d->header.h) return;
    uint32_t sx = ((uint32_t)box_w * 256 + d->header.w - 1) / d->header.w;
    uint32_t sy = ((uint32_t)box_h * 256 + d->header.h - 1) / d->header.h;
    lv_image_set_scale(img, sx > sy ? sx : sy);
}

void ui_label_fit(lv_obj_t *label, const char *text, int32_t max_w)
{
    const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    lv_point_t sz;
    lv_text_get_size(&sz, text, font, lv_obj_get_style_text_letter_space(label, LV_PART_MAIN), 0,
                     LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_size(label, sz.x < max_w ? sz.x + 1 : max_w, lv_font_get_line_height(font));
    lv_label_set_text(label, text);
}

/* ---------------- 点击音 ---------------- */

static void click_sound_cb(lv_event_t *e)
{
    (void)e;
    ui_sound_click();
}

void ui_add_click_sound(lv_obj_t *obj)
{
    lv_obj_add_event_cb(obj, click_sound_cb, LV_EVENT_CLICKED, NULL);
}

static lv_obj_t *bare(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

/* ---------------- 圆形按钮 ---------------- */

lv_obj_t *ui_round_btn(lv_obj_t *parent, ui_icon_t ic, int diameter, ui_round_style_t style,
                       lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = bare(parent);
    lv_obj_set_size(b, diameter, diameter);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(b, diameter < 44 ? (44 - diameter) / 2 + 2 : 4);
    switch (style) {
    case UI_ROUND_GLASS:
        lv_obj_set_style_bg_color(b, lv_color_hex(UI_COL_WHITE), 0);
        lv_obj_set_style_bg_opa(b, 236, 0);
        lv_obj_set_style_bg_opa(b, 170, LV_STATE_PRESSED);
        break;
    case UI_ROUND_OUTLINE:
    case UI_ROUND_CREAM:
        lv_obj_set_style_bg_color(b, lv_color_hex(style == UI_ROUND_CREAM ? UI_COL_BG : UI_COL_WHITE), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(UI_COL_BORDER), 0);
        lv_obj_set_style_border_width(b, 2, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(UI_COL_BORDER), LV_STATE_PRESSED);
        break;
    }
    lv_obj_set_style_opa(b, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_t *icon = ui_icon_create(b, ic, diameter >= 52 ? 28 : 24, UI_COL_TEXT);
    lv_obj_center(icon);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    ui_add_click_sound(b);
    return b;
}

void ui_round_btn_set_icon(lv_obj_t *btn, ui_icon_t ic, uint32_t color)
{
    lv_obj_t *icon = btn ? lv_obj_get_child(btn, 0) : NULL;
    if (!icon) return;
    ui_icon_set(icon, ic, lv_obj_get_width(btn) >= 52 ? 28 : 24);
    lv_obj_set_style_image_recolor(icon, lv_color_hex(color), 0);
}

void ui_heart_btn_set(lv_obj_t *btn, bool favorite)
{
    ui_round_btn_set_icon(btn, favorite ? UI_IC_HEART_F : UI_IC_HEART, favorite ? UI_COL_PINK_DK : UI_COL_TEXT);
}

/* ---------------- 胶囊按钮（带厚度） ---------------- */

lv_obj_t *ui_chunky_btn(lv_obj_t *parent, ui_icon_t ic, const char *text, uint32_t face, uint32_t edge,
                        uint32_t text_color, int w, int h, lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = bare(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(b, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(b, 5, 0);
    lv_obj_set_style_border_width(b, 2, LV_STATE_PRESSED);
    lv_obj_set_style_translate_y(b, 3, LV_STATE_PRESSED);
    lv_obj_set_style_opa(b, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_set_style_pad_bottom(b, 3, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b, 6, 0);
    lv_obj_t *icon = ui_icon_create(b, ic, h >= 50 ? 24 : 20, text_color);
    lv_obj_t *lbl = lv_label_create(b);
    lv_obj_set_style_text_color(lbl, lv_color_hex(text_color), 0);
    lv_obj_set_style_text_font(lbl, ui_font_cn(h >= 50 ? 22 : 16), 0);
    lv_label_set_text(lbl, text ? text : "");
    (void)icon;
    ui_chunky_set(b, ic, text, face, edge);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    ui_add_click_sound(b);
    return b;
}

void ui_chunky_set(lv_obj_t *btn, ui_icon_t ic, const char *text, uint32_t face, uint32_t edge)
{
    lv_obj_set_style_bg_color(btn, lv_color_hex(face), 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(edge), 0);
    lv_obj_t *icon = lv_obj_get_child(btn, 0), *lbl = lv_obj_get_child(btn, 1);
    if (icon) ui_icon_set(icon, ic, lv_obj_get_height(btn) >= 50 ? 24 : 20);
    if (lbl && text) lv_label_set_text(lbl, text);
}

/* ---------------- 首页入口块 ---------------- */

lv_obj_t *ui_tile(lv_obj_t *parent, const lv_image_dsc_t *art, const char *title, const char *sub,
                  uint32_t face, uint32_t edge, lv_event_cb_t cb, void *user)
{
    lv_obj_t *t = bare(parent);
    lv_obj_set_size(t, 216, 120);
    lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(t, 26, 0);
    lv_obj_set_style_bg_color(t, lv_color_hex(face), 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(t, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(t, lv_color_hex(edge), 0);
    lv_obj_set_style_border_width(t, 6, 0);
    lv_obj_set_style_border_width(t, 2, LV_STATE_PRESSED);
    lv_obj_set_style_translate_y(t, 4, LV_STATE_PRESSED);

    if (art) {
        lv_obj_t *img = lv_image_create(t);
        lv_image_set_src(img, art);
        lv_obj_set_pos(img, 4, 8);
        lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    }
    /* 快乐体大字 + 深色"投影"标签，做出原稿的立体字效果 */
    lv_obj_t *shadow = lv_label_create(t);
    lv_label_set_text(shadow, title);
    lv_obj_set_style_text_font(shadow, ui_font_kid(32), 0);
    lv_obj_set_style_text_color(shadow, lv_color_hex(edge), 0);
    lv_obj_set_pos(shadow, 106, 24);
    lv_obj_t *lbl = lv_label_create(t);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_font(lbl, ui_font_kid(32), 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(UI_COL_WHITE), 0);
    lv_obj_set_pos(lbl, 106, 22);
    lv_obj_t *s = lv_label_create(t);
    lv_label_set_text(s, sub ? sub : "");
    lv_obj_set_style_text_color(s, lv_color_hex(UI_COL_WHITE), 0);
    lv_obj_set_pos(s, 108, 68);
    lv_obj_set_user_data(t, s);
    if (cb) lv_obj_add_event_cb(t, cb, LV_EVENT_CLICKED, user);
    ui_add_click_sound(t);
    return t;
}

void ui_tile_set_sub(lv_obj_t *tile, const char *sub)
{
    lv_obj_t *s = lv_obj_get_user_data(tile);
    if (s && sub) lv_label_set_text(s, sub);
}

/* ---------------- 页面标题栏 ---------------- */

lv_obj_t *ui_page_header(lv_obj_t *parent, ui_icon_t back_ic, const char *title, lv_event_cb_t back_cb)
{
    lv_obj_t *back = ui_round_btn(parent, back_ic, 40, UI_ROUND_OUTLINE, back_cb, NULL);
    lv_obj_set_pos(back, 12, 8);
    lv_obj_t *t = lv_label_create(parent);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, ui_font_kid(24), 0);
    lv_obj_set_style_text_color(t, lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_set_pos(t, 64, 14);
    return t;
}

/* ---------------- 状态胶囊 ---------------- */

typedef struct { lv_obj_t *icon, *label; lv_timer_t *timer; } pill_t;

static void pill_refresh(lv_timer_t *t)
{
    pill_t *p = lv_timer_get_user_data(t);
    bool online = network_state() >= NET_STATE_GOT_IP;
    ui_icon_set(p->icon, online ? UI_IC_WIFI : UI_IC_WIFI_OFF, 20);
    char buf[16] = "";
    if (!online) {
        lv_label_set_text(p->label, "未联网");
        return;
    }
    if (system_time_is_synced()) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(buf, sizeof(buf), "%H:%M", &tm);
    }
    lv_label_set_text(p->label, buf);
}

static void pill_deleted(lv_event_t *e)
{
    pill_t *p = lv_event_get_user_data(e);
    lv_timer_delete(p->timer);
    lv_free(p);
}

lv_obj_t *ui_status_pill(lv_obj_t *parent, bool on_photo)
{
    lv_obj_t *o = bare(parent);
    lv_obj_set_size(o, LV_SIZE_CONTENT, 28);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(on_photo ? UI_COL_SMOKE : UI_COL_WHITE), 0);
    lv_obj_set_style_bg_opa(o, on_photo ? 140 : 190, 0);
    lv_obj_set_style_pad_hor(o, 10, 0);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(o, 5, 0);
    uint32_t fg = on_photo ? UI_COL_WHITE : UI_COL_TEXT;
    pill_t *p = lv_malloc(sizeof(pill_t));
    p->icon = ui_icon_create(o, UI_IC_WIFI, 20, fg);
    p->label = lv_label_create(o);
    lv_obj_set_style_text_color(p->label, lv_color_hex(fg), 0);
    p->timer = lv_timer_create(pill_refresh, 2000, p);
    pill_refresh(p->timer);
    lv_obj_add_event_cb(o, pill_deleted, LV_EVENT_DELETE, p);
    return o;
}

/* ---------------- 分段按钮 ---------------- */

static void seg_clicked(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_current_target(e);
    lv_obj_t *seg = lv_obj_get_parent(btn);
    int index = (int)lv_obj_get_index(btn);
    ui_seg_select(seg, index);
    lv_obj_send_event(seg, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)index);
}

lv_obj_t *ui_seg(lv_obj_t *parent, const char *const *labels, int n, int active, lv_event_cb_t changed)
{
    lv_obj_t *seg = bare(parent);
    lv_obj_set_size(seg, LV_SIZE_CONTENT, 40);
    lv_obj_set_style_radius(seg, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(seg, lv_color_hex(UI_COL_BG), 0);
    lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(seg, lv_color_hex(UI_COL_BORDER), 0);
    lv_obj_set_style_border_width(seg, 2, 0);
    lv_obj_set_style_pad_all(seg, 2, 0);
    lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);
    for (int i = 0; i < n; i++) {
        lv_obj_t *b = bare(seg);
        lv_obj_set_size(b, LV_SIZE_CONTENT, 32);
        lv_obj_set_style_min_width(b, 52, 0);
        lv_obj_set_style_pad_hor(b, 10, 0);
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(b, 6);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, labels[i]);
        lv_obj_center(l);
        lv_obj_add_event_cb(b, seg_clicked, LV_EVENT_CLICKED, NULL);
        ui_add_click_sound(b);
    }
    ui_seg_select(seg, active);
    if (changed) lv_obj_add_event_cb(seg, changed, LV_EVENT_VALUE_CHANGED, NULL);
    return seg;
}

void ui_seg_select(lv_obj_t *seg, int index)
{
    uint32_t n = lv_obj_get_child_cnt(seg);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *b = lv_obj_get_child(seg, i);
        bool on = (int)i == index;
        lv_obj_set_style_bg_color(b, lv_color_hex(UI_COL_ORANGE), 0);
        lv_obj_set_style_bg_opa(b, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(b, 0), lv_color_hex(on ? UI_COL_WHITE : UI_COL_TEXT_DIM), 0);
    }
}

/* ---------------- 弹窗 ---------------- */

/* ---------------- 加载遮罩 / 轻提示 ---------------- */

lv_obj_t *ui_loading_show(const char *text)
{
    lv_obj_t *mask = bare(lv_screen_active());
    lv_obj_set_size(mask, 480, 320);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(mask, lv_color_hex(UI_COL_SMOKE), 0);
    lv_obj_set_style_bg_opa(mask, 120, 0);
    lv_obj_t *card = bare(mask);
    lv_obj_set_size(card, 240, 132);
    lv_obj_center(card);
    lv_obj_set_style_radius(card, 26, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(UI_COL_BG), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, 14, 0);
    lv_obj_t *sp = lv_spinner_create(card);
    lv_obj_set_size(sp, 40, 40);
    lv_obj_set_style_arc_width(sp, 6, 0);
    lv_obj_set_style_arc_width(sp, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(sp, lv_color_hex(UI_COL_BORDER), 0);
    lv_obj_set_style_arc_color(sp, lv_color_hex(UI_COL_ORANGE), LV_PART_INDICATOR);
    lv_obj_t *l = lv_label_create(card);
    lv_obj_set_style_text_font(l, ui_font_cn(22), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(UI_COL_TEXT), 0);
    lv_label_set_text(l, text);
    return mask;
}

void ui_loading_set_text(lv_obj_t *loading, const char *text)
{
    if (!loading) return;
    lv_obj_t *card = lv_obj_get_child(loading, 0);
    lv_obj_t *l = card ? lv_obj_get_child(card, 1) : NULL;
    if (l) lv_label_set_text(l, text);
}

static void toast_fade_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa(obj, (lv_opa_t)v, 0);
}

static void toast_done_cb(lv_anim_t *a)
{
    lv_obj_delete_async(a->var);
}

void ui_toast(const char *text)
{
    lv_obj_t *t = bare(lv_screen_active());
    lv_obj_set_size(t, LV_SIZE_CONTENT, 48);
    lv_obj_set_style_pad_hor(t, 20, 0);
    lv_obj_set_style_radius(t, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(t, lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_set_style_bg_opa(t, 235, 0);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(t, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(t, 8, 0);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *dot = bare(t);
    lv_obj_set_size(dot, 28, 28);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(UI_COL_GREEN), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_t *ic = ui_icon_create(dot, UI_IC_CHECK, 20, UI_COL_WHITE);
    lv_obj_center(ic);
    lv_obj_t *l = lv_label_create(t);
    lv_obj_set_style_text_font(l, ui_font_cn(22), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(UI_COL_WHITE), 0);
    lv_label_set_text(l, text);
    lv_obj_align(t, LV_ALIGN_BOTTOM_MID, 0, -100);   /* 播放页控制条（y≥236）之上 */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, t);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_delay(&a, 1500);
    lv_anim_set_duration(&a, 300);
    lv_anim_set_exec_cb(&a, toast_fade_cb);
    lv_anim_set_completed_cb(&a, toast_done_cb);
    lv_anim_start(&a);
}

void ui_dialog_close_cb(lv_event_t *e)
{
    lv_obj_t *mask = lv_event_get_user_data(e);
    if (mask) lv_obj_delete_async(mask);
}

lv_obj_t *ui_dialog(const ui_dialog_spec_t *spec)
{
    lv_obj_t *mask = bare(lv_screen_active());
    lv_obj_set_size(mask, 480, 320);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);            /* 吞掉背后点击 */
    lv_obj_set_style_bg_color(mask, lv_color_hex(UI_COL_SMOKE), 0);
    lv_obj_set_style_bg_opa(mask, 120, 0);

    lv_obj_t *card = bare(mask);
    lv_obj_set_size(card, 400, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(card, 196, 0);
    lv_obj_center(card);
    lv_obj_set_style_radius(card, 26, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(UI_COL_BG), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(card, 24, 0);
    lv_obj_set_style_shadow_opa(card, 70, 0);
    lv_obj_set_style_shadow_offset_y(card, 6, 0);
    lv_obj_set_style_pad_top(card, 20, 0);
    lv_obj_set_style_pad_bottom(card, 16, 0);
    lv_obj_set_style_pad_left(card, 116, 0);
    lv_obj_set_style_pad_right(card, 20, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 8, 0);

    const lv_image_dsc_t *fox = ui_asset(UI_ASSET_FOX_ERROR);
    if (fox) {
        lv_obj_t *img = lv_image_create(card);
        lv_image_set_src(img, fox);
        lv_obj_add_flag(img, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_align(img, LV_ALIGN_BOTTOM_LEFT, -102, 16);
    }

    lv_obj_t *head = bare(card);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, 10, 0);
    if (spec->photo) {
        lv_obj_t *frame = bare(head);
        lv_obj_set_size(frame, 66, 44);
        lv_obj_set_style_radius(frame, 8, 0);
        lv_obj_set_style_clip_corner(frame, true, 0);
        lv_obj_set_style_border_color(frame, lv_color_hex(UI_COL_WHITE), 0);
        lv_obj_set_style_border_width(frame, 2, 0);
        lv_obj_t *ph = lv_image_create(frame);
        lv_image_set_src(ph, spec->photo);
        ui_image_cover(ph, 62, 40);
    }
    lv_obj_t *t = lv_label_create(head);
    lv_label_set_text(t, spec->title);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(t, 1);
    lv_obj_set_style_text_font(t, ui_font_kid(24), 0);
    lv_obj_set_style_text_color(t, lv_color_hex(UI_COL_TEXT), 0);

    if (spec->body && spec->body[0]) {
        lv_obj_t *b = lv_label_create(card);
        lv_label_set_text(b, spec->body);
        lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(b, LV_PCT(100));
        lv_obj_set_style_text_color(b, lv_color_hex(UI_COL_TEXT_DIM), 0);
        lv_obj_set_style_text_line_space(b, 4, 0);
    }

    lv_obj_t *row = bare(card);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(row, 8, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);
    if (spec->left) {
        ui_chunky_btn(row, UI_IC_NONE, spec->left, UI_COL_WHITE, UI_COL_BORDER,
                      spec->left_danger ? UI_COL_DANGER : UI_COL_TEXT, 112, 44,
                      spec->left_cb ? spec->left_cb : ui_dialog_close_cb, mask);
    }
    uint32_t face = spec->right_face ? spec->right_face : UI_COL_ORANGE;
    uint32_t edge = spec->right_edge ? spec->right_edge : UI_COL_ORANGE_DK;
    ui_chunky_btn(row, UI_IC_NONE, spec->right ? spec->right : "知道了", face, edge, UI_COL_WHITE,
                  112, 44, spec->right_cb ? spec->right_cb : ui_dialog_close_cb, mask);
    return mask;
}
