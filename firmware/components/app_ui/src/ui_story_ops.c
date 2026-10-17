/**
 * @file ui_story_ops.c
 * @brief 故事库的后台操作：删除（单条/批量，带加载与完成提示）与收藏。
 *
 * TF 卡写入（删除意图双槽 fsync 每条约数百毫秒、收藏标记 fsync）放在独立任务里，
 * LVGL 线程只显示进度：删除时加载遮罩"正在删除 2/5"，完成后把条目从列表里直接移除
 * 并提示"已删除 N 条故事"，再后台重扫兜底；收藏先改界面再写盘。
 */

#include "ui_story_ops.h"
#include "ui_screens.h"
#include "ui_widgets.h"
#include "app_ui.h"
#include "board_adapter.h"
#include "story_service.h"
#include "storage_service.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "story_ops";

typedef enum { OP_DELETE, OP_FAVORITE, OP_FAVORITE_MANY } op_kind_t;

typedef struct {
    op_kind_t kind;
    int count;
    char (*ids)[STORAGE_ID_HEX];      /* OP_DELETE / OP_FAVORITE_MANY：PSRAM 数组，由工作任务释放 */
    char story_id[STORAGE_ID_HEX];    /* OP_FAVORITE */
    bool favorite;
    ui_ops_done_cb done;
} op_t;

static QueueHandle_t s_queue;
static lv_obj_t *s_loading;
static volatile bool s_busy;

void ui_dialog_show_error(const char *code, bool retryable);

static void delete_progress(int done, int total, void *user)
{
    (void)user;
    if (!board_adapter_lvgl_lock(500)) return;
    if (s_loading && total > 1) {
        char t[48];
        snprintf(t, sizeof(t), "正在删除 %d/%d", done < total ? done + 1 : total, total);
        ui_loading_set_text(s_loading, t);
    }
    board_adapter_lvgl_unlock();
}

static void run_delete(op_t *op)
{
    int deleted = 0;
    esp_err_t err = story_delete_many((const char (*)[STORAGE_ID_HEX])op->ids, op->count,
                                      delete_progress, NULL, &deleted);
    ESP_LOGI(TAG, "delete %d/%d -> %s", deleted, op->count, esp_err_to_name(err));
    board_adapter_lvgl_lock(portMAX_DELAY);
    if (s_loading) { lv_obj_delete(s_loading); s_loading = NULL; }
    if (deleted > 0) {
        /* 删除意图已落盘：直接从列表移除，界面立刻更新；后台重扫只做兜底 */
        app_ui_forget_stories((const char (*)[STORAGE_ID_HEX])op->ids, op->count);
        if (op->done) op->done(deleted);
        if (ui_navigator_current() == SCREEN_LIBRARY) screen_library_reload();
        char t[48];
        if (deleted == 1) snprintf(t, sizeof(t), "已删除");
        else snprintf(t, sizeof(t), "已删除 %d 条故事", deleted);
        ui_toast(t);
    }
    if (err == ESP_ERR_INVALID_STATE) ui_dialog_show_error("DEVICE_BUSY", false);
    else if (err != ESP_OK) ui_dialog_show_error("SD_IO_ERROR", false);
    board_adapter_lvgl_unlock();
    if (deleted > 0) app_ui_refresh_library();
}

static void ops_task(void *arg)
{
    (void)arg;
    op_t op;
    for (;;) {
        if (xQueueReceive(s_queue, &op, portMAX_DELAY) != pdTRUE) continue;
        if (op.kind == OP_DELETE) {
            run_delete(&op);
            heap_caps_free(op.ids);
            s_busy = false;
        } else if (op.kind == OP_FAVORITE_MANY) {
            int failed = 0;
            for (int i = 0; i < op.count; i++) {
                if (storage_set_favorite(op.ids[i], op.favorite) != ESP_OK) {
                    failed++;
                    if (board_adapter_lvgl_lock(1000)) {
                        app_ui_list_set_favorite(op.ids[i], !op.favorite);   /* 写失败：界面回滚 */
                        board_adapter_lvgl_unlock();
                    }
                }
            }
            if (failed && board_adapter_lvgl_lock(1000)) {
                if (ui_navigator_current() == SCREEN_LIBRARY) screen_library_reload();
                ui_dialog_show_error("SD_IO_ERROR", false);
                board_adapter_lvgl_unlock();
            }
            heap_caps_free(op.ids);
        } else {
            esp_err_t err = storage_set_favorite(op.story_id, op.favorite);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "favorite %s -> %s", op.story_id, esp_err_to_name(err));
                /* 写失败：界面回滚 */
                if (board_adapter_lvgl_lock(1000)) {
                    app_ui_list_set_favorite(op.story_id, !op.favorite);
                    if (ui_navigator_current() == SCREEN_LIBRARY) screen_library_reload();
                    ui_dialog_show_error("SD_IO_ERROR", false);
                    board_adapter_lvgl_unlock();
                }
            }
        }
    }
}

void ui_ops_init(void)
{
    if (s_queue) return;
    s_queue = xQueueCreate(8, sizeof(op_t));
    if (!s_queue || xTaskCreate(ops_task, "story_ops", 6144, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "init failed");
    }
}

bool ui_ops_delete(const char (*ids)[STORAGE_ID_HEX], int count, ui_ops_done_cb done)
{
    if (!s_queue || s_busy || count <= 0) return false;
    if (story_generation_active()) {
        ui_dialog_show_error("DEVICE_BUSY", false);
        return false;
    }
    op_t op = { .kind = OP_DELETE, .count = count, .done = done };
    op.ids = heap_caps_malloc(sizeof(op.ids[0]) * count, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!op.ids) return false;
    memcpy(op.ids, ids, sizeof(op.ids[0]) * count);
    s_busy = true;
    s_loading = ui_loading_show("正在删除…");
    if (xQueueSend(s_queue, &op, 0) != pdTRUE) {
        heap_caps_free(op.ids);
        lv_obj_delete(s_loading);
        s_loading = NULL;
        s_busy = false;
        return false;
    }
    return true;
}

void ui_ops_set_favorite(const char *story_id, bool favorite)
{
    if (!s_queue || !story_id || !story_id[0]) return;
    app_ui_list_set_favorite(story_id, favorite);
    op_t op = { .kind = OP_FAVORITE, .favorite = favorite };
    strlcpy(op.story_id, story_id, sizeof(op.story_id));
    xQueueSend(s_queue, &op, 0);
}

bool ui_ops_set_favorite_many(const char (*ids)[STORAGE_ID_HEX], int count, bool favorite)
{
    if (!s_queue || count <= 0) return false;
    op_t op = { .kind = OP_FAVORITE_MANY, .count = count, .favorite = favorite };
    op.ids = heap_caps_malloc(sizeof(op.ids[0]) * count, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!op.ids) return false;
    memcpy(op.ids, ids, sizeof(op.ids[0]) * count);
    for (int i = 0; i < count; i++) app_ui_list_set_favorite(ids[i], favorite);
    if (xQueueSend(s_queue, &op, 0) != pdTRUE) {
        for (int i = 0; i < count; i++) app_ui_list_set_favorite(ids[i], !favorite);
        heap_caps_free(op.ids);
        return false;
    }
    return true;
}
