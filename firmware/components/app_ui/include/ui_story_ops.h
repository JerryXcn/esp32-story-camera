#pragma once
/**
 * @file ui_story_ops.h
 * @brief 故事库后台操作：删除（加载遮罩 + 完成提示）与收藏（见 ui_story_ops.c）。
 */

#include <stdbool.h>
#include "storage_service.h"

/** 删除完成（LVGL 线程，已持锁）：deleted 为成功条数，用于退出选择模式/跳转页面 */
typedef void (*ui_ops_done_cb)(int deleted);

void ui_ops_init(void);
/** LVGL 线程：显示"正在删除…"并在后台删除；生成中或已有删除在进行时返回 false */
bool ui_ops_delete(const char (*ids)[STORAGE_ID_HEX], int count, ui_ops_done_cb done);
/** LVGL 线程：立即更新界面里的收藏标记，后台写盘（失败回滚并提示） */
void ui_ops_set_favorite(const char *story_id, bool favorite);
/** LVGL 线程：批量收藏/取消收藏（界面立即更新，后台逐条写盘，失败回滚并提示） */
bool ui_ops_set_favorite_many(const char (*ids)[STORAGE_ID_HEX], int count, bool favorite);
