/**
 * @file app_context.c
 * @brief 启动组装（实施计划 9.1/9.2）。任何外设失败都不阻塞 UI：
 *        卡损坏展示错误而不是空库；启动后没有用户动作不得播放历史音频。
 */

#include "app_context.h"

#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "settings_service.h"
#include "system_service.h"
#include "board_adapter.h"
#include "storage_service.h"
#include "audio_service.h"
#include "network_service.h"
#include "story_service.h"
#include "app_ui.h"
#include "debug_console.h"

static const char *TAG = "boot";
static app_stage_cb s_stage_cb;
static void *s_stage_user;
static bool s_sd_ok;

static void emit(app_stage_t st, bool ok)
{
    if (s_stage_cb) s_stage_cb(st, ok, s_stage_user);
}

static void on_story_event(const story_event_t *ev, void *user)
{
    /* 转发给 UI 线程处理（LVGL 锁内更新控件），迟到事件按 generation 丢弃 */
    app_ui_on_story_event(ev);
}

static void on_net_state(net_state_t state, void *user)
{
    (void)user;
    if (state == NET_STATE_GOT_IP) {
        app_ui_boot_status("网络已连接");
        emit(APP_STAGE_NETWORK, true);
    }
}

/* 相机与联网放后台：先保证可操作 UI（产品文档 5.2 画面1：联网和相机可稍后完成） */
static void deferred_init_task(void *arg)
{
    struct board_camera_config cam_cfg = { .preview_w = 320, .preview_h = 240 };
    bool cam_ok = board_adapter_init_camera(&cam_cfg) == ESP_OK;
    app_ui_boot_status(cam_ok ? "相机已就绪" : "相机未就绪");
    emit(APP_STAGE_CAMERA, cam_ok);
    app_ui_set_camera_available(cam_ok);   /* 快门可用性 = 相机 && 卡（页面内刷新） */

    if (network_init(on_net_state, NULL) == ESP_OK) {
        network_connect_saved();       /* 后台连接与校时；无网络不挡 UI */
    } else {
        app_ui_boot_status("网络不可用");
    }
    vTaskDelete(NULL);
}

static void on_audio_state(audio_state_t state, void *user)
{
    (void)user;
    if (state == AUDIO_ERROR) board_adapter_report_fault("audio", -1);
}

static void recover_task(void *arg)
{
    /* 后台恢复：分批检查索引，不阻塞首屏（实施计划 6.5/9.1）；
     * 扫描期间库页显示"正在查找故事…"，完成后才判定空库（DATA-04）。 */
    story_summary_t *summaries = heap_caps_malloc(STORAGE_MAX_STORIES * sizeof(*summaries), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!summaries) { ESP_LOGE(TAG, "recovery allocation failed"); vTaskDelete(NULL); return; }
    if (audio_is_quiescent()) storage_continue_delete();
    int n = storage_recover_all(summaries, STORAGE_MAX_STORIES);
    /* NVS can come from factory firmware while this TF card has older stories.
     * Reconcile before enabling captures so new photos sort ahead of them. */
    esp_err_t seq_err = settings_ensure_capture_seq(n > 0 ? summaries[0].capture_seq : 0);
    if (seq_err != ESP_OK) {
        ESP_LOGE(TAG, "capture counter reconciliation failed: %s", esp_err_to_name(seq_err));
        app_ui_set_storage_available(false);
    }
    ESP_LOGI(TAG, "recovered %d stories", n);
    app_ui_set_story_list(summaries, n);
    heap_caps_free(summaries);
    app_ui_set_scan_pending(false);
    emit(APP_STAGE_RECOVER, seq_err == ESP_OK);
    vTaskDelete(NULL);
}

void app_context_boot(void)
{
    bool board_ok = settings_init() == ESP_OK;
    emit(APP_STAGE_BOARD, board_ok);

    /* 最短路径点亮可操作 UI（产品文档 5.2：先显示可操作 UI，不伪造进度） */
    board_adapter_init_display();      /* 面板 + 触摸 + 背光（不含渲染任务） */
    app_ui_boot_status("屏幕已就绪");
    app_ui_init();                     /* 构建初始屏幕（LVGL 非线程安全，先建后渲染） */
    board_adapter_lvgl_start();        /* UI 构建完成，启动渲染任务 */
    emit(APP_STAGE_UI, true);

    /* TF：挂载成功才建 storybox 契约目录；失败如实上报（禁用快门） */
    s_sd_ok = board_adapter_init_sd() == ESP_OK;
    if (s_sd_ok) {
        s_sd_ok = storage_init(board_adapter_sd_mount_point()) == ESP_OK;
    }
    app_ui_boot_status(s_sd_ok ? "存储卡已就绪" : "未检测到存储卡");
    emit(APP_STAGE_STORAGE, s_sd_ok);
    app_ui_set_storage_available(s_sd_ok);   /* 卡不可写禁用快门（相机就绪后再叠加） */

    /* 音频：ES8311 + 功放 + 上电提示音（链路自证） */
    audio_state_cb cb = on_audio_state;
    audio_init(cb, NULL);
    bool audio_ok = board_adapter_init_audio() == ESP_OK;
    if (audio_ok) {
        app_ui_boot_status("声音已就绪");
        /* 开机音量读取用户设置（不再固定 40） */
        product_settings_t set;
        settings_load_product(&set);
        audio_set_volume(set.volume);
    }
    emit(APP_STAGE_AUDIO, audio_ok);

    story_service_init(on_story_event, NULL);

    /* 相机与联网交给后台任务；UI 已可操作，不再阻塞 */
    xTaskCreate(deferred_init_task, "deferred_init", 8192, NULL, 5, NULL);
    xTaskCreate(recover_task, "recover", 12288, NULL, 4, NULL);   /* -O2 下 8 KB 实测溢出 */

    /* M0 实机调试控制台（screenshot / tap / touch / info / camreg ...） */
    debug_console_start();

    /* 进入可操作主界面（拍摄页）；相机就绪前快门不可用 */
    app_ui_enter_main();
}
