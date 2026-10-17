#pragma once
/**
 * @file ui_camera_direct.h
 * @brief 拍照页旁路直绘：PPA 把 LVGL 浮层叠到相机预览上，整帧直接写屏（见 ui_camera_direct.c）。
 */

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

/** 拍照页创建时调用；失败时预览继续走 LVGL 路径 */
bool ui_camera_direct_init(lv_obj_t *camera_root, lv_obj_t *preview);
/** 相机任务：每帧先交给这里；返回 true 表示已合成上屏（不再走 LVGL 路径） */
bool ui_camera_direct_frame(const void *rgb565_480x320);
/** LVGL 线程定时调用：按需进入旁路、界面变化时重画浮层 */
void ui_camera_direct_tick(void);
/** LVGL 线程：离开拍照页时退出旁路（LVGL 重新接管整屏） */
void ui_camera_direct_exit(void);
/** 调试：旁路中最近一次合成帧（小端 RGB565 480x320），未开启时 NULL */
const void *ui_camera_direct_last_frame(void);
/** 调试：旁路累计上屏帧数（算帧率用） */
uint32_t ui_camera_direct_frames_shown(void);
