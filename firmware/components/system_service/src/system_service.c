/**
 * @file system_service.c
 * @brief 时钟与资源统计实现。时间同步由 network_service 在 SNTP 完成后调用
 *        system_time_mark_synced；本组件只负责质量标记，不伪造日期。
 */

#include "system_service.h"

#include <time.h>
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static bool s_time_synced = false;

bool system_time_is_synced(void)
{
    return s_time_synced;
}

void system_time_mark_synced(void)
{
    s_time_synced = true;
}

int64_t system_time_now_epoch(void)
{
    if (!s_time_synced) return 0;
    time_t now = time(NULL);
    return (int64_t)now;
}

bool system_last_reset_was_abnormal(void)
{
    esp_reset_reason_t r = esp_reset_reason();
    return r == ESP_RST_PANIC || r == ESP_RST_BROWNOUT || r == ESP_RST_TASK_WDT ||
           r == ESP_RST_WDT || r == ESP_RST_INT_WDT;
}

void system_heap_snapshot(uint32_t *internal_min, uint32_t *psram_min)
{
    if (internal_min) *internal_min = esp_get_minimum_free_heap_size();
    if (psram_min) *psram_min = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
}
