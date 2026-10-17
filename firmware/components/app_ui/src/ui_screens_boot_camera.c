/**
 * @file ui_screens_boot_camera.c
 * @brief 开机页（画面1）与全屏拍摄页（画面2/3，v0.2）。
 *
 * 开机：左侧狐狸场景 + 快乐体标题 + 真实初始化状态（不伪造百分比）；完成后进首页。
 * 拍摄（v0.2 全屏 3:2）：
 * - 预览 480x320 铺满（相机服务 PPA 缩放），所见即所得：保存的原图同样裁成 3:2；
 * - 只留三个操作：回首页、故事库、橙色大快门；右下角最近一张照片；
 * - 叠在实时画面上的控件不用阴影/旋转，避免每帧重绘的额外开销；
 * - 快门：闪屏 + 快门声 + "正在保存照片…"，保存/生成期间快门不可按（防连拍）；
 * - 相机/存储异常时画面中央显示原因，而不是黑屏。
 * 家长首次联网说明：未确认时在线生成前弹窗确认，确认后持久化。
 */

#include "ui_screens.h"
#include "ui_camera_direct.h"
#include "ui_assets.h"
#include "ui_widgets.h"
#include "ui_sound.h"
#include "app_ui.h"
#include "story_service.h"
#include "storage_service.h"
#include "camera_service.h"
#include "board_adapter.h"
#include "network_service.h"

#include "lvgl.h"
#include <string.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void on_library_open(lv_event_t *e);
void on_library_open_all(lv_event_t *e);
void on_settings_open(lv_event_t *e);
void on_go_home(lv_event_t *e);
void ui_dialog_show_error(const char *code, bool retryable);

/* ---------------- 开机 ---------------- */

static lv_obj_t *s_boot_status;

static void boot_bar_anim(void *bar, int32_t v)
{
    lv_bar_set_start_value(bar, v, LV_ANIM_OFF);
    lv_bar_set_value(bar, v + 35, LV_ANIM_OFF);
}

lv_obj_t *screen_boot_create(void)
{
    lv_obj_t *root = lv_obj_create(ui_screens_parent());
    lv_obj_set_style_bg_color(root, lv_color_hex(UI_COL_BG), 0);

    const lv_image_dsc_t *scene = ui_asset(UI_ASSET_BOOT_SCENE);
    if (scene) {
        lv_obj_t *frame = lv_obj_create(root);
        lv_obj_remove_style_all(frame);
        lv_obj_set_size(frame, 232, 304);
        lv_obj_set_pos(frame, 0, 8);
        lv_obj_set_style_radius(frame, 22, 0);
        lv_obj_set_style_clip_corner(frame, true, 0);
        lv_obj_t *img = lv_image_create(frame);
        lv_image_set_src(img, scene);
        lv_obj_set_pos(img, 0, 0);
    }
    lv_obj_t *title = lv_label_create(root);
    lv_label_set_text(title, "小故事\n大世界");
    lv_obj_set_style_text_font(title, ui_font_kid(44), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_COL_ORANGE_DK), 0);
    lv_obj_set_style_text_line_space(title, 6, 0);
    lv_obj_set_pos(title, 262, 54);

    lv_obj_t *slogan = lv_label_create(root);
    lv_label_set_text(slogan, "拍一拍 · 听一听\n看见更多想象");
    lv_obj_set_style_text_color(slogan, lv_color_hex(UI_COL_TEXT_DIM), 0);
    lv_obj_set_pos(slogan, 264, 172);

    lv_obj_t *bar = lv_bar_create(root);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 180, 10);
    lv_obj_set_pos(bar, 264, 234);
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_COL_BORDER), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_COL_ORANGE), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_bar_set_mode(bar, LV_BAR_MODE_RANGE);
    lv_bar_set_range(bar, 0, 100);
    /* 只表示"正在启动"：一段色块来回滑动，不显示假百分比 */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, bar);
    lv_anim_set_values(&a, 0, 65);
    lv_anim_set_duration(&a, 900);
    lv_anim_set_playback_duration(&a, 900);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&a, boot_bar_anim);
    lv_anim_start(&a);

    s_boot_status = lv_label_create(root);
    lv_label_set_text(s_boot_status, "正在启动…");
    lv_obj_set_style_text_color(s_boot_status, lv_color_hex(UI_COL_TEXT_DIM), 0);
    lv_obj_set_pos(s_boot_status, 264, 254);
    return root;
}

void screen_boot_set_status(const char *text)
{
    if (s_boot_status) lv_label_set_text(s_boot_status, text);
}

/* ---------------- 拍摄 ---------------- */

static lv_obj_t *s_preview;          /* lv_image：源为相机服务双缓冲 */
static lv_image_dsc_t s_preview_dsc; /* 描述符：仅换 data 指针 */
static lv_obj_t *s_shutter, *s_shutter_icon;
static lv_obj_t *s_saving_chip, *s_flash, *s_bubble, *s_last_thumb, *s_last_img;
static lv_obj_t *s_status_pill_mid, *s_status_label;  /* 相机/存储故障态提示 */
static bool s_capturing_ui;          /* 已显示"正在保存"（等待保存结果） */
static volatile bool s_preview_on, s_preview_starting, s_preview_wanted;
static TaskHandle_t s_camera_controller;
static uint8_t *s_preview_pixels;
static lv_obj_t *s_consent_mask;

static void preview_frame_cb(const camera_frame_t *frame, void *user)
{
    (void)user;
    /* 旁路直绘开启时：PPA 合成浮层后整帧上屏，不经 LVGL（见 ui_camera_direct.c） */
    if (frame->width == UI_PREVIEW_W && frame->height == UI_PREVIEW_H && ui_camera_direct_frame(frame->data)) return;
    /* 进入旁路前的头几帧：短暂持 LVGL 锁换源并触发重绘（CAM-01 建议路径） */
    if (!board_adapter_lvgl_lock(100)) return;
    if (s_preview_wanted && s_preview && s_preview_pixels && frame->width == UI_PREVIEW_W && frame->height == UI_PREVIEW_H) {
        memcpy(s_preview_pixels, frame->data, UI_PREVIEW_W * UI_PREVIEW_H * 2);
        s_preview_dsc.data = s_preview_pixels;
        lv_image_set_src(s_preview, &s_preview_dsc);
        lv_obj_invalidate(s_preview);
    }
    board_adapter_lvgl_unlock();
}

static void set_shutter_enabled(bool on)
{
    if (!s_shutter) return;
    if (on) lv_obj_remove_state(s_shutter, LV_STATE_DISABLED);
    else lv_obj_add_state(s_shutter, LV_STATE_DISABLED);
}

static void flash_anim_cb(void *obj, int32_t v)
{
    lv_obj_set_style_bg_opa(obj, (lv_opa_t)v, 0);
}

static void flash_done_cb(lv_anim_t *a)
{
    lv_obj_add_flag(a->var, LV_OBJ_FLAG_HIDDEN);
}

static void on_shutter(lv_event_t *e)
{
    (void)e;
    if (!app_ui_shutter_ready() || s_capturing_ui) return;
    /* 立即反馈：闪屏 + 快门声 + 正在保存，禁用快门防连拍（产品文档 5.2 画面3） */
    s_capturing_ui = true;
    set_shutter_enabled(false);
    ui_sound_shutter();
    lv_obj_remove_flag(s_flash, LV_OBJ_FLAG_HIDDEN);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_flash);
    lv_anim_set_values(&a, LV_OPA_90, LV_OPA_TRANSP);
    lv_anim_set_duration(&a, 350);
    lv_anim_set_exec_cb(&a, flash_anim_cb);
    lv_anim_set_completed_cb(&a, flash_done_cb);
    lv_anim_start(&a);
    lv_obj_remove_flag(s_saving_chip, LV_OBJ_FLAG_HIDDEN);
    esp_err_t err = story_capture_async();
    if (err != ESP_OK) {
        s_capturing_ui = false;
        set_shutter_enabled(true);
        lv_obj_add_flag(s_saving_chip, LV_OBJ_FLAG_HIDDEN);
        ui_dialog_show_error(err == ESP_ERR_NOT_SUPPORTED ? "STORY_LIMIT" :
                             err == ESP_ERR_NO_MEM ? "STORAGE_FULL" : "CAMERA_FAILED", false);
    }
}

static void show_problem(const char *text)
{
    if (!s_status_pill_mid) return;
    if (text) {
        lv_label_set_text(s_status_label, text);
        lv_obj_remove_flag(s_status_pill_mid, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_status_pill_mid, LV_OBJ_FLAG_HIDDEN);
    }
}

void screen_camera_update_availability(void)
{
    if (!s_shutter) return;
    if (!s_capturing_ui) set_shutter_enabled(app_ui_shutter_ready());   /* 保存中保持禁用 */
    if (!s_preview_on && !s_preview_starting && app_ui_camera_ok() &&
        ui_navigator_current() == SCREEN_CAMERA) screen_camera_on_enter();
    if (!app_ui_sd_ok()) show_problem("没有存储卡，不能拍照");
    else if (!app_ui_camera_ok()) show_problem("相机还没准备好");
    else if (app_ui_scan_pending()) show_problem("正在查找故事…");
    else if (!camera_service_ready()) show_problem("正在打开相机…");
    else if (camera_luma_mean() < 3) show_problem("画面太暗，看看镜头和灯光");
    else if (story_generation_active() && !s_capturing_ui) show_problem("上一个故事还在生成");
    else show_problem(NULL);
}

static void camera_controller(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t notification;
        xTaskNotifyWait(0, UINT32_MAX, &notification, pdMS_TO_TICKS(100));
        if (s_preview_wanted && app_ui_camera_ok() && !s_preview_on) {
            s_preview_starting = true;
            esp_err_t err = camera_start_preview(preview_frame_cb, NULL);
            s_preview_on = err == ESP_OK;
            s_preview_starting = false;
            if (err != ESP_OK) app_ui_set_camera_available(false);
        }
        if (!s_preview_wanted && s_preview_on) {
            camera_stop_preview();
            s_preview_on = false;
        }
    }
}

static void camera_health_timer(lv_timer_t *timer)
{
    (void)timer;
    if (ui_navigator_current() == SCREEN_CAMERA) screen_camera_update_availability();
}

static void bubble_fade_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa(obj, (lv_opa_t)v, 0);
}

static void bind_last_thumb(void)
{
    if (!s_last_img) return;
    if (app_ui_list_count() > 0 && ui_thumb_bind(s_last_img, app_ui_list()[0].thumb_path, 48, 48)) {
        ui_image_cover(s_last_img, 48, 48);
        lv_obj_remove_flag(s_last_thumb, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_last_thumb, LV_OBJ_FLAG_HIDDEN);
    }
}

void screen_camera_on_enter(void)
{
    s_preview_wanted = true;
    if (s_camera_controller) xTaskNotify(s_camera_controller, 1, eSetBits);
    if (s_bubble) {
        /* 提示气泡显示 5 秒后淡出 */
        lv_anim_delete(s_bubble, bubble_fade_cb);
        lv_obj_set_style_opa(s_bubble, LV_OPA_COVER, 0);
        lv_obj_remove_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_bubble);
        lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
        lv_anim_set_delay(&a, 5000);
        lv_anim_set_duration(&a, 600);
        lv_anim_set_exec_cb(&a, bubble_fade_cb);
        lv_anim_start(&a);
    }
    bind_last_thumb();
}

static void direct_tick_cb(lv_timer_t *t)
{
    (void)t;
    ui_camera_direct_tick();
}

void screen_camera_on_exit(void)
{
    ui_camera_direct_exit();
    s_preview_wanted = false;
    if (s_camera_controller) xTaskNotify(s_camera_controller, 1, eSetBits);
}

void screen_camera_on_story_event(const story_event_t *ev)
{
    if (!s_shutter) return;
    if (ev->state == OP_CAPTURE_SAVING) {
        lv_obj_remove_flag(s_saving_chip, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (ev->state == OP_CAPTURE_DONE || ev->state == OP_COMPLETED || ev->state == OP_FAILED ||
        ev->state == OP_UPLOADING || ev->state == OP_CONTENT_REJECTED ||
        ev->state == OP_WAITING_USER) {
        /* 保存有结果（或已进入生成流程）：恢复快门、收起"正在保存" */
        s_capturing_ui = false;
        lv_obj_add_flag(s_saving_chip, LV_OBJ_FLAG_HIDDEN);
        screen_camera_update_availability();
    }
    if (ui_navigator_current() != SCREEN_CAMERA) return;
    if (ev->state == OP_FAILED && strcmp(ev->error_code, "CAMERA_FAILED") == 0) {
        ui_dialog_show_error("CAMERA_FAILED", false);
    } else if (ev->state == OP_FAILED && ev->operation_id[0] == 0) {
        /* 拍照/保存阶段的失败（生成阶段失败由生成中页面就地提示） */
        ui_dialog_show_error(ev->error_code[0] ? ev->error_code : "SD_IO_ERROR", ev->error_retryable);
    }
}

/* ---------------- 家长首次联网说明（产品文档 6.1） ---------------- */

static void on_consent_ok(lv_event_t *e)
{
    lv_obj_t *mask = (lv_obj_t *)lv_event_get_user_data(e);
    s_consent_mask = NULL;
    lv_obj_delete_async(mask);
    const char *story = app_ui_current_story()->story_id;
    if (story_confirm_consent() == ESP_OK && story_start_generation(story, false) == ESP_OK) {
        screen_generating_set_story_id(story);
        ui_navigator_go(SCREEN_GENERATING);
    } else {
        ui_dialog_show_error(network_state() < NET_STATE_GOT_IP ? "OFFLINE" : "NETWORK_TIMEOUT", false);
    }
}

static void on_consent_cancel(lv_event_t *e)
{
    lv_obj_t *mask = (lv_obj_t *)lv_event_get_user_data(e);
    s_consent_mask = NULL;
    lv_obj_delete_async(mask);   /* 拒绝不影响离线照片（记录仍为仅照片） */
}

void screen_camera_show_consent(void)
{
    if (s_consent_mask) return;
    ui_dialog_spec_t spec = {
        .title = "开始前请家长确认",
        .body = "点\"生成故事\"会把这页绘本的照片上传，由 AI 编故事、配声音。照片保存在存储卡里。确认后不再询问。",
        .left = "暂不联网", .left_cb = on_consent_cancel,
        .right = "同意", .right_cb = on_consent_ok,
        .right_face = UI_COL_SKY, .right_edge = UI_COL_SKY_DK,
    };
    s_consent_mask = ui_dialog(&spec);
}

lv_obj_t *screen_camera_create(void)
{
    lv_obj_t *root = lv_obj_create(ui_screens_parent());
    lv_obj_set_style_bg_color(root, lv_color_hex(0x1A1410), 0);

    s_preview_pixels = heap_caps_calloc(UI_PREVIEW_W * UI_PREVIEW_H, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    memset(&s_preview_dsc, 0, sizeof(s_preview_dsc));
    s_preview_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_preview_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_preview_dsc.header.w = UI_PREVIEW_W;
    s_preview_dsc.header.h = UI_PREVIEW_H;
    s_preview_dsc.header.stride = UI_PREVIEW_W * 2;
    s_preview_dsc.data_size = UI_PREVIEW_W * UI_PREVIEW_H * 2;
    if (s_preview_pixels) xTaskCreate(camera_controller, "cam_control", 4096, NULL, 4, &s_camera_controller);
    lv_timer_create(camera_health_timer, 200, NULL);

    s_preview = lv_image_create(root);
    lv_obj_set_pos(s_preview, 0, 0);
    lv_obj_set_size(s_preview, UI_PREVIEW_W, UI_PREVIEW_H);
    if (ui_camera_direct_init(root, s_preview)) lv_timer_create(direct_tick_cb, 15, NULL);

    /* 故障/等待提示（画面中央白色胶囊） */
    s_status_pill_mid = lv_obj_create(root);
    lv_obj_remove_style_all(s_status_pill_mid);
    lv_obj_set_size(s_status_pill_mid, LV_SIZE_CONTENT, 40);
    lv_obj_set_style_pad_hor(s_status_pill_mid, 18, 0);
    lv_obj_set_style_radius(s_status_pill_mid, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_status_pill_mid, lv_color_hex(UI_COL_WHITE), 0);
    lv_obj_set_style_bg_opa(s_status_pill_mid, 235, 0);
    lv_obj_align(s_status_pill_mid, LV_ALIGN_CENTER, -40, 0);
    s_status_label = lv_label_create(s_status_pill_mid);
    lv_obj_center(s_status_label);
    lv_obj_add_flag(s_status_pill_mid, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *home = ui_round_btn(root, UI_IC_HOME, 44, UI_ROUND_GLASS, on_go_home, NULL);
    lv_obj_set_pos(home, 12, 12);

    s_bubble = lv_obj_create(root);
    lv_obj_remove_style_all(s_bubble);
    lv_obj_set_size(s_bubble, LV_SIZE_CONTENT, 36);
    lv_obj_set_style_pad_hor(s_bubble, 14, 0);
    lv_obj_set_style_radius(s_bubble, 18, 0);
    lv_obj_set_style_bg_color(s_bubble, lv_color_hex(UI_COL_WHITE), 0);
    lv_obj_set_style_bg_opa(s_bubble, LV_OPA_COVER, 0);
    lv_obj_set_pos(s_bubble, 66, 16);
    lv_obj_t *bl = lv_label_create(s_bubble);
    lv_label_set_text(bl, "对准绘本，拍下这一页");
    lv_obj_center(bl);

    lv_obj_t *pill = ui_status_pill(root, true);
    lv_obj_align(pill, LV_ALIGN_TOP_RIGHT, -12, 16);

    /* 直径取偶数：LVGL 9.2 软件渲染下奇数半径（如 54px）的半透明圆底不绘制（实机复现） */
    lv_obj_t *lib = ui_round_btn(root, UI_IC_BOOKS, 52, UI_ROUND_GLASS, on_library_open_all, NULL);
    lv_obj_set_pos(lib, 480 - 30 - 52, 56);
    lv_obj_t *lib_lbl = lv_obj_create(root);
    lv_obj_remove_style_all(lib_lbl);
    lv_obj_set_size(lib_lbl, LV_SIZE_CONTENT, 22);
    lv_obj_set_style_pad_hor(lib_lbl, 8, 0);
    lv_obj_set_style_radius(lib_lbl, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(lib_lbl, lv_color_hex(UI_COL_SMOKE), 0);
    lv_obj_set_style_bg_opa(lib_lbl, 150, 0);
    lv_obj_t *ll = lv_label_create(lib_lbl);
    lv_label_set_text(ll, "故事库");
    lv_obj_set_style_text_color(ll, lv_color_hex(UI_COL_WHITE), 0);
    lv_obj_center(ll);
    lv_obj_align_to(lib_lbl, lib, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);

    s_shutter = lv_obj_create(root);
    lv_obj_remove_style_all(s_shutter);
    lv_obj_set_size(s_shutter, 88, 88);
    lv_obj_set_pos(s_shutter, 480 - 12 - 88, 140);
    lv_obj_add_flag(s_shutter, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_shutter, 8);
    lv_obj_set_style_radius(s_shutter, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_shutter, lv_color_hex(UI_COL_ORANGE), 0);
    lv_obj_set_style_bg_color(s_shutter, lv_color_hex(UI_COL_ORANGE_DK), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(s_shutter, lv_color_hex(UI_COL_MUTE), LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(s_shutter, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_shutter, lv_color_hex(UI_COL_WHITE), 0);
    lv_obj_set_style_border_width(s_shutter, 6, 0);
    s_shutter_icon = ui_icon_create(s_shutter, UI_IC_CAMERA, 40, UI_COL_WHITE);
    lv_obj_center(s_shutter_icon);
    lv_obj_add_event_cb(s_shutter, on_shutter, LV_EVENT_CLICKED, NULL);

    s_last_thumb = lv_obj_create(root);
    lv_obj_remove_style_all(s_last_thumb);
    lv_obj_set_size(s_last_thumb, 54, 54);
    lv_obj_set_pos(s_last_thumb, 480 - 29 - 54, 320 - 12 - 54);
    lv_obj_add_flag(s_last_thumb, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(s_last_thumb, 14, 0);
    lv_obj_set_style_clip_corner(s_last_thumb, true, 0);
    lv_obj_set_style_bg_color(s_last_thumb, lv_color_hex(UI_COL_WHITE), 0);
    lv_obj_set_style_bg_opa(s_last_thumb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_last_thumb, lv_color_hex(UI_COL_WHITE), 0);
    lv_obj_set_style_border_width(s_last_thumb, 3, 0);
    s_last_img = lv_image_create(s_last_thumb);
    lv_obj_remove_flag(s_last_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_last_thumb, on_library_open_all, LV_EVENT_CLICKED, NULL);
    ui_add_click_sound(s_last_thumb);
    lv_obj_add_flag(s_last_thumb, LV_OBJ_FLAG_HIDDEN);

    s_flash = lv_obj_create(root);
    lv_obj_remove_style_all(s_flash);
    lv_obj_set_size(s_flash, 480, 320);
    lv_obj_set_style_bg_color(s_flash, lv_color_hex(UI_COL_WHITE), 0);
    lv_obj_add_flag(s_flash, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_flash, LV_OBJ_FLAG_CLICKABLE);

    s_saving_chip = lv_obj_create(root);
    lv_obj_remove_style_all(s_saving_chip);
    lv_obj_set_size(s_saving_chip, LV_SIZE_CONTENT, 48);
    lv_obj_set_style_pad_hor(s_saving_chip, 20, 0);
    lv_obj_set_style_radius(s_saving_chip, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_saving_chip, lv_color_hex(UI_COL_WHITE), 0);
    lv_obj_set_style_bg_opa(s_saving_chip, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(s_saving_chip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_saving_chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_saving_chip, 10, 0);
    lv_obj_t *sp = lv_spinner_create(s_saving_chip);
    lv_obj_set_size(sp, 22, 22);
    lv_obj_set_style_arc_width(sp, 4, 0);
    lv_obj_set_style_arc_width(sp, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(sp, lv_color_hex(UI_COL_BORDER), 0);
    lv_obj_set_style_arc_color(sp, lv_color_hex(UI_COL_ORANGE), LV_PART_INDICATOR);
    lv_obj_t *sl = lv_label_create(s_saving_chip);
    lv_label_set_text(sl, "正在保存照片…");
    lv_obj_set_style_text_font(sl, ui_font_cn(22), 0);
    lv_obj_center(s_saving_chip);
    lv_obj_add_flag(s_saving_chip, LV_OBJ_FLAG_HIDDEN);
    return root;
}

/* 共用跳转回调：返回故事库（保留当前视图：全部 / 收藏） */
void on_library_open(lv_event_t *e)
{
    (void)e;
    screen_library_reload();
    ui_navigator_go(SCREEN_LIBRARY);
}

/* 打开全部故事（首页"故事库"、拍照页） */
void on_library_open_all(lv_event_t *e)
{
    (void)e;
    screen_library_open(false);
}

void on_settings_open(lv_event_t *e)
{
    (void)e;
    ui_navigator_go(SCREEN_SETTINGS);
}

void on_go_home(lv_event_t *e)
{
    (void)e;
    ui_navigator_go(SCREEN_HOME);
}
