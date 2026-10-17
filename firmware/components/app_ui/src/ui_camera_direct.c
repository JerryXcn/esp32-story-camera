/**
 * @file ui_camera_direct.c
 * @brief 拍照页旁路直绘（v0.3.1 性能）：相机画面不再经 LVGL 每帧整屏重画。
 *
 * 原路径每个相机帧都让 LVGL 软件重画整屏（约 55 ms）再分块同步发 SPI，预览只有 6~10 帧/秒。
 * 参考出厂固件拍照页的 LVGL 旁路做法，并保留浮在画面上的按钮：
 * - LVGL 只在界面变化时把当前屏幕（预览图隐藏、背景透明）快照成 ARGB8888 浮层，
 *   弹窗遮罩挂在当前屏幕上，自然包含在浮层里；
 * - 每个相机帧在相机任务里由 PPA 硬件把浮层叠到 480x320 预览上（约几毫秒），
 *   显示任务把最新合成帧整帧写屏（SPI 80 MHz 约 31 ms），合成与发送并行；
 * - 拍照保存等相机不出帧的时候，界面变化用最后一帧重新合成。
 * 进入/退出都在 LVGL 线程（持锁）里完成；退出后整屏 invalidate 由 LVGL 接管。
 */

#include "ui_camera_direct.h"
#include "ui_screens.h"
#include "board_adapter.h"

#include "driver/ppa.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "cam_direct";

#define DW 480
#define DH 320
#define RGB565_BYTES (DW * DH * 2)
#define ARGB_BYTES   (DW * DH * 4)
#define BUF_ALIGN    128
#define FRAME_FRESH_US     (300 * 1000)   /* 相机帧在这段时间内到过才进入旁路 */
#define RECOMPOSE_IDLE_US  (60 * 1000)    /* 相机这么久没出帧时，界面变化用最后一帧重新合成 */

static ppa_client_handle_t s_blend;
static uint8_t *s_overlay[2];          /* ARGB8888 浮层双缓冲：LVGL 写后台，合成读前台 */
static int s_ov_front;
static uint8_t *s_out[3];              /* RGB565 合成帧：一块在发送、一块待发送、一块在写 */
static int s_out_ready = -1, s_out_sending = -1, s_out_last = -1;
static SemaphoreHandle_t s_mutex;      /* 合成、浮层交换、缓冲状态 */
static TaskHandle_t s_display_task;
static volatile bool s_active;         /* 旁路已开启（相机帧走合成） */
static const void *s_last_frame;       /* 最近一帧相机预览（相机服务的双缓冲之一） */
static volatile int64_t s_frame_seen_us, s_last_frame_us;
static lv_obj_t *s_root, *s_preview;
static lv_opa_t s_saved_root_opa, s_saved_scr_opa;
static volatile uint32_t s_frames_shown, s_overlay_renders;

static void *alloc_psram(size_t size)
{
    return heap_caps_aligned_calloc(BUF_ALIGN, 1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

/** 持锁调用：浮层叠到 frame 上，结果放进一块空闲合成缓冲并通知显示任务 */
static void compose_locked(const void *frame)
{
    int k = s_out_ready;   /* 还没发出去的旧帧直接被新帧覆盖 */
    if (k < 0) {
        for (k = 0; k < 3 && (k == s_out_sending || k == s_out_last); k++) {}
        if (k >= 3) k = (s_out_sending + 1) % 3;
    }
    ppa_blend_oper_config_t cfg = {
        .in_bg = {
            .buffer = frame, .pic_w = DW, .pic_h = DH, .block_w = DW, .block_h = DH,
            .blend_cm = PPA_BLEND_COLOR_MODE_RGB565,
        },
        .in_fg = {
            .buffer = s_overlay[s_ov_front], .pic_w = DW, .pic_h = DH, .block_w = DW, .block_h = DH,
            .blend_cm = PPA_BLEND_COLOR_MODE_ARGB8888,
        },
        .out = {
            .buffer = s_out[k], .buffer_size = RGB565_BYTES, .pic_w = DW, .pic_h = DH,
            .blend_cm = PPA_BLEND_COLOR_MODE_RGB565,
        },
        .bg_alpha_update_mode = PPA_ALPHA_FIX_VALUE,
        .bg_alpha_fix_val = 255,
        .fg_alpha_update_mode = PPA_ALPHA_NO_CHANGE,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    if (ppa_do_blend(s_blend, &cfg) != ESP_OK) return;
    s_out_ready = k;
    s_out_last = k;
    xTaskNotifyGive(s_display_task);
}

static void display_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            int k = s_out_ready;
            s_out_ready = -1;
            s_out_sending = k;
            xSemaphoreGive(s_mutex);
            if (k < 0) break;
            if (board_adapter_display_blit(s_out[k]) == ESP_OK) s_frames_shown++;
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_out_sending = -1;
            xSemaphoreGive(s_mutex);
        }
    }
}

/** LVGL 线程：当前屏幕快照到后台浮层，再与前台交换 */
static void render_overlay(void)
{
    int back = s_ov_front ^ 1;
    lv_draw_buf_t buf;
    if (lv_draw_buf_init(&buf, DW, DH, LV_COLOR_FORMAT_ARGB8888, DW * 4, s_overlay[back], ARGB_BYTES) != LV_RESULT_OK ||
        lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_ARGB8888, &buf) != LV_RESULT_OK) {
        ESP_LOGW(TAG, "overlay snapshot failed");
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_ov_front = back;
    xSemaphoreGive(s_mutex);
    s_overlay_renders++;
}

bool ui_camera_direct_init(lv_obj_t *camera_root, lv_obj_t *preview)
{
    s_root = camera_root;
    s_preview = preview;
    ppa_client_config_t pcfg = { .oper_type = PPA_OPERATION_BLEND };
    s_mutex = xSemaphoreCreateMutex();
    for (int i = 0; i < 2; i++) s_overlay[i] = alloc_psram(ARGB_BYTES);
    for (int i = 0; i < 3; i++) s_out[i] = alloc_psram(RGB565_BYTES);
    bool ok = s_mutex && s_overlay[0] && s_overlay[1] && s_out[0] && s_out[1] && s_out[2] &&
              ppa_register_client(&pcfg, &s_blend) == ESP_OK &&
              xTaskCreate(display_task, "cam_display", 4096, NULL, 6, &s_display_task) == pdPASS;
    if (!ok) {
        ESP_LOGE(TAG, "init failed; camera preview stays on the LVGL path");
        s_blend = NULL;
    }
    return ok;
}

bool ui_camera_direct_frame(const void *rgb565_480x320)
{
    int64_t now = esp_timer_get_time();
    s_frame_seen_us = now;
    if (!s_active) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool handled = s_active;   /* 退出可能刚刚发生 */
    if (handled) {
        s_last_frame = rgb565_480x320;
        s_last_frame_us = now;
        compose_locked(rgb565_480x320);
    }
    xSemaphoreGive(s_mutex);
    return handled;
}

static void enter(void)
{
    s_saved_root_opa = lv_obj_get_style_bg_opa(s_root, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_t *scr = lv_screen_active();
    s_saved_scr_opa = lv_obj_get_style_bg_opa(scr, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_flag(s_preview, LV_OBJ_FLAG_HIDDEN);
    board_adapter_display_bypass(true);
    render_overlay();
    board_adapter_display_bypass_take_dirty();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_last_frame = NULL;
    s_out_ready = -1;
    s_out_last = -1;
    s_active = true;
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "on");
}

void ui_camera_direct_exit(void)
{
    if (!s_active) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);   /* 等正在进行的合成结束 */
    s_active = false;
    s_last_frame = NULL;
    s_out_ready = -1;
    xSemaphoreGive(s_mutex);
    board_adapter_display_bypass(false);       /* 等正在发送的整帧结束 */
    lv_obj_set_style_bg_opa(s_root, s_saved_root_opa, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lv_screen_active(), s_saved_scr_opa, LV_PART_MAIN);
    lv_obj_remove_flag(s_preview, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(lv_screen_active());
    ESP_LOGI(TAG, "off (shown=%lu overlay=%lu)", (unsigned long)s_frames_shown, (unsigned long)s_overlay_renders);
}

void ui_camera_direct_tick(void)
{
    if (!s_blend) return;
    int64_t now = esp_timer_get_time();
    bool want = ui_navigator_current() == SCREEN_CAMERA && now - s_frame_seen_us < FRAME_FRESH_US;
    if (!s_active) {
        if (want) enter();
        return;
    }
    if (ui_navigator_current() != SCREEN_CAMERA) {
        ui_camera_direct_exit();
        return;
    }
    if (!board_adapter_display_bypass_take_dirty()) return;
    render_overlay();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_active && s_last_frame && now - s_last_frame_us > RECOMPOSE_IDLE_US) compose_locked(s_last_frame);
    xSemaphoreGive(s_mutex);
}

const void *ui_camera_direct_last_frame(void)
{
    return s_active && s_out_last >= 0 ? s_out[s_out_last] : NULL;
}

uint32_t ui_camera_direct_frames_shown(void)
{
    return s_frames_shown;
}
