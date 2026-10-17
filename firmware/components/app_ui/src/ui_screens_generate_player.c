/**
 * @file ui_screens_generate_player.c
 * @brief 生成中（画面4）与全屏播放页（画面5，v0.2）。
 *
 * 生成中：背景为刚拍的这一页（60x40 放大即模糊）+ 奶油色蒙层；阅读狐狸 + 星星；
 * 五步只反映 story_service 真实事件（未完成不打勾，当前步转圈）；
 * "先去玩别的"回首页（story_cancel_wait，生成在后台继续）；失败弹窗就地出现。
 * 播放：照片铺满全屏（original.jpg 解码裁成 480x320），顶部返回/标题/更多，
 * 底部奶油色控制条：从头播放、播放暂停、只读进度、音量（弹出滑条）；
 * 生成完成的自动播放先说"故事准备好啦！"；播完弹出"讲完啦！"卡片。
 * 离开播放页即停止播放（实施计划 9.3）。
 */

#include "ui_screens.h"
#include "ui_story_ops.h"
#include "ui_assets.h"
#include "ui_widgets.h"
#include "ui_sound.h"
#include "app_ui.h"
#include "assets.h"
#include "audio_service.h"
#include "story_service.h"
#include "settings_service.h"
#include "storage_service.h"

#include "lvgl.h"
#include <string.h>
#include <stdio.h>

void on_library_open(lv_event_t *e);
void on_go_home(lv_event_t *e);
void ui_dialog_show_error_cb(const char *code, bool retryable, const char *story_id);
void ui_dialog_show_rejected(const char *story_id);

/* ---------------- 生成中 ---------------- */

typedef struct { lv_obj_t *row, *dot, *check, *spin, *label; } step_ui_t;
static step_ui_t s_steps[5];
static lv_obj_t *s_gen_bg;
static char s_gen_story[STORAGE_ID_HEX];

static const char *k_steps[] = {
    "照片已保存", "正在理解画面", "生成故事内容", "准备声音", "马上就好",
};

static void set_step(int done_count)
{
    for (int i = 0; i < 5; i++) {
        step_ui_t *s = &s_steps[i];
        bool done = i < done_count, active = i == done_count;
        lv_obj_set_style_bg_opa(s->row, active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(s->dot, lv_color_hex(UI_COL_GREEN), 0);
        lv_obj_set_style_bg_opa(s->dot, done ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(s->dot, lv_color_hex(done ? UI_COL_GREEN : UI_COL_MUTE), 0);
        lv_obj_set_style_border_width(s->dot, active ? 0 : 3, 0);
        if (done) lv_obj_remove_flag(s->check, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s->check, LV_OBJ_FLAG_HIDDEN);
        if (active) lv_obj_remove_flag(s->spin, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s->spin, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(s->label, lv_color_hex(done || active ? UI_COL_TEXT : UI_COL_TEXT_DIM), 0);
    }
}

void screen_generating_set_story_id(const char *story_id)
{
    if (!story_id || !story_id[0] || !s_gen_bg) return;
    if (strcmp(story_id, s_gen_story) == 0) return;
    strlcpy(s_gen_story, story_id, sizeof(s_gen_story));
    char thumb[192];
    const lv_image_dsc_t *blur = NULL;
    if (storage_get_thumb_path(story_id, thumb, sizeof(thumb)) == ESP_OK) blur = ui_photo_blur(thumb);
    if (blur) {
        lv_image_set_src(s_gen_bg, blur);
        lv_obj_remove_flag(s_gen_bg, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_gen_bg, LV_OBJ_FLAG_HIDDEN);
    }
}

static void twinkle_cb(void *obj, int32_t v)
{
    lv_obj_set_style_image_opa(obj, (lv_opa_t)v, 0);
}

static void add_star(lv_obj_t *root, const lv_image_dsc_t *src, int x, int y, uint32_t delay)
{
    lv_obj_t *st = lv_image_create(root);
    lv_image_set_src(st, src);
    lv_obj_set_pos(st, x, y);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, st);
    lv_anim_set_values(&a, LV_OPA_COVER, 90);
    lv_anim_set_duration(&a, 900);
    lv_anim_set_playback_duration(&a, 900);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&a, twinkle_cb);
    lv_anim_start(&a);
}

void on_stop_wait(lv_event_t *e)
{
    (void)e;
    story_cancel_wait();               /* 令牌失效：生成在后台继续，完成后不自动播放 */
    ui_navigator_go(SCREEN_HOME);
}

lv_obj_t *screen_generating_create(void)
{
    lv_obj_t *root = lv_obj_create(ui_screens_parent());
    lv_obj_set_style_bg_color(root, lv_color_hex(UI_COL_BG), 0);

    s_gen_bg = lv_image_create(root);
    lv_obj_set_size(s_gen_bg, 480, 320);
    lv_obj_set_pos(s_gen_bg, 0, 0);
    lv_image_set_inner_align(s_gen_bg, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_add_flag(s_gen_bg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *veil = lv_obj_create(root);
    lv_obj_remove_style_all(veil);
    lv_obj_set_size(veil, 480, 320);
    lv_obj_set_style_bg_color(veil, lv_color_hex(UI_COL_BG), 0);
    lv_obj_set_style_bg_opa(veil, 195, 0);

    lv_obj_t *stop = ui_chunky_btn(root, UI_IC_BACK, "先去玩别的", UI_COL_WHITE, UI_COL_BORDER,
                                   UI_COL_TEXT, 136, 42, on_stop_wait, NULL);
    lv_obj_set_pos(stop, 12, 12);

    lv_obj_t *title = lv_label_create(root);
    lv_label_set_text(title, "我正在看看这一页…");
    lv_obj_set_style_text_font(title, ui_font_kid(24), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_set_pos(title, 168, 20);

    lv_obj_t *fox = lv_image_create(root);
    lv_image_set_src(fox, &img_fox_reading_t);
    lv_obj_set_pos(fox, 30, 124);
    add_star(root, &img_star_large, 200, 96, 0);
    add_star(root, &img_star_small, 24, 112, 600);
    add_star(root, &img_star_small, 214, 250, 1100);

    lv_obj_t *list = lv_obj_create(root);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, 216, LV_SIZE_CONTENT);
    lv_obj_set_pos(list, 248, 66);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 6, 0);
    for (int i = 0; i < 5; i++) {
        step_ui_t *s = &s_steps[i];
        s->row = lv_obj_create(list);
        lv_obj_remove_style_all(s->row);
        lv_obj_set_size(s->row, LV_SIZE_CONTENT, 38);
        lv_obj_set_style_radius(s->row, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s->row, lv_color_hex(UI_COL_WHITE), 0);
        lv_obj_set_style_pad_left(s->row, 6, 0);
        lv_obj_set_style_pad_right(s->row, 14, 0);
        lv_obj_set_flex_flow(s->row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(s->row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(s->row, 10, 0);
        s->dot = lv_obj_create(s->row);
        lv_obj_remove_style_all(s->dot);
        lv_obj_set_size(s->dot, 26, 26);
        lv_obj_set_style_radius(s->dot, LV_RADIUS_CIRCLE, 0);
        s->check = ui_icon_create(s->dot, UI_IC_CHECK, 20, UI_COL_WHITE);
        lv_obj_center(s->check);
        s->spin = lv_spinner_create(s->dot);
        lv_obj_set_size(s->spin, 26, 26);
        lv_obj_center(s->spin);
        lv_obj_set_style_arc_width(s->spin, 4, 0);
        lv_obj_set_style_arc_width(s->spin, 4, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(s->spin, lv_color_hex(UI_COL_BORDER), 0);
        lv_obj_set_style_arc_color(s->spin, lv_color_hex(UI_COL_ORANGE), LV_PART_INDICATOR);
        s->label = lv_label_create(s->row);
        lv_label_set_text(s->label, k_steps[i]);
    }
    set_step(0);
    return root;
}

void screen_generating_on_enter(void)
{
    set_step(0);                       /* 进入时只有第一步在进行（事件驱动更新） */
    if (!s_gen_story[0]) screen_generating_set_story_id(app_ui_current_story()->story_id);
    ui_sound_prompt(UI_PROMPT_LOOKING);
}

void screen_generating_on_exit(void)
{
    s_gen_story[0] = '\0';             /* 下次进入按新故事重新加载背景 */
}

/** 真实状态 → 完成步数（未知进度不打勾；阶段由事件驱动） */
static int steps_for_state(operation_state_t st)
{
    switch (st) {
    case OP_UPLOADING: case OP_WAITING_NETWORK:
    case OP_NEEDS_CONFIRMATION: case OP_REMOTE_PENDING: return 1;
    case OP_DOWNLOADING: case OP_COMMITTING: return 4;
    case OP_COMPLETED: return 5;
    default: return 0;
    }
}

void screen_generating_on_story_event(const story_event_t *ev)
{
    screen_generating_set_story_id(ev->story_id);
    int completed = steps_for_state(ev->state);
    if (ev->state == OP_REMOTE_PENDING) {
        if (strcmp(ev->stage, "story_generating") == 0) completed = 2;
        else if (strcmp(ev->stage, "tts_generating") == 0) completed = 3;
        else if (strcmp(ev->stage, "ready") == 0) completed = 4;
    }
    if (completed) set_step(completed);
    if (ev->state == OP_CONTENT_REJECTED) {
        ui_dialog_show_rejected(ev->story_id);
    } else if (ev->state == OP_FAILED || ev->state == OP_WAITING_USER) {
        ui_dialog_show_error_cb(ev->error_code[0] ? ev->error_code : "NETWORK_TIMEOUT",
                                ev->state == OP_WAITING_USER ? true : ev->error_retryable, ev->story_id);
    }
}

/* ---------------- 播放 ---------------- */

static lv_obj_t *s_photo_img, *s_title_label, *s_track, *s_track_fill, *s_time_label;
static lv_obj_t *s_btn_play, *s_play_icon, *s_vol_panel, *s_vol_slider, *s_endcard;
static lv_timer_t *s_progress_timer, *s_vol_hide_timer;
static story_summary_t s_playing;
static bool s_audio_error_shown, s_intro_pending, s_story_started, s_end_shown;

/* 上一个 / 下一个（故事库当前视图里能听的故事，循环）与播完自动下一个 */
#define AUTO_NEXT_S 5          /* 结束语说完后再倒数的秒数 */
static lv_obj_t *s_btn_prev, *s_btn_next, *s_end_next;
static lv_timer_t *s_auto_next_timer;
static int s_auto_next_left;

static void play_neighbor(int dir)
{
    story_summary_t nb;
    if (screen_library_neighbor(s_playing.story_id, dir, &nb)) screen_player_open(&nb);
}

static void auto_next_stop(void)
{
    if (s_auto_next_timer) {
        lv_timer_delete(s_auto_next_timer);
        s_auto_next_timer = NULL;
    }
}

static void auto_next_label(void)
{
    char t[24];
    snprintf(t, sizeof(t), "下一个 (%d)", s_auto_next_left);
    ui_chunky_set(s_end_next, UI_IC_SKIP_NEXT, s_auto_next_timer ? t : "下一个", UI_COL_GREEN, UI_COL_GREEN_DK);
}

static void auto_next_tick(lv_timer_t *t)
{
    (void)t;
    if (audio_clip_active()) return;    /* "讲完啦，还想再听一遍吗？"说完再开始倒数 */
    if (--s_auto_next_left > 0) {
        auto_next_label();
        return;
    }
    auto_next_stop();
    play_neighbor(1);
}

static void show_endcard(bool show)
{
    s_end_shown = show;
    auto_next_stop();
    if (!show) {
        lv_obj_add_flag(s_endcard, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    story_summary_t nb;
    bool has_next = screen_library_neighbor(s_playing.story_id, 1, &nb);
    if (has_next) {
        /* 默认"下一个"：倒计时结束自动播放，点其他选项即取消 */
        lv_obj_remove_flag(s_end_next, LV_OBJ_FLAG_HIDDEN);
        s_auto_next_left = AUTO_NEXT_S;
        s_auto_next_timer = lv_timer_create(auto_next_tick, 1000, NULL);
    } else {
        lv_obj_add_flag(s_end_next, LV_OBJ_FLAG_HIDDEN);
    }
    if (has_next) auto_next_label();
    lv_obj_remove_flag(s_endcard, LV_OBJ_FLAG_HIDDEN);
}

static void on_prev(lv_event_t *e)
{
    (void)e;
    play_neighbor(-1);
}

static void on_next(lv_event_t *e)
{
    (void)e;
    play_neighbor(1);
}

static void on_player_gesture(lv_event_t *e)
{
    (void)e;
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_LEFT) play_neighbor(1);
    else if (dir == LV_DIR_RIGHT) play_neighbor(-1);
}

static void start_story(void)
{
    s_intro_pending = false;
    s_story_started = true;
    s_audio_error_shown = false;
    show_endcard(false);
    if (story_play(s_playing.story_id) != ESP_OK) {
        s_audio_error_shown = true;
        ui_dialog_show_error_cb("AUDIO_DECODE_FAILED", false, s_playing.story_id);
    }
}

static void refresh_progress(lv_timer_t *t)
{
    (void)t;
    if (!s_track) return;
    if (s_intro_pending) {
        /* 先说完"故事准备好啦！"再开始故事 */
        if (!ui_sound_prompt_busy()) start_story();
        return;
    }
    if (audio_clip_active()) return;   /* 正在说提示语（如"讲完啦"），不是故事本身 */
    audio_state_t state = audio_get_state();
    ui_icon_set(s_play_icon, state == AUDIO_PLAYING || state == AUDIO_LOADING ? UI_IC_PAUSE : UI_IC_PLAY, 28);
    if (state == AUDIO_ERROR && !s_audio_error_shown && s_story_started) {
        s_audio_error_shown = true;
        ui_dialog_show_error_cb("AUDIO_DECODE_FAILED", false, s_playing.story_id);
    }
    int permille = audio_progress_permille();
    uint32_t total = audio_duration_ms();
    if (state == AUDIO_STOPPED && s_story_started && permille >= 990 && !s_end_shown) {
        s_story_started = false;
        show_endcard(true);
        ui_sound_prompt(UI_PROMPT_DONE);
    }
    if (state == AUDIO_STOPPED && s_end_shown) permille = 1000;
    lv_obj_set_width(s_track_fill, (int32_t)lv_obj_get_width(s_track) * permille / 1000);
    uint32_t cur = total * (uint32_t)permille / 1000;
    lv_label_set_text_fmt(s_time_label, "%02u:%02u / %02u:%02u",
                          (unsigned)(cur / 60000), (unsigned)((cur / 1000) % 60),
                          (unsigned)(total / 60000), (unsigned)((total / 1000) % 60));
}

static void on_play_pause(lv_event_t *e)
{
    (void)e;
    if (s_intro_pending || audio_clip_active()) { start_story(); return; }
    audio_state_t state = audio_get_state();
    if (state == AUDIO_PAUSED) audio_resume();
    else if (state == AUDIO_PLAYING) audio_pause();
    else if (state != AUDIO_LOADING) start_story();
    refresh_progress(NULL);
}

static void on_restart(lv_event_t *e)
{
    (void)e;
    if (s_playing.story_id[0]) start_story();
}

static void on_endcard_back(lv_event_t *e)
{
    (void)e;
    show_endcard(false);
    on_library_open(NULL);
}

static void vol_hide(lv_timer_t *t)
{
    (void)t;
    lv_obj_add_flag(s_vol_panel, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(s_vol_hide_timer);
}

static void on_volume_toggle(lv_event_t *e)
{
    (void)e;
    if (lv_obj_has_flag(s_vol_panel, LV_OBJ_FLAG_HIDDEN)) {
        product_settings_t set;
        settings_load_product(&set);
        lv_slider_set_value(s_vol_slider, set.volume, LV_ANIM_OFF);
        lv_obj_remove_flag(s_vol_panel, LV_OBJ_FLAG_HIDDEN);
        lv_timer_reset(s_vol_hide_timer);
        lv_timer_resume(s_vol_hide_timer);
    } else {
        vol_hide(NULL);
    }
}

static void on_volume(lv_event_t *e)
{
    uint8_t vol = (uint8_t)lv_slider_get_value(s_vol_slider);
    audio_set_volume(vol);
    lv_timer_reset(s_vol_hide_timer);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        product_settings_t set;
        settings_load_product(&set);
        set.volume = vol;
        settings_save_product(&set);
    }
}

static lv_obj_t *s_player_fav;

static void on_player_fav(lv_event_t *e)
{
    (void)e;
    s_playing.favorite = !s_playing.favorite;
    ui_ops_set_favorite(s_playing.story_id, s_playing.favorite);
    ui_heart_btn_set(s_player_fav, s_playing.favorite);
    ui_toast(s_playing.favorite ? "已收藏" : "已取消收藏");
}

static void on_more(lv_event_t *e)
{
    (void)e;
    screen_detail_open(&s_playing);    /* on_exit 停止播放 */
}

void screen_player_on_exit(void)
{
    s_intro_pending = false;
    s_story_started = false;
    story_stop_playback();             /* 离开播放页即停止（实施计划 9.3） */
    if (s_progress_timer) {
        lv_timer_delete(s_progress_timer);
        s_progress_timer = NULL;
    }
    show_endcard(false);
    if (s_vol_panel) lv_obj_add_flag(s_vol_panel, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *style_slider(lv_obj_t *slider)
{
    lv_obj_remove_style_all(slider);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(UI_COL_BORDER), LV_PART_MAIN);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(UI_COL_SKY), LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_bg_color(slider, lv_color_hex(UI_COL_WHITE), LV_PART_KNOB);
    lv_obj_set_style_border_color(slider, lv_color_hex(UI_COL_SKY), LV_PART_KNOB);
    lv_obj_set_style_border_width(slider, 3, LV_PART_KNOB);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 7, LV_PART_KNOB);
    lv_obj_set_ext_click_area(slider, 14);
    return slider;
}

lv_obj_t *screen_player_create(void)
{
    lv_obj_t *root = lv_obj_create(ui_screens_parent());
    lv_obj_set_style_bg_color(root, lv_color_hex(0x1A1410), 0);

    s_photo_img = lv_image_create(root);
    lv_obj_set_size(s_photo_img, 480, 320);
    lv_obj_set_pos(s_photo_img, 0, 0);

    lv_obj_t *fade = lv_obj_create(root);
    lv_obj_remove_style_all(fade);
    lv_obj_set_size(fade, 480, 84);
    lv_obj_set_style_bg_color(fade, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_grad_color(fade, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_grad_dir(fade, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_opa(fade, 110, 0);
    lv_obj_set_style_bg_grad_opa(fade, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(fade, LV_OPA_COVER, 0);

    lv_obj_t *back = ui_round_btn(root, UI_IC_BACK, 44, UI_ROUND_GLASS, on_library_open, NULL);
    lv_obj_set_pos(back, 12, 12);
    lv_obj_t *pill = lv_obj_create(root);
    lv_obj_remove_style_all(pill);
    lv_obj_set_size(pill, LV_SIZE_CONTENT, 36);
    lv_obj_set_style_max_width(pill, 340, 0);
    lv_obj_set_style_pad_hor(pill, 16, 0);
    lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(pill, lv_color_hex(UI_COL_SMOKE), 0);
    lv_obj_set_style_bg_opa(pill, 140, 0);
    lv_obj_set_pos(pill, 66, 16);
    s_title_label = lv_label_create(pill);
    lv_obj_set_style_text_font(s_title_label, ui_font_cn(22), 0);
    lv_obj_set_style_text_color(s_title_label, lv_color_hex(UI_COL_WHITE), 0);
    lv_label_set_text(s_title_label, "…");
    lv_obj_center(s_title_label);
    lv_obj_t *more = ui_round_btn(root, UI_IC_MORE, 44, UI_ROUND_GLASS, on_more, NULL);
    lv_obj_set_pos(more, 480 - 12 - 44, 12);
    s_player_fav = ui_round_btn(root, UI_IC_HEART, 44, UI_ROUND_GLASS, on_player_fav, NULL);
    lv_obj_set_pos(s_player_fav, 480 - 12 - 44 - 8 - 44, 12);

    /* 上一个 / 下一个：贴两侧边缘、画面中部，半透明不挡画面，热区比外观大一圈；也可左右滑动 */
    s_btn_prev = ui_round_btn(root, UI_IC_SKIP_PREV, 48, UI_ROUND_GLASS, on_prev, NULL);
    s_btn_next = ui_round_btn(root, UI_IC_SKIP_NEXT, 48, UI_ROUND_GLASS, on_next, NULL);
    lv_obj_set_pos(s_btn_prev, 8, 124);
    lv_obj_set_pos(s_btn_next, 480 - 8 - 48, 124);
    lv_obj_t *nav[2] = { s_btn_prev, s_btn_next };
    for (int i = 0; i < 2; i++) {
        lv_obj_set_style_bg_opa(nav[i], 150, 0);
        lv_obj_set_style_bg_opa(nav[i], 230, LV_STATE_PRESSED);
        lv_obj_set_ext_click_area(nav[i], 18);
    }
    lv_obj_add_event_cb(root, on_player_gesture, LV_EVENT_GESTURE, NULL);

    lv_obj_t *bar = lv_obj_create(root);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 456, 72);
    lv_obj_set_pos(bar, 12, 236);
    lv_obj_set_style_radius(bar, 26, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_COL_BG), 0);
    lv_obj_set_style_bg_opa(bar, 242, 0);
    lv_obj_set_style_pad_hor(bar, 12, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 12, 0);

    lv_obj_t *re = ui_round_btn(bar, UI_IC_REPLAY, 44, UI_ROUND_CREAM, on_restart, NULL);
    lv_obj_set_style_image_recolor(lv_obj_get_child(re, 0), lv_color_hex(UI_COL_ORANGE_DK), 0);

    s_btn_play = lv_obj_create(bar);
    lv_obj_remove_style_all(s_btn_play);
    lv_obj_set_size(s_btn_play, 58, 58);
    lv_obj_add_flag(s_btn_play, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(s_btn_play, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_btn_play, lv_color_hex(UI_COL_GREEN), 0);
    lv_obj_set_style_bg_opa(s_btn_play, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(s_btn_play, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(s_btn_play, lv_color_hex(UI_COL_GREEN_DK), 0);
    lv_obj_set_style_border_width(s_btn_play, 4, 0);
    lv_obj_set_style_translate_y(s_btn_play, 2, LV_STATE_PRESSED);
    s_play_icon = ui_icon_create(s_btn_play, UI_IC_PAUSE, 28, UI_COL_WHITE);
    lv_obj_center(s_play_icon);
    lv_obj_add_event_cb(s_btn_play, on_play_pause, LV_EVENT_CLICKED, NULL);

    lv_obj_t *prog = lv_obj_create(bar);
    lv_obj_remove_style_all(prog);
    lv_obj_set_height(prog, 44);
    lv_obj_set_flex_grow(prog, 1);
    lv_obj_set_flex_flow(prog, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(prog, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(prog, 6, 0);
    s_track = lv_obj_create(prog);
    lv_obj_remove_style_all(s_track);
    lv_obj_set_size(s_track, LV_PCT(100), 10);
    lv_obj_set_style_radius(s_track, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_track, lv_color_hex(UI_COL_BORDER), 0);
    lv_obj_set_style_bg_opa(s_track, LV_OPA_COVER, 0);
    lv_obj_set_style_clip_corner(s_track, true, 0);
    s_track_fill = lv_obj_create(s_track);
    lv_obj_remove_style_all(s_track_fill);
    lv_obj_set_size(s_track_fill, 0, 10);
    lv_obj_set_style_radius(s_track_fill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_track_fill, lv_color_hex(UI_COL_GREEN), 0);
    lv_obj_set_style_bg_opa(s_track_fill, LV_OPA_COVER, 0);
    s_time_label = lv_label_create(prog);
    lv_obj_set_style_text_color(s_time_label, lv_color_hex(UI_COL_TEXT_DIM), 0);
    lv_label_set_text(s_time_label, "00:00 / 00:00");

    ui_round_btn(bar, UI_IC_VOLUME, 44, UI_ROUND_CREAM, on_volume_toggle, NULL);

    /* 音量弹出条（控制条上方，3 秒无操作自动收起） */
    s_vol_panel = lv_obj_create(root);
    lv_obj_remove_style_all(s_vol_panel);
    lv_obj_set_size(s_vol_panel, 236, 52);
    lv_obj_set_pos(s_vol_panel, 480 - 12 - 236, 176);
    lv_obj_set_style_radius(s_vol_panel, 26, 0);
    lv_obj_set_style_bg_color(s_vol_panel, lv_color_hex(UI_COL_WHITE), 0);
    lv_obj_set_style_bg_opa(s_vol_panel, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_vol_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *vic = ui_icon_create(s_vol_panel, UI_IC_VOLUME, 24, UI_COL_SKY_DK);
    lv_obj_align(vic, LV_ALIGN_LEFT_MID, 14, 0);
    s_vol_slider = style_slider(lv_slider_create(s_vol_panel));
    lv_obj_set_size(s_vol_slider, 164, 10);
    lv_obj_align(s_vol_slider, LV_ALIGN_LEFT_MID, 52, 0);
    lv_slider_set_range(s_vol_slider, 0, 100);
    lv_obj_add_event_cb(s_vol_slider, on_volume, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_vol_slider, on_volume, LV_EVENT_RELEASED, NULL);
    lv_obj_add_flag(s_vol_panel, LV_OBJ_FLAG_HIDDEN);
    s_vol_hide_timer = lv_timer_create(vol_hide, 3000, NULL);
    lv_timer_pause(s_vol_hide_timer);

    /* 播完卡片 */
    s_endcard = lv_obj_create(root);
    lv_obj_remove_style_all(s_endcard);
    lv_obj_set_size(s_endcard, 480, 320);
    lv_obj_add_flag(s_endcard, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(s_endcard, lv_color_hex(UI_COL_SMOKE), 0);
    lv_obj_set_style_bg_opa(s_endcard, 120, 0);
    lv_obj_t *card = lv_obj_create(s_endcard);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 464, 250);
    lv_obj_center(card);
    lv_obj_set_style_radius(card, 28, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(UI_COL_BG), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, 8, 0);
    lv_obj_t *fx = lv_image_create(card);
    lv_image_set_src(fx, &img_fox_clap);
    lv_obj_t *et = lv_label_create(card);
    lv_label_set_text(et, "讲完啦！");
    lv_obj_set_style_text_font(et, ui_font_kid(32), 0);
    lv_obj_set_style_text_color(et, lv_color_hex(UI_COL_ORANGE_DK), 0);
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 10, 0);
    ui_chunky_btn(row, UI_IC_REPLAY, "重新播放", UI_COL_WHITE, UI_COL_BORDER, UI_COL_TEXT, 124, 50, on_restart, NULL);
    s_end_next = ui_chunky_btn(row, UI_IC_SKIP_NEXT, "下一个", UI_COL_GREEN, UI_COL_GREEN_DK, UI_COL_WHITE,
                               142, 50, on_next, NULL);
    ui_chunky_btn(row, UI_IC_BOOKS, "返回故事库", UI_COL_WHITE, UI_COL_BORDER, UI_COL_TEXT, 160, 50, on_endcard_back, NULL);
    lv_obj_add_flag(s_endcard, LV_OBJ_FLAG_HIDDEN);
    return root;
}

void screen_player_on_enter(void)
{
    if (!s_progress_timer) s_progress_timer = lv_timer_create(refresh_progress, 200, NULL);
}

static void open_common(const story_summary_t *s, bool intro)
{
    if (!s || !s->story_id[0]) return;
    app_ui_set_current_story(s);
    s_playing = *s;
    ui_navigator_go(SCREEN_PLAYER);    /* 先创建/进入，再绑定数据 */
    ui_label_fit(s_title_label, s->title[0] ? s->title : "未命名故事", 256);
    ui_heart_btn_set(s_player_fav, s->favorite);
    story_summary_t nb;
    bool many = screen_library_neighbor(s->story_id, 1, &nb);
    lv_obj_t *nav[2] = { s_btn_prev, s_btn_next };
    for (int i = 0; i < 2; i++) {
        if (many) lv_obj_remove_flag(nav[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(nav[i], LV_OBJ_FLAG_HIDDEN);
    }
    const lv_image_dsc_t *full = ui_photo_full(s->thumb_path);
    if (full) {
        lv_image_set_src(s_photo_img, full);
        ui_image_cover(s_photo_img, 480, 320);
    } else if (ui_thumb_bind(s_photo_img, s->thumb_path, 480, 320)) {
        ui_image_cover(s_photo_img, 480, 320);    /* 原图读不到时用缩略图放大兜底 */
    }
    show_endcard(false);
    lv_obj_set_width(s_track_fill, 0);
    s_story_started = false;
    if (intro) {
        s_intro_pending = true;
        ui_sound_prompt(UI_PROMPT_READY);
        ui_icon_set(s_play_icon, UI_IC_PAUSE, 28);
    } else {
        start_story();
    }
    refresh_progress(NULL);
}

void screen_player_open(const story_summary_t *s) { open_common(s, false); }
void screen_player_open_with_intro(const story_summary_t *s) { open_common(s, true); }

void on_player_back(lv_event_t *e)
{
    (void)e;
    ui_navigator_go(SCREEN_LIBRARY);
}
