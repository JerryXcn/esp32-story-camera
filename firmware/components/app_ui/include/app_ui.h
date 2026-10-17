#pragma once
/**
 * @file app_ui.h
 * @brief UI 入口（实施计划 4/5）：480x320 横屏、12 画面、控件与位图分离。
 * 坐标原点左上；未实现控件隐藏，不保留假按钮（产品文档 4）。
 */

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"
#include "storage_service.h"
#include "story_service.h"

#ifdef __cplusplus
extern "C" {
#endif

void app_ui_init(void);

/** 故事列表模型（按 capture_seq 倒序，storage_recover_all 产出） */
void app_ui_set_story_list(const story_summary_t *list, int count);
void app_ui_set_camera_available(bool ok);
void app_ui_set_storage_available(bool ok);
void app_ui_set_scan_pending(bool pending);   /* 恢复扫描进行中（区分空库） */
bool app_ui_scan_pending(void);
bool app_ui_sd_ok(void);
/** 快门可用 = 相机就绪且卡可写（产品文档 5.2 画面2） */
bool app_ui_shutter_ready(void);

/** 屏幕间共享的当前条目与列表访问（ui_screens_*.c 使用） */
const story_summary_t *app_ui_list(void);
int app_ui_list_count(void);
bool app_ui_camera_ok(void);
void app_ui_set_current_story(const story_summary_t *s);
const story_summary_t *app_ui_current_story(void);

/** story_service 事件入口（内部转 LVGL 线程） */
void app_ui_on_story_event(const story_event_t *ev);

/** 更新开机页的真实初始化状态文本（内部加 LVGL 锁，可在任意任务调用）。 */
void app_ui_boot_status(const char *text);

/** 进入主界面（首页）。启动阶段全部完成后调用。 */
void app_ui_enter_main(void);

/** 调试命令用：回到开机页（调用方须持有 LVGL 锁） */
void app_ui_debug_reset(void);

/** 调试命令用：切换到指定页面 ID（详见 ui_screens.h 的 screen_id_t）。 */
void app_ui_debug_goto(int screen_id);

/** 调试命令用：以故事库/详情/播放页打开当前列表第 index 条。 */
void app_ui_debug_open_story(int index);

void app_ui_refresh_library(void);
/** LVGL 线程：把已删除的故事从列表中移除，并在之后的扫描结果里过滤掉 */
void app_ui_forget_stories(const char (*ids)[STORAGE_ID_HEX], int n);
/** LVGL 线程：更新列表与当前故事的收藏标记（写盘由 ui_story_ops 负责） */
void app_ui_list_set_favorite(const char *story_id, bool favorite);
int app_ui_favorite_count(void);

#ifdef __cplusplus
}
#endif
