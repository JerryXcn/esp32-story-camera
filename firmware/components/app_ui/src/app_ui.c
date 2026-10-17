/**
 * @file app_ui.c
 * @brief 导航器与事件分发（实施计划 3：UI 只消费状态快照；
 * 事件按 story_id/operation 核对，迟到事件丢弃）。
 *
 * 事件队列（FLOW-06）：进度事件允许合并（队列满丢最旧进度）；
 * 终态（完成/失败/离开等待/照片已保存）必须可靠送达——满时阻塞短暂重试。
 */

#include "app_ui.h"
#include "ui_screens.h"
#include "ui_story_ops.h"
#include "ui_assets.h"
#include "board_adapter.h"
#include "camera_service.h"
#include "audio_service.h"

#include "lvgl.h"
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "ui";

#define UI_EVENT_QUEUE_LEN 16

typedef struct {
    const screen_t *screen;
    screen_id_t id;
} nav_entry_t;

static screen_t s_screens[] = {
    /* 顺序对应 screen_id_t */
    { screen_boot_create, NULL, NULL, NULL },
    { screen_camera_create, screen_camera_on_enter, screen_camera_on_exit, NULL },
    { screen_generating_create, screen_generating_on_enter, screen_generating_on_exit, NULL },
    { screen_player_create, screen_player_on_enter, screen_player_on_exit, NULL },
    { screen_library_create, screen_library_on_enter, NULL, NULL },
    { screen_detail_create, NULL, NULL, NULL },
    { screen_settings_create, screen_settings_on_enter, NULL, NULL },
    { screen_wifi_create, screen_wifi_on_enter, screen_wifi_on_exit, NULL },
    { screen_password_create, screen_password_on_enter, screen_password_on_exit, NULL },
    { screen_home_create, screen_home_on_enter, NULL, NULL },
    { screen_server_create, screen_server_on_enter, NULL, NULL },
};

static lv_obj_t *s_container;

void ui_theme_init(void);
void app_ui_start_pump(void);
static screen_id_t s_current = SCREEN_BOOT;
static QueueHandle_t s_evq;
static QueueHandle_t s_terminalq;
static uint32_t s_last_sequence;
static bool s_refresh_running, s_refresh_again;
static portMUX_TYPE s_refresh_lock = portMUX_INITIALIZER_UNLOCKED;
static story_summary_t *s_list;
static int s_list_count;
static bool s_camera_ok;
static bool s_sd_ok;
static bool s_scan_pending = true;    /* 启动恢复扫描进行中 */
static story_summary_t s_current_story;   /* 播放/详情页当前条目 */

#define SCREEN_COUNT ((int)(sizeof(s_screens) / sizeof(s_screens[0])))

/* 终态必须可靠送达（FLOW-06） */
static bool is_terminal(operation_state_t st)
{
    return st == OP_COMPLETED || st == OP_FAILED || st == OP_CONTENT_REJECTED ||
           st == OP_WAITING_USER || st == OP_CAPTURE_DONE;
}

/* 屏幕根对象必须是容器的子对象创建。LVGL9 的 lv_obj_set_parent 拒绝 reparent
 * 屏幕（obj->parent==NULL 时直接返回），因此不能先 lv_obj_create(NULL) 再挂载，
 * 否则页面永远进不了渲染树（M0 实测：屏幕只剩根背景）。 */
void ui_screens_set_parent(lv_obj_t *parent)
{
    s_container = parent;
}

lv_obj_t *ui_screens_parent(void)
{
    return s_container;
}

static void show(screen_id_t id)
{
    if (id < 0 || id >= SCREEN_COUNT) return;
    if (s_screens[id].root == NULL) {
        s_screens[id].root = s_screens[id].create();
        if (s_screens[id].root) {
            lv_obj_set_style_pad_all(s_screens[id].root, 0, 0);
            lv_obj_set_style_border_width(s_screens[id].root, 0, 0);
            lv_obj_set_style_radius(s_screens[id].root, 0, 0);
            lv_obj_remove_flag(s_screens[id].root, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_size(s_screens[id].root, 480, 320);
            lv_obj_set_pos(s_screens[id].root, 0, 0);
        }
    }
    if (!s_screens[id].root) return;
    if (s_screens[s_current].on_exit && (int)s_current != (int)id) {
        s_screens[s_current].on_exit();
    }
    /* 切换可见性（不删除、不 reparent：各屏幕状态保留，重进由 on_enter 负责） */
    for (int i = 0; i < SCREEN_COUNT; i++) {
        if (s_screens[i].root) lv_obj_add_flag(s_screens[i].root, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_remove_flag(s_screens[id].root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_screens[id].root);
    s_current = id;
    if (s_screens[id].on_enter) s_screens[id].on_enter();
}

void ui_navigator_init(lv_obj_t *parent)
{
    ui_screens_set_parent(parent);
    s_evq = xQueueCreate(UI_EVENT_QUEUE_LEN, sizeof(story_event_t));
    s_terminalq = xQueueCreate(1, sizeof(story_event_t));
    show(SCREEN_BOOT);
}

void ui_navigator_go(screen_id_t id)
{
    show(id);
}

void ui_navigator_back(void)
{
    /* 单级返回（v0.2）：详情/播放->故事库，密码->Wi-Fi，Wi-Fi->设置，其余->首页 */
    switch (s_current) {
    case SCREEN_DETAIL: case SCREEN_PLAYER: show(SCREEN_LIBRARY); break;
    case SCREEN_PASSWORD: show(SCREEN_WIFI); break;
    case SCREEN_WIFI: show(SCREEN_SETTINGS); break;
    default: show(SCREEN_HOME); break;
    }
}

screen_id_t ui_navigator_current(void)
{
    return s_current;
}

void app_ui_init(void)
{
    ui_ops_init();
    ui_theme_init();
    s_list = heap_caps_calloc(STORAGE_MAX_STORIES, sizeof(*s_list), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    static lv_obj_t *scr;
    scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    s_container = lv_obj_create(scr);
    lv_obj_remove_style_all(s_container);
    lv_obj_set_size(s_container, 480, 320);
    lv_obj_center(s_container);
    ui_navigator_init(s_container);
    app_ui_start_pump();
}

/* story_service 事件 -> LVGL 线程（队列 + lv_timer 抽取） */
void app_ui_on_story_event(const story_event_t *ev)
{
    if (!s_evq) return;
    if (is_terminal(ev->state)) {
        /* 终态可靠送达：短暂阻塞重试，绝不在满队时静默丢弃 */
        if (xQueueSend(s_evq, ev, 0) != pdTRUE && s_terminalq)
            xQueueOverwrite(s_terminalq, ev);
    } else {
        /* Progress is disposable. Never remove a queued terminal event. */
        xQueueSend(s_evq, ev, 0);
    }
}

/* 终态后后台重扫存储（DATA-04：创建/提交/删除后索引可靠刷新）。
 * IO 不在 LVGL 线程执行；结果经 app_ui_set_story_list 回投。 */
static void refresh_task(void *arg)
{
    (void)arg;
    story_summary_t *summaries = heap_caps_malloc(sizeof(*summaries) * STORAGE_MAX_STORIES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (summaries) {
        for (;;) {
            while (!audio_is_quiescent() && audio_get_state() == AUDIO_STOPPED) vTaskDelay(pdMS_TO_TICKS(10));
            int n = storage_recover_all(summaries, STORAGE_MAX_STORIES);
            app_ui_set_story_list(summaries, n);
            portENTER_CRITICAL(&s_refresh_lock);
            bool again = s_refresh_again;
            s_refresh_again = false;
            if (!again) s_refresh_running = false;
            portEXIT_CRITICAL(&s_refresh_lock);
            if (!again) break;
        }
        free(summaries);
    } else {
        portENTER_CRITICAL(&s_refresh_lock);
        s_refresh_running = false;
        portEXIT_CRITICAL(&s_refresh_lock);
        ESP_LOGE(TAG, "library scan allocation failed");
    }
    vTaskDelete(NULL);
}
void app_ui_refresh_library(void)
{
    portENTER_CRITICAL(&s_refresh_lock);
    bool running = s_refresh_running;
    if (running) s_refresh_again = true;
    else s_refresh_running = true;
    portEXIT_CRITICAL(&s_refresh_lock);
    if (!running && xTaskCreate(refresh_task, "lib_refresh", 12288, NULL, 4, NULL) != pdPASS) {
        portENTER_CRITICAL(&s_refresh_lock);
        s_refresh_running = false;
        portEXIT_CRITICAL(&s_refresh_lock);
    }
}

static void handle_event(const story_event_t *ev)
{
    if (ev->sequence && ev->sequence <= s_last_sequence) return;
    s_last_sequence = ev->sequence;
    if (is_terminal(ev->state)) app_ui_refresh_library();
    if (ev->state == OP_NEEDS_CONFIRMATION) { screen_camera_show_consent(); return; }
    if (ev->state == OP_CAPTURE_DONE || ev->state == OP_COMPLETED) {
        screen_camera_on_story_event(ev); /* Clear the saving mask even after navigation. */
        if (ev->has_summary && ev->state == OP_CAPTURE_DONE) s_current_story = ev->summary;
        if (ev->state == OP_CAPTURE_DONE) {
            if (ev->has_summary) screen_detail_open(&ev->summary);
        } else if (ev->has_summary && story_claim_autoplay(ev->autoplay_token)) {
            screen_player_open_with_intro(&ev->summary);
        }
        return;
    }
    if (ui_navigator_current() == SCREEN_CAMERA) screen_camera_on_story_event(ev);
    else if (ui_navigator_current() == SCREEN_GENERATING) screen_generating_on_story_event(ev);
    if ((ev->state == OP_UPLOADING || ev->state == OP_REMOTE_PENDING) &&
        ui_navigator_current() == SCREEN_CAMERA) {
        screen_generating_set_story_id(ev->story_id);
        ui_navigator_go(SCREEN_GENERATING);
    }
}
static void pump_events(lv_timer_t *t)
{
    (void)t;
    story_event_t ev;
    while (s_evq && xQueueReceive(s_evq, &ev, 0) == pdTRUE) handle_event(&ev);
    if (s_terminalq && xQueueReceive(s_terminalq, &ev, 0) == pdTRUE) handle_event(&ev);
}

/* ---- 供 main/app_context 调用的接口 ---- */

/* 刚删除的故事 ID：删除前已开始的后台扫描可能还带着它们，回投列表时过滤掉（ID 随机不复用，过滤恒正确） */
#define FORGOTTEN_MAX 128
static char s_forgotten[FORGOTTEN_MAX][STORAGE_ID_HEX];
static int s_forgotten_next;

static bool is_forgotten(const char *id)
{
    for (int i = 0; i < FORGOTTEN_MAX; i++) {
        if (s_forgotten[i][0] && strcmp(s_forgotten[i], id) == 0) return true;
    }
    return false;
}

void app_ui_forget_stories(const char (*ids)[STORAGE_ID_HEX], int n)
{
    for (int k = 0; k < n; k++) {
        strlcpy(s_forgotten[s_forgotten_next], ids[k], STORAGE_ID_HEX);
        s_forgotten_next = (s_forgotten_next + 1) % FORGOTTEN_MAX;
    }
    int w = 0;
    for (int i = 0; i < s_list_count; i++) {
        if (is_forgotten(s_list[i].story_id)) continue;
        if (w != i) s_list[w] = s_list[i];
        w++;
    }
    s_list_count = w;
}

void app_ui_list_set_favorite(const char *story_id, bool favorite)
{
    for (int i = 0; i < s_list_count; i++) {
        if (strcmp(s_list[i].story_id, story_id) == 0) s_list[i].favorite = favorite;
    }
    if (strcmp(s_current_story.story_id, story_id) == 0) s_current_story.favorite = favorite;
}

int app_ui_favorite_count(void)
{
    int n = 0;
    for (int i = 0; i < s_list_count; i++) n += s_list[i].favorite;
    return n;
}

void app_ui_set_story_list(const story_summary_t *list, int count)
{
    if (!s_list || !list || count < 0) return;
    if (board_adapter_lvgl_lock(1000)) {
        int n = 0;
        for (int i = 0; i < count && n < STORAGE_MAX_STORIES; i++) {
            if (!is_forgotten(list[i].story_id)) s_list[n++] = list[i];
        }
        s_list_count = n;

        if (ui_navigator_current() == SCREEN_LIBRARY) screen_library_reload();
        else if (ui_navigator_current() == SCREEN_HOME) screen_home_on_enter();
        board_adapter_lvgl_unlock();
    }
}

void app_ui_set_camera_available(bool ok)
{
    s_camera_ok = ok;
    if (board_adapter_lvgl_lock(200)) {
        screen_camera_update_availability();
        board_adapter_lvgl_unlock();
    }
}

void app_ui_set_storage_available(bool ok)
{
    s_sd_ok = ok;
    if (board_adapter_lvgl_lock(200)) {
        screen_camera_update_availability();
        board_adapter_lvgl_unlock();
    }
}

void app_ui_set_scan_pending(bool pending)
{
    bool changed = (s_scan_pending != pending);
    s_scan_pending = pending;
    if (changed && board_adapter_lvgl_lock(500)) {
        screen_library_reload();
        if (ui_navigator_current() == SCREEN_HOME) screen_home_on_enter();
        board_adapter_lvgl_unlock();
    }
}

bool app_ui_scan_pending(void)
{
    return s_scan_pending;
}

void app_ui_boot_status(const char *text)
{
    if (board_adapter_lvgl_lock(1000)) {
        screen_boot_set_status(text);
        board_adapter_lvgl_unlock();
    }
}

void app_ui_enter_main(void)
{
    if (board_adapter_lvgl_lock(2000)) {
        show(SCREEN_HOME);
        board_adapter_lvgl_unlock();
    }
}

void app_ui_debug_reset(void)
{
    /* 调试用：回到开机页（调用方须持有 LVGL 锁） */
    show(SCREEN_BOOT);
}

void app_ui_debug_goto(int screen_id)
{
    if (screen_id < 0 || screen_id >= SCREEN_COUNT) return;
    show((screen_id_t)screen_id);
}

void app_ui_debug_open_story(int index)
{
    if (index < 0 || index >= s_list_count) return;
    screen_detail_open(&s_list[index]);
}

const story_summary_t *app_ui_list(void) { return s_list; }
int app_ui_list_count(void) { return s_list_count; }
bool app_ui_camera_ok(void) { return s_camera_ok; }
bool app_ui_sd_ok(void) { return s_sd_ok; }
/** 快门可用 = 相机就绪 且 卡可写（产品文档 5.2 画面2） */
bool app_ui_shutter_ready(void) { return s_camera_ok && s_sd_ok && !s_scan_pending && camera_service_ready() && camera_luma_mean() >= 3 && !story_generation_active(); }

void app_ui_set_current_story(const story_summary_t *s)
{
    s_current_story = *s;
}
const story_summary_t *app_ui_current_story(void) { return &s_current_story; }

static lv_timer_t *s_pump;
void app_ui_start_pump(void)
{
    if (!s_pump) s_pump = lv_timer_create(pump_events, 50, NULL);
}
