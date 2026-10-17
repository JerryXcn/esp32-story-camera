/**
 * @file ui_screens_library_detail.c
 * @brief 故事库（画面6/11）与详情（画面7），v0.2。
 *
 * 故事库：四列卡片（照片铺满 + 标题 + 拍摄日期）；右上角状态角标：
 *   绿色 ▶ 可以听（点了直接播放）；橙色 ! 上次没生成好；照片变灰 + "未生成" 还没有故事；
 *   没有故事的卡片点了进详情。左右滑动或两侧箭头翻页，底部圆点显示页码。
 *   扫描结束才判定空库（区分"正在查找"与"空库"）。
 *   收藏视图（首页"收藏"进入）只列已收藏的故事；卡片左上角粉心表示已收藏。
 *   选择模式（按住任意卡片 6 秒，防小朋友误触；不设"选择"按钮与全选）：点卡片打勾，可跨页，
 *   底部"收藏 / 取消收藏 / 删除"批量操作（删除需确认），左上角 × 退出。
 * 详情：3:2 照片、标题、日期/版本标签；主按钮按状态为 播放故事 / 生成故事 / 继续生成；
 *   重新生成先确认（旧故事保留）；删除先确认。
 */

#include "ui_screens.h"
#include "ui_assets.h"
#include "ui_widgets.h"
#include "app_ui.h"
#include "assets.h"
#include "story_service.h"
#include "storage_service.h"
#include "network_service.h"
#include "ui_story_ops.h"

#include "lvgl.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "esp_heap_caps.h"

void ui_dialog_show_error(const char *code, bool retryable);
void ui_dialog_confirm_delete(const char *story_id);
void ui_dialog_confirm_regen(const char *story_id);
void on_go_home(lv_event_t *e);
void on_library_open(lv_event_t *e);

#define CARDS_PER_PAGE 4
#define CARD_W 100
#define CARD_H 138
#define PHOTO_H 96

/** 拍摄时间（本地时区）；未校时拍摄的条目没有可信日期，返回 false */
static bool format_capture_time(const story_summary_t *s, const char *fmt, char *out, size_t n)
{
    struct tm tm;
    time_t t = (time_t)s->created_at_epoch;
    out[0] = '\0';
    return s->has_time && t > 0 && localtime_r(&t, &tm) && strftime(out, n, fmt, &tm);
}

static bool has_unfinished(const story_summary_t *s)
{
    char op[STORAGE_ID_HEX];
    return storage_find_latest_operation(s->story_id, op) == ESP_OK;
}

/** 缩略图按"铺满裁切"显示在 w x h 的框里 */
static void bind_cover(lv_obj_t *img, const char *path, int32_t w, int32_t h)
{
    if (!ui_thumb_bind(img, path, w, h)) return;
    ui_image_cover(img, w, h);
}

/* ---------------- 故事库 ---------------- */

static lv_obj_t *s_root_lib, *s_grid, *s_empty_box, *s_scan_label, *s_count_label;
static lv_obj_t *s_prev, *s_next, *s_dots, *s_page_text;
static lv_obj_t *s_lib_title, *s_lib_left, *s_sel_bar, *s_fav_add_btn, *s_fav_rm_btn, *s_delete_btn;
static lv_obj_t *s_empty_t1, *s_empty_t2, *s_empty_go;
static int s_page;
static bool s_fav_view, s_select, s_long_pressed;
#define SELECT_HOLD_MS 6000                     /* 按住卡片多久进入选择模式 */
static lv_timer_t *s_hold_timer;
static char s_hold_id[STORAGE_ID_HEX];
static char (*s_sel)[STORAGE_ID_HEX];      /* 选择模式里已勾选的故事 ID（跨页保留） */
static int s_sel_n;
static int16_t s_view[STORAGE_MAX_STORIES];   /* 当前视图（全部/收藏）→ 列表下标 */
static int s_view_n;

static void rebuild_grid(void);

static void rebuild_async(void *arg)
{
    (void)arg;
    rebuild_grid();
}

static int sel_find(const char *id)
{
    for (int i = 0; i < s_sel_n; i++) if (strcmp(s_sel[i], id) == 0) return i;
    return -1;
}

static void sel_toggle(const char *id)
{
    int i = sel_find(id);
    if (i >= 0) {
        memmove(s_sel[i], s_sel[i + 1], (size_t)(s_sel_n - i - 1) * STORAGE_ID_HEX);
        s_sel_n--;
    } else if (s_sel && s_sel_n < STORAGE_MAX_STORIES) {
        strlcpy(s_sel[s_sel_n++], id, STORAGE_ID_HEX);
    }
}

static void set_select_mode(bool on)
{
    s_select = on;
    s_sel_n = 0;
}

static void on_card_click(lv_event_t *e)
{
    /* 刚完成一次滑动翻页时不当作点击 */
    lv_indev_t *indev = lv_indev_active();
    if (indev && lv_indev_get_gesture_dir(indev) != LV_DIR_NONE) return;
    const story_summary_t *s = lv_event_get_user_data(e);
    if (!s) return;
    if (s_long_pressed) {           /* 长按已进入选择模式，松手的这次点击不再切换 */
        s_long_pressed = false;
        return;
    }
    if (s_select) {
        sel_toggle(s->story_id);
        lv_async_call(rebuild_async, NULL);   /* 不能在卡片自己的事件里删除卡片 */
        return;
    }
    if (s->availability == STORY_AVAIL_PLAYABLE) screen_player_open(s);
    else screen_detail_open(s);
}

static void hold_cancel(void)
{
    if (s_hold_timer) {
        lv_timer_delete(s_hold_timer);
        s_hold_timer = NULL;
    }
}

static void on_hold_elapsed(lv_timer_t *t)
{
    (void)t;
    s_hold_timer = NULL;                        /* 单次定时器，触发后 LVGL 自动删除 */
    if (s_select || !s_sel || ui_navigator_current() != SCREEN_LIBRARY) return;
    set_select_mode(true);
    sel_toggle(s_hold_id);
    s_long_pressed = true;
    ui_toast("选择要删除的故事");
    lv_async_call(rebuild_async, NULL);
}

static void on_card_hold(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    const story_summary_t *s = lv_event_get_user_data(e);
    if (code == LV_EVENT_PRESSED) {
        s_long_pressed = false;
        hold_cancel();
        if (s_select || !s) return;
        strlcpy(s_hold_id, s->story_id, sizeof(s_hold_id));
        s_hold_timer = lv_timer_create(on_hold_elapsed, SELECT_HOLD_MS, NULL);
        lv_timer_set_repeat_count(s_hold_timer, 1);
    } else {                                    /* 松手 / 滑走：取消 */
        hold_cancel();
    }
}

/** 卡片左上角小圆标：选择模式下为勾选框，否则已收藏时为粉心 */
static void add_corner_mark(lv_obj_t *card, const story_summary_t *s)
{
    bool selected = s_select && sel_find(s->story_id) >= 0;
    if (!s_select && !s->favorite) return;
    lv_obj_t *m = lv_obj_create(card);
    lv_obj_remove_style_all(m);
    lv_obj_set_size(m, 28, 28);
    lv_obj_set_pos(m, 6, 6);
    lv_obj_set_style_radius(m, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(m, lv_color_hex(UI_COL_WHITE), 0);
    lv_obj_set_style_border_width(m, 2, 0);
    lv_obj_set_style_bg_opa(m, s_select && !selected ? 150 : LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(m, lv_color_hex(s_select ? (selected ? UI_COL_ORANGE : UI_COL_SMOKE) : UI_COL_PINK), 0);
    if (!s_select || selected) {
        lv_obj_t *ic = ui_icon_create(m, s_select ? UI_IC_CHECK : UI_IC_HEART_F, 20, UI_COL_WHITE);
        lv_obj_center(ic);
    }
    if (selected) {
        lv_obj_set_style_border_side(card, LV_BORDER_SIDE_FULL, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(UI_COL_ORANGE), 0);
        lv_obj_set_style_border_width(card, 4, 0);
    }
}

static lv_obj_t *make_card(lv_obj_t *parent, const story_summary_t *s)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, CARD_W, CARD_H);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_clip_corner(card, true, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(UI_COL_WHITE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(card, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(UI_COL_BORDER), 0);
    lv_obj_set_style_border_width(card, 3, 0);
    lv_obj_set_style_translate_y(card, 2, LV_STATE_PRESSED);

    bool playable = s->availability == STORY_AVAIL_PLAYABLE;
    lv_obj_t *ph = lv_obj_create(card);
    lv_obj_remove_style_all(ph);
    lv_obj_set_size(ph, CARD_W, PHOTO_H);
    lv_obj_set_style_bg_color(ph, lv_color_hex(UI_COL_BORDER), 0);
    lv_obj_set_style_bg_opa(ph, LV_OPA_COVER, 0);
    lv_obj_t *img = lv_image_create(ph);
    bind_cover(img, s->thumb_path, CARD_W, PHOTO_H);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_size(title, CARD_W - 12, 20);
    lv_obj_set_pos(title, 7, PHOTO_H + 2);
    lv_label_set_text(title, playable && s->title[0] ? s->title : "还没有故事");
    lv_obj_set_style_text_color(title, lv_color_hex(playable ? UI_COL_TEXT : UI_COL_TEXT_DIM), 0);
    char when[16];
    if (format_capture_time(s, "%m-%d", when, sizeof(when))) {
        lv_obj_t *date = lv_label_create(card);
        lv_label_set_text(date, when);
        lv_obj_set_style_text_color(date, lv_color_hex(UI_COL_TEXT_DIM), 0);
        lv_obj_set_pos(date, 7, PHOTO_H + 20);
    }

    if (playable) {
        lv_obj_t *b = lv_obj_create(card);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, 30, 30);
        lv_obj_set_pos(b, CARD_W - 36, 6);
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(UI_COL_GREEN), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(UI_COL_WHITE), 0);
        lv_obj_set_style_border_width(b, 2, 0);
        lv_obj_t *ic = ui_icon_create(b, UI_IC_PLAY, 20, UI_COL_WHITE);
        lv_obj_center(ic);
    } else {
        lv_obj_t *veil = lv_obj_create(ph);
        lv_obj_remove_style_all(veil);
        lv_obj_set_size(veil, CARD_W, PHOTO_H);
        lv_obj_set_style_bg_color(veil, lv_color_hex(UI_COL_BG), 0);
        lv_obj_set_style_bg_opa(veil, 120, 0);
        if (has_unfinished(s)) {
            lv_obj_t *b = lv_obj_create(card);
            lv_obj_remove_style_all(b);
            lv_obj_set_size(b, 30, 30);
            lv_obj_set_pos(b, CARD_W - 36, 6);
            lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(b, lv_color_hex(UI_COL_ORANGE), 0);
            lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(b, lv_color_hex(UI_COL_WHITE), 0);
            lv_obj_set_style_border_width(b, 2, 0);
            lv_obj_t *l = lv_label_create(b);
            lv_label_set_text(l, "！");
            lv_obj_set_style_text_font(l, ui_font_cn(22), 0);
            lv_obj_set_style_text_color(l, lv_color_hex(UI_COL_WHITE), 0);
            lv_obj_center(l);
        } else {
            lv_obj_t *chip = lv_obj_create(ph);
            lv_obj_remove_style_all(chip);
            lv_obj_set_size(chip, LV_SIZE_CONTENT, 26);
            lv_obj_set_style_pad_hor(chip, 8, 0);
            lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(chip, lv_color_hex(UI_COL_WHITE), 0);
            lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
            lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(chip, 2, 0);
            ui_icon_create(chip, UI_IC_SPARK, 20, UI_COL_ORANGE_DK);
            lv_obj_t *l = lv_label_create(chip);
            lv_label_set_text(l, "未生成");
            lv_obj_set_style_text_color(l, lv_color_hex(UI_COL_ORANGE_DK), 0);
            lv_obj_center(chip);
        }
    }
    add_corner_mark(card, s);
    /* 子对象不接收点击，点在任何位置都算卡片 */
    for (uint32_t c = 0; c < lv_obj_get_child_cnt(card); c++) {
        lv_obj_t *ch = lv_obj_get_child(card, c);
        lv_obj_remove_flag(ch, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(ch, LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_EVENT_BUBBLE);
        for (uint32_t g = 0; g < lv_obj_get_child_cnt(ch); g++) {
            lv_obj_remove_flag(lv_obj_get_child(ch, g), LV_OBJ_FLAG_CLICKABLE);
        }
    }
    lv_obj_add_event_cb(card, on_card_click, LV_EVENT_CLICKED, (void *)s);
    lv_obj_add_event_cb(card, on_card_hold, LV_EVENT_PRESSED, (void *)s);
    lv_obj_add_event_cb(card, on_card_hold, LV_EVENT_RELEASED, (void *)s);
    lv_obj_add_event_cb(card, on_card_hold, LV_EVENT_PRESS_LOST, (void *)s);
    ui_add_click_sound(card);
    return card;
}

static void build_view(void)
{
    const story_summary_t *list = app_ui_list();
    int n = app_ui_list_count();
    s_view_n = 0;
    for (int i = 0; i < n; i++) {
        if (!s_fav_view || list[i].favorite) s_view[s_view_n++] = (int16_t)i;
    }
    /* 已不在视图里的勾选（刚被删除/取消收藏）去掉 */
    for (int k = s_sel_n - 1; k >= 0; k--) {
        bool found = false;
        for (int v = 0; v < s_view_n && !found; v++) found = strcmp(list[s_view[v]].story_id, s_sel[k]) == 0;
        if (!found) sel_toggle(s_sel[k]);
    }
}

static void update_header(void)
{
    lv_label_set_text(s_lib_title, s_select ? "选择故事" : s_fav_view ? "收藏" : "故事库");
    ui_round_btn_set_icon(s_lib_left, s_select ? UI_IC_CLOSE : UI_IC_HOME, UI_COL_TEXT);
    if (s_select) lv_label_set_text_fmt(s_count_label, "已选 %d 个", s_sel_n);
    else lv_label_set_text_fmt(s_count_label, s_fav_view ? "%d 个收藏" : "%d 个故事", s_view_n);
    if (s_select) {
        lv_obj_t *btns[3] = { s_fav_add_btn, s_fav_rm_btn, s_delete_btn };
        for (int i = 0; i < 3; i++) {
            if (s_sel_n) lv_obj_remove_state(btns[i], LV_STATE_DISABLED);
            else lv_obj_add_state(btns[i], LV_STATE_DISABLED);
        }
        lv_obj_remove_flag(s_sel_bar, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_sel_bar, LV_OBJ_FLAG_HIDDEN);
    }
}

static void rebuild_grid(void)
{
    if (!s_grid) return;
    hold_cancel();                              /* 卡片要重建，按住计时作废 */
    lv_obj_clean(s_grid);
    lv_obj_clean(s_dots);
    build_view();
    const story_summary_t *list = app_ui_list();
    int n = s_view_n;
    lv_obj_add_flag(s_empty_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_scan_label, LV_OBJ_FLAG_HIDDEN);
    if (n == 0 && s_select) set_select_mode(false);
    update_header();

    if (n == 0) {
        lv_obj_add_flag(s_prev, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_next, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_page_text, "");
        if (app_ui_scan_pending()) {
            lv_obj_remove_flag(s_scan_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_label_set_text(s_empty_t1, s_fav_view ? "还没有收藏的故事" : "这里还没有故事哦");
            lv_label_set_text(s_empty_t2, s_fav_view ? "听故事时点右上角的爱心，就能收藏到这里"
                                                     : "拍下第一页绘本，让小狐狸讲给你听");
            if (s_fav_view) ui_chunky_set(s_empty_go, UI_IC_BOOKS, "去故事库", UI_COL_SKY, UI_COL_SKY_DK);
            else ui_chunky_set(s_empty_go, UI_IC_CAMERA, "去拍照", UI_COL_ORANGE, UI_COL_ORANGE_DK);
            lv_obj_remove_flag(s_empty_box, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    int pages = (n + CARDS_PER_PAGE - 1) / CARDS_PER_PAGE;
    if (s_page >= pages) s_page = pages - 1;
    if (s_page < 0) s_page = 0;
    if (pages > 1) {
        lv_obj_remove_flag(s_prev, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_next, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_prev, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_next, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_page == 0) lv_obj_add_state(s_prev, LV_STATE_DISABLED); else lv_obj_remove_state(s_prev, LV_STATE_DISABLED);
    if (s_page + 1 >= pages) lv_obj_add_state(s_next, LV_STATE_DISABLED); else lv_obj_remove_state(s_next, LV_STATE_DISABLED);
    /* 页码：页数少用圆点，多了用"3 / 9"；选择模式下底部中间放删除按钮 */
    if (s_select) {
        lv_label_set_text(s_page_text, "");
    } else if (pages > 1 && pages <= 8) {
        lv_label_set_text(s_page_text, "");
        for (int p = 0; p < pages; p++) {
            lv_obj_t *d = lv_obj_create(s_dots);
            lv_obj_remove_style_all(d);
            lv_obj_set_size(d, p == s_page ? 22 : 8, 8);
            lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(d, lv_color_hex(p == s_page ? UI_COL_ORANGE : UI_COL_MUTE), 0);
            lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        }
    } else if (pages > 8) {
        lv_label_set_text_fmt(s_page_text, "%d / %d", s_page + 1, pages);
    } else {
        lv_label_set_text(s_page_text, "");
    }
    for (int i = s_page * CARDS_PER_PAGE; i < n && i < (s_page + 1) * CARDS_PER_PAGE; i++) {
        make_card(s_grid, &list[s_view[i]]);
    }
}

static void turn_page(int delta)
{
    int n = s_view_n;
    int pages = (n + CARDS_PER_PAGE - 1) / CARDS_PER_PAGE;
    int next = s_page + delta;
    if (next < 0 || next >= pages) return;
    s_page = next;
    rebuild_grid();
}

static void on_library_page(lv_event_t *ev)
{
    turn_page((int)(intptr_t)lv_event_get_user_data(ev));
}

static void on_library_gesture(lv_event_t *e)
{
    (void)e;
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_LEFT) turn_page(1);
    else if (dir == LV_DIR_RIGHT) turn_page(-1);
}

static void on_empty_go(lv_event_t *e)
{
    (void)e;
    if (s_fav_view) {
        s_fav_view = false;
        s_page = 0;
        rebuild_grid();
    } else {
        ui_navigator_go(SCREEN_CAMERA);
    }
}

static void on_lib_left(lv_event_t *e)
{
    if (s_select) {
        set_select_mode(false);
        rebuild_grid();
    } else {
        on_go_home(e);
    }
}

static void on_fav_selected(lv_event_t *e)
{
    bool fav = lv_event_get_user_data(e) != NULL;
    int n = s_sel_n;
    if (n == 0 || !ui_ops_set_favorite_many((const char (*)[STORAGE_ID_HEX])s_sel, n, fav)) return;
    set_select_mode(false);
    rebuild_grid();
    char t[40];
    snprintf(t, sizeof(t), fav ? "已收藏 %d 个" : "已取消收藏 %d 个", n);
    ui_toast(t);
}

static void on_batch_deleted(int deleted)
{
    (void)deleted;
    set_select_mode(false);
}

static void on_batch_delete_yes(lv_event_t *e)
{
    lv_obj_delete_async(lv_event_get_user_data(e));
    ui_ops_delete((const char (*)[STORAGE_ID_HEX])s_sel, s_sel_n, on_batch_deleted);
}

static void on_delete_selected(lv_event_t *e)
{
    (void)e;
    if (s_sel_n == 0) return;
    static char title[40];
    snprintf(title, sizeof(title), "删除这 %d 条故事？", s_sel_n);
    ui_dialog_spec_t spec = {
        .title = title,
        .body = "照片和故事都会删掉，在设备里就找不到了。",
        .left = "取消",
        .right = "删除", .right_cb = on_batch_delete_yes,
        .right_face = UI_COL_DANGER, .right_edge = 0xC2453E,
    };
    ui_dialog(&spec);
}

bool screen_library_neighbor(const char *story_id, int dir, story_summary_t *out)
{
    /* 播放页"上一个/下一个"：故事库当前视图（全部/收藏）里能听的故事，按列表顺序循环 */
    static int16_t cand[STORAGE_MAX_STORIES];
    const story_summary_t *list = app_ui_list();
    int n = app_ui_list_count();
    for (int pass = 0; pass < 2; pass++) {
        bool fav_only = s_fav_view && pass == 0;
        int c = 0, pos = -1;
        for (int i = 0; i < n; i++) {
            if (list[i].availability != STORY_AVAIL_PLAYABLE || (fav_only && !list[i].favorite)) continue;
            if (strcmp(list[i].story_id, story_id) == 0) pos = c;
            cand[c++] = (int16_t)i;
        }
        if (pos < 0) continue;              /* 当前故事不在收藏视图里：改按全部故事 */
        if (c < 2) return false;
        *out = list[cand[(pos + (dir > 0 ? 1 : c - 1)) % c]];
        return true;
    }
    return false;
}

void screen_library_open(bool favorites)
{
    s_fav_view = favorites;
    set_select_mode(false);
    s_page = 0;
    ui_navigator_go(SCREEN_LIBRARY);
}

lv_obj_t *screen_library_create(void)
{
    s_root_lib = lv_obj_create(ui_screens_parent());
    lv_obj_set_style_bg_color(s_root_lib, lv_color_hex(UI_COL_BG), 0);
    lv_obj_add_event_cb(s_root_lib, on_library_gesture, LV_EVENT_GESTURE, NULL);

    s_sel = heap_caps_calloc(STORAGE_MAX_STORIES, STORAGE_ID_HEX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_lib_left = ui_round_btn(s_root_lib, UI_IC_HOME, 40, UI_ROUND_OUTLINE, on_lib_left, NULL);
    lv_obj_set_pos(s_lib_left, 12, 8);
    s_lib_title = lv_label_create(s_root_lib);
    lv_label_set_text(s_lib_title, "故事库");
    lv_obj_set_style_text_font(s_lib_title, ui_font_kid(24), 0);
    lv_obj_set_style_text_color(s_lib_title, lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_set_pos(s_lib_title, 64, 14);
    lv_obj_t *count = lv_obj_create(s_root_lib);
    lv_obj_remove_style_all(count);
    lv_obj_set_size(count, LV_SIZE_CONTENT, 28);
    lv_obj_set_style_pad_hor(count, 12, 0);
    lv_obj_set_style_radius(count, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(count, lv_color_hex(UI_COL_WHITE), 0);
    lv_obj_set_style_bg_opa(count, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(count, lv_color_hex(UI_COL_BORDER), 0);
    lv_obj_set_style_border_width(count, 1, 0);
    lv_obj_align(count, LV_ALIGN_TOP_RIGHT, -16, 14);
    s_count_label = lv_label_create(count);
    lv_obj_set_style_text_color(s_count_label, lv_color_hex(UI_COL_TEXT_DIM), 0);
    lv_obj_center(s_count_label);

    /* 四列：22 + 4*100 + 3*12 + 22 = 480 */
    s_grid = lv_obj_create(s_root_lib);
    lv_obj_remove_style_all(s_grid);
    lv_obj_set_size(s_grid, 480, CARD_H + 8);
    lv_obj_set_pos(s_grid, 0, 64);
    lv_obj_set_style_pad_left(s_grid, 22, 0);
    lv_obj_set_style_pad_column(s_grid, 12, 0);
    lv_obj_set_flex_flow(s_grid, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(s_grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_grid, LV_OBJ_FLAG_GESTURE_BUBBLE);

    s_prev = ui_round_btn(s_root_lib, UI_IC_BACK, 40, UI_ROUND_OUTLINE, on_library_page, (void *)(intptr_t)-1);
    lv_obj_set_pos(s_prev, 22, 262);
    s_next = ui_round_btn(s_root_lib, UI_IC_NEXT, 40, UI_ROUND_OUTLINE, on_library_page, (void *)(intptr_t)1);
    lv_obj_set_pos(s_next, 480 - 22 - 40, 262);
    s_dots = lv_obj_create(s_root_lib);
    lv_obj_remove_style_all(s_dots);
    lv_obj_set_size(s_dots, 240, 12);
    lv_obj_align(s_dots, LV_ALIGN_TOP_MID, 0, 276);
    lv_obj_set_flex_flow(s_dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_dots, 8, 0);
    s_page_text = lv_label_create(s_root_lib);
    lv_obj_set_style_text_color(s_page_text, lv_color_hex(UI_COL_TEXT_DIM), 0);
    lv_obj_align(s_page_text, LV_ALIGN_TOP_MID, 0, 271);
    /* 选择模式底部操作条（两侧翻页箭头之间）：收藏 / 取消收藏 / 删除 */
    s_sel_bar = lv_obj_create(s_root_lib);
    lv_obj_remove_style_all(s_sel_bar);
    lv_obj_set_size(s_sel_bar, LV_SIZE_CONTENT, 50);
    lv_obj_set_flex_flow(s_sel_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_sel_bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_sel_bar, 8, 0);
    lv_obj_align(s_sel_bar, LV_ALIGN_TOP_MID, 0, 258);
    s_fav_add_btn = ui_chunky_btn(s_sel_bar, UI_IC_HEART_F, "收藏", UI_COL_PINK, UI_COL_PINK_DK, UI_COL_WHITE,
                                  104, 46, on_fav_selected, (void *)1);
    s_fav_rm_btn = ui_chunky_btn(s_sel_bar, UI_IC_HEART, "取消收藏", UI_COL_WHITE, UI_COL_BORDER, UI_COL_TEXT,
                                 128, 46, on_fav_selected, (void *)0);
    s_delete_btn = ui_chunky_btn(s_sel_bar, UI_IC_TRASH, "删除", UI_COL_DANGER, 0xC2453E, UI_COL_WHITE,
                                 104, 46, on_delete_selected, NULL);
    lv_obj_t *btns[3] = { s_fav_add_btn, s_fav_rm_btn, s_delete_btn };
    for (int i = 0; i < 3; i++) lv_obj_set_style_opa(btns[i], LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_add_flag(s_sel_bar, LV_OBJ_FLAG_HIDDEN);

    s_scan_label = lv_label_create(s_root_lib);
    lv_label_set_text(s_scan_label, "正在查找故事…");
    lv_obj_set_style_text_color(s_scan_label, lv_color_hex(UI_COL_TEXT_DIM), 0);
    lv_obj_center(s_scan_label);
    lv_obj_add_flag(s_scan_label, LV_OBJ_FLAG_HIDDEN);

    s_empty_box = lv_obj_create(s_root_lib);
    lv_obj_remove_style_all(s_empty_box);
    lv_obj_set_size(s_empty_box, 480, 264);
    lv_obj_set_pos(s_empty_box, 0, 56);
    const lv_image_dsc_t *fox = ui_asset(UI_ASSET_FOX_EMPTY);
    if (fox) {
        lv_obj_t *img = lv_image_create(s_empty_box);
        lv_image_set_src(img, fox);
        lv_obj_align(img, LV_ALIGN_TOP_MID, 0, 6);
    }
    s_empty_t1 = lv_label_create(s_empty_box);
    lv_label_set_text(s_empty_t1, "这里还没有故事哦");
    lv_obj_set_style_text_font(s_empty_t1, ui_font_kid(24), 0);
    lv_obj_align(s_empty_t1, LV_ALIGN_TOP_MID, 0, 128);
    s_empty_t2 = lv_label_create(s_empty_box);
    lv_label_set_text(s_empty_t2, "拍下第一页绘本，让小狐狸讲给你听");
    lv_obj_set_style_text_color(s_empty_t2, lv_color_hex(UI_COL_TEXT_DIM), 0);
    lv_obj_align(s_empty_t2, LV_ALIGN_TOP_MID, 0, 162);
    s_empty_go = ui_chunky_btn(s_empty_box, UI_IC_CAMERA, "去拍照", UI_COL_ORANGE, UI_COL_ORANGE_DK,
                               UI_COL_WHITE, 160, 48, on_empty_go, NULL);
    lv_obj_align(s_empty_go, LV_ALIGN_TOP_MID, 0, 196);
    lv_obj_add_flag(s_empty_box, LV_OBJ_FLAG_HIDDEN);
    return s_root_lib;
}

void screen_library_reload(void)
{
    rebuild_grid();
}

void screen_library_on_enter(void)
{
    rebuild_grid();
}

void on_back_camera(lv_event_t *e)
{
    (void)e;
    ui_navigator_back();
}

/* ---------------- 详情 ---------------- */

static lv_obj_t *s_root_detail, *s_detail_title, *s_detail_photo, *s_chips;
static lv_obj_t *s_btn_primary, *s_btn_secondary, *s_btn_delete, *s_btn_fav;
static bool s_detail_unfinished;

static void on_detail_fav(lv_event_t *e)
{
    (void)e;
    const story_summary_t *s = app_ui_current_story();
    bool fav = !s->favorite;
    ui_ops_set_favorite(s->story_id, fav);   /* 同时更新 current story */
    ui_heart_btn_set(s_btn_fav, fav);
    ui_toast(fav ? "已收藏" : "已取消收藏");
}

static void add_chip(const char *text)
{
    lv_obj_t *c = lv_obj_create(s_chips);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, LV_SIZE_CONTENT, 26);
    lv_obj_set_style_pad_hor(c, 10, 0);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(UI_COL_WHITE), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(UI_COL_BORDER), 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_t *l = lv_label_create(c);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_hex(UI_COL_TEXT_DIM), 0);
    lv_obj_center(l);
}

static void start_generation(bool regenerate)
{
    const story_summary_t *s = app_ui_current_story();
    esp_err_t err = story_start_generation(s->story_id, regenerate);
    if (err == ESP_OK) {
        screen_generating_set_story_id(s->story_id);
        ui_navigator_go(SCREEN_GENERATING);
    } else if (err == ESP_ERR_NOT_SUPPORTED) {
        ui_dialog_show_error("VERSION_LIMIT", false);
    } else if (network_state() < NET_STATE_GOT_IP) {
        ui_dialog_show_error("OFFLINE", false);
    }
    /* 其余情况（未确认联网说明）由 NEEDS_CONFIRMATION 事件弹窗 */
}

static void resume_generation(void)
{
    const story_summary_t *s = app_ui_current_story();
    if (story_resume_generation(s->story_id) == ESP_OK) {
        screen_generating_set_story_id(s->story_id);
        ui_navigator_go(SCREEN_GENERATING);
    } else {
        ui_dialog_show_error(network_state() < NET_STATE_GOT_IP ? "OFFLINE" : "NETWORK_TIMEOUT", false);
    }
}

static void on_detail_primary(lv_event_t *e)
{
    (void)e;
    const story_summary_t *s = app_ui_current_story();
    if (s->availability == STORY_AVAIL_PLAYABLE) screen_player_open(s);
    else if (s_detail_unfinished) resume_generation();
    else start_generation(false);
}

static void on_detail_secondary(lv_event_t *e)
{
    (void)e;
    if (s_detail_unfinished) resume_generation();
    else ui_dialog_confirm_regen(app_ui_current_story()->story_id);
}

void ui_detail_regen_confirmed(void)
{
    start_generation(true);
}

static void on_delete_tap(lv_event_t *e)
{
    (void)e;
    ui_dialog_confirm_delete(app_ui_current_story()->story_id);
}

static void on_detail_back(lv_event_t *e)
{
    (void)e;
    on_library_open(NULL);
}

lv_obj_t *screen_detail_create(void)
{
    s_root_detail = lv_obj_create(ui_screens_parent());
    lv_obj_set_style_bg_color(s_root_detail, lv_color_hex(UI_COL_BG), 0);
    ui_page_header(s_root_detail, UI_IC_BACK, "故事详情", on_detail_back);
    s_btn_fav = ui_round_btn(s_root_detail, UI_IC_HEART, 44, UI_ROUND_OUTLINE, on_detail_fav, NULL);
    lv_obj_set_pos(s_btn_fav, 480 - 16 - 44, 8);

    lv_obj_t *frame = lv_obj_create(s_root_detail);
    lv_obj_remove_style_all(frame);
    lv_obj_set_size(frame, 228, 155);
    lv_obj_set_pos(frame, 16, 66);
    lv_obj_set_style_radius(frame, 18, 0);
    lv_obj_set_style_clip_corner(frame, true, 0);
    lv_obj_set_style_bg_color(frame, lv_color_hex(UI_COL_BORDER), 0);
    lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(frame, lv_color_hex(UI_COL_WHITE), 0);
    lv_obj_set_style_border_width(frame, 4, 0);
    s_detail_photo = lv_image_create(frame);

    s_detail_title = lv_label_create(s_root_detail);
    lv_label_set_long_mode(s_detail_title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(s_detail_title, ui_font_cn(22), 0);
    lv_obj_set_size(s_detail_title, 210, lv_font_get_line_height(ui_font_cn(22)));
    lv_obj_set_pos(s_detail_title, 258, 68);

    s_chips = lv_obj_create(s_root_detail);
    lv_obj_remove_style_all(s_chips);
    lv_obj_set_size(s_chips, 210, LV_SIZE_CONTENT);
    lv_obj_set_pos(s_chips, 258, 102);
    lv_obj_set_flex_flow(s_chips, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(s_chips, 6, 0);
    lv_obj_set_style_pad_row(s_chips, 6, 0);

    s_btn_primary = ui_chunky_btn(s_root_detail, UI_IC_PLAY, "播放故事", UI_COL_GREEN, UI_COL_GREEN_DK,
                                  UI_COL_WHITE, 206, 54, on_detail_primary, NULL);
    lv_obj_set_pos(s_btn_primary, 258, 168);

    s_btn_secondary = ui_chunky_btn(s_root_detail, UI_IC_REFRESH, "重新生成", UI_COL_WHITE, UI_COL_BORDER,
                                    UI_COL_TEXT, 132, 44, on_detail_secondary, NULL);
    lv_obj_set_pos(s_btn_secondary, 16, 252);
    s_btn_delete = ui_chunky_btn(s_root_detail, UI_IC_TRASH, "删除", UI_COL_WHITE, UI_COL_BORDER,
                                 UI_COL_DANGER, 104, 44, on_delete_tap, NULL);
    lv_obj_set_pos(s_btn_delete, 160, 252);
    return s_root_detail;
}

void screen_detail_open(const story_summary_t *s)
{
    app_ui_set_current_story(s);
    ui_navigator_go(SCREEN_DETAIL);
    bool playable = s->availability == STORY_AVAIL_PLAYABLE;
    s_detail_unfinished = has_unfinished(s);
    lv_label_set_text(s_detail_title, playable && s->title[0] ? s->title : "还没有故事");
    bind_cover(s_detail_photo, s->thumb_path, 220, 147);
    ui_heart_btn_set(s_btn_fav, s->favorite);

    lv_obj_clean(s_chips);
    char when[32];
    add_chip(format_capture_time(s, "%m-%d %H:%M", when, sizeof(when)) ? when : "时间未知");
    if (s->current_version > 0) {
        char v[24];
        snprintf(v, sizeof(v), "第 %d 版", s->current_version);
        add_chip(v);
    } else {
        add_chip("照片已保存");
    }

    if (playable) {
        ui_chunky_set(s_btn_primary, UI_IC_PLAY, "播放故事", UI_COL_GREEN, UI_COL_GREEN_DK);
        ui_chunky_set(s_btn_secondary, UI_IC_REFRESH, s_detail_unfinished ? "继续生成" : "重新生成",
                      UI_COL_WHITE, UI_COL_BORDER);
        lv_obj_remove_flag(s_btn_secondary, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_btn_delete, 160, 252);
    } else {
        ui_chunky_set(s_btn_primary, s_detail_unfinished ? UI_IC_REFRESH : UI_IC_SPARK,
                      s_detail_unfinished ? "继续生成" : "生成故事", UI_COL_ORANGE, UI_COL_ORANGE_DK);
        lv_obj_add_flag(s_btn_secondary, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_btn_delete, 16, 252);
    }
}
