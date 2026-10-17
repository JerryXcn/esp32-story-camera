#pragma once
/**
 * @file camera_service.h
 * @brief 相机采集（实施计划 3/5.1）：预览帧生命周期与单张拍照。
 *
 * 帧所有权规则（优化建议 CAM-01/03）：
 * - 本组件内部唯一任务拥有 V4L2 句柄与全部驱动缓冲；DQBUF 后的帧
 *   在 QBUF 归还前绝不被第二个上下文引用。
 * - 预览：PPA 缩放到组件私有的双缓冲后回调 UI；UI 只读最新完成帧。
 * - 拍照：与预览同一采集循环内串行完成（PPA 方向校正 → 硬件 JPEG 编码
 *   原图/缩略图），结果缓冲在 camera_release_jpeg 前由本组件持有。
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 预览帧（已缩放到 UI 预览尺寸的 RGB565，由本组件双缓冲轮换）。 */
typedef struct {
    const uint8_t *data;    /* RGB565 */
    int width;
    int height;
    uint32_t frame_seq;     /* UI 用 sequence 丢弃迟到帧 */
} camera_frame_t;

/** 预览回调：相机任务上下文执行，只允许短暂操作（如 LVGL 锁内换源后 invalidate）。 */
typedef void (*camera_frame_cb)(const camera_frame_t *frame, void *user);

/** 拍照结果：JPEG 原图与缩略图（缓冲归本组件，保存完成后 camera_release_jpeg）。 */
typedef struct {
    const uint8_t *jpeg;        /* 原图 JPEG（方向与预览一致，居中裁成 3:2：1280x848） */
    size_t jpeg_len;
    uint32_t jpeg_w;
    uint32_t jpeg_h;
    const uint8_t *thumb_jpeg;  /* 240x160 缩略图 JPEG（3:2，与原图同一裁切） */
    size_t thumb_len;
} camera_capture_t;

/** 相机是否已完成初始化（esp_video_init + 传感器识别）。 */
bool camera_service_ready(void);

/** 最近一次成功出帧的单调时间（ms）；无帧超时检测用。 */
uint32_t camera_last_frame_ms(void);
uint32_t camera_luma_mean(void);

/** 启动预览（幂等）：打开取流并开始向 cb 投递缩放后帧。 */
esp_err_t camera_start_preview(camera_frame_cb cb, void *user);

/** 停止预览并释放取流（阻塞至采集任务确认退出）。 */
void camera_stop_preview(void);

/** 拍照：暂停预览显示，串行完成方向校正 + JPEG 编码；结果经 out 返回。
 *  调用完成后必须调用 camera_release_jpeg。 */
esp_err_t camera_capture_jpeg(camera_capture_t *out);

void camera_release_jpeg(void);

/* ---------- 诊断（调试控制台用；经采集任务抓取，不与预览争抢 DQBUF） ---------- */

typedef struct {
    uint32_t width;
    uint32_t height;
    size_t bytes;          /* 帧有效字节数 */
    uint32_t luma_mean;    /* 解码 RGB 后计算的真实 Y 均值（0..255） */
    uint32_t luma_dark_pct; /* Y 落在 0..31 的像素占比（黑帧判定） */
} camera_frame_stats_t;

/** 抓一帧并计算亮度统计（真实 Y：77R+150G+29B，非字节均值）。
 *  dst 非空时额外输出 ≤640x480 的 RGB565 等比缩放副本（诊断导出，
 *  调用方提供 ≥640x480x2 缓冲）；导出尺寸写入 ow/oh。 */
esp_err_t camera_debug_grab_stats(camera_frame_stats_t *out, uint8_t *dump_dst,
                                  size_t dump_cap, uint32_t *dump_w, uint32_t *dump_h);

#ifdef __cplusplus
}
#endif
