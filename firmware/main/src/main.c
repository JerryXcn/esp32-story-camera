/**
 * @file main.c
 * @brief 入口：组装各服务并交给 LVGL 主循环。
 */

#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "app_context.h"
#include "app_ui.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "story camera boot");
    /* 首版简体中文市场：本地时间按北京时间显示（存储仍用 UTC epoch） */
    setenv("TZ", "CST-8", 1);
    tzset();
    app_context_boot();
    /* LVGL 定时器由 board_adapter/app_ui 内部任务驱动；主任务让出 */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
