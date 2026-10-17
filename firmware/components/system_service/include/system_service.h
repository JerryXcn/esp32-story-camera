#pragma once
/**
 * @file system_service.h
 * @brief 系统杂项（实施计划 3/9.1）：时钟质量、资源统计、异常复位记录。
 * 未校时的时间不得当作真实日期（time_source=unknown，UI 显示“时间未同步”）。
 */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 时间源：SNTP 同步成功后才视为 synced */
bool system_time_is_synced(void);
int64_t system_time_now_epoch(void);       /* 未同步时返回 0 */
void system_time_mark_synced(void);

/** 上次复位是否为掉电/panic（用于提示恢复，不自动发起任何云调用） */
bool system_last_reset_was_abnormal(void);

/** 资源采样：内部堆最低余量 / PSRAM 最低余量（M0 压力探测用） */
void system_heap_snapshot(uint32_t *internal_min, uint32_t *psram_min);

#ifdef __cplusplus
}
#endif
