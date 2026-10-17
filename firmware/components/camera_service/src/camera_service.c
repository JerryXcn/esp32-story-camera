/**
 * @file camera_service.c
 * @brief 相机采集实现（ESP32-P4 + OV5647 + MIPI-CSI，V4L2/esp_video）。
 *
 * 架构（对齐官方示例 09_video_lcd_display + 优化建议 CAM-01/02/03）：
 * - 唯一采集任务持有 /dev/video0 句柄；USERPTR 缓冲（PSRAM、cache 对齐），
 *   避免 mmap 长度/索引混乱；held_index/bytesused 独立跟踪（CAM-03）。
 * - 预览：PPA 硬件旋转+缩放（安装方向校正，见 CONFIG_STORY_CAMERA_*）→ 480x320 双缓冲 →
 *   回调 UI 切换显示；处理完立即 QBUF，不积压旧帧（CAM-01）。
 * - 拍照：同一采集循环内暂停预览投递，PPA 做与预览一致的方向校正（旋转+镜像）后，
 *   用 P4 硬件 JPEG 编码器（driver/jpeg_encode.h，与 esp_video JPEG 设备同
 *   驱动）编码原图与缩略图（CAM-02）；预览/保存构图严格一致。
 * - DQBUF 阻塞由 STREAMOFF 解除；停止流程可确认任务退出（CAM-03）。
 *
 * M0 结论（2026-09-12 实机）：esp_video 2.3.0 + esp_ipa 2.2.0~1
 * （rev v1.3/eco2 不支持 esp_ipa 2.3 预编译库的 Zba 位操作指令）。
 */

#include "camera_service.h"

#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_private/esp_cache_private.h"
#include "esp_memory_utils.h"
#include "linux/videodev2.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/ppa.h"
#include "driver/jpeg_encode.h"
#include "board_adapter.h"

static const char *TAG = "camera";

#define CSI_DEV            "/dev/video0"
#define BUF_COUNT          3
#define CAM_TASK_STACK     (6 * 1024)
#define CAM_TASK_PRIO      5
/* v0.2：全屏 3:2 预览（与 app_ui 全屏预览一致）。1280→480 为 6/16，PPA 整数档 */
#define PREVIEW_W          480
#define PREVIEW_H          320
/* 缩略图 3:2（240x160）；旧版本拍的 240x180 由 UI 兼容显示 */
#define THUMB_W            240
#define THUMB_H            160
/* PPA 采样步进上限（PPA SRM 缩放比限制 1/16 起） */
#define CAM_JPEG_QUALITY   80

/* 采集状态（CAM-03：集中管理，失败路径统一清理） */
typedef struct {
    int fd;                          /* -1 = 未打开 */
    uint8_t *buf[BUF_COUNT];         /* USERPTR 缓冲（PSRAM、cache 对齐） */
    size_t buf_size;                 /* 单缓冲容量（REQBUFS 返回） */
    uint32_t width, height;          /* G_FMT 协商结果 */
    uint32_t format;                 /* 实际像素格式 */
    uint32_t bytesperline;           /* 实际 stride */
    bool streaming;
    int held_index;                  /* 当前 DQBUF 未归还的缓冲；-1 = 无 */
    size_t bytesused;                /* held 帧有效字节数 */
    uint32_t frame_seq;
    volatile bool stop_req;          /* 采集任务退出请求 */
    TaskHandle_t task;
    SemaphoreHandle_t stopped_sem;   /* 任务退出确认 */
    /* 预览 */
    camera_frame_cb cb;
    void *cb_user;
    uint8_t *preview_buf[2];         /* PPA 输出双缓冲（352x264 RGB565） */
    int preview_front;               /* UI 当前显示的缓冲 */
    volatile bool preview_pause;     /* 拍照期间暂停预览投递 */
    /* 拍照 */
    volatile bool capture_req;
    SemaphoreHandle_t started_sem;
    SemaphoreHandle_t capture_done;  /* 拍照结果就绪 */
    camera_capture_t capture_result;
    esp_err_t capture_err;
    /* 工作缓冲 */
    uint8_t *work_buf;               /* 方向校正后的全尺寸 RGB565 */
    uint8_t *thumb_buf;              /* 240x180 RGB565 */
    uint8_t *jpeg_buf;               /* 原图 JPEG 输出（上限 4MiB） */
    uint8_t *thumb_jpeg_buf;         /* 缩略图 JPEG 输出 */
    size_t cache_align;
    /* 硬件 JPEG 引擎（懒初始化） */
    jpeg_encoder_handle_t jenc;
    /* PPA */
    ppa_client_handle_t ppa;
    /* 健康状态 */
    volatile uint32_t last_frame_ms;
    volatile uint32_t luma_mean, dark_pct;
} cam_state_t;

static cam_state_t s_cam = {
    .fd = -1,
    .held_index = -1,
    .stopped_sem = NULL,
};
/* STREAMOFF unblocks DQBUF in the owner task. The controller and owner must
 * share this lock so teardown cannot stop an already stopped ISP concurrently. */
static SemaphoreHandle_t s_stream_lock;

bool camera_service_ready(void)
{
    return s_cam.streaming && !s_cam.stop_req && s_cam.last_frame_ms &&
           (uint32_t)(esp_timer_get_time() / 1000) - s_cam.last_frame_ms < 1500;
}

uint32_t camera_last_frame_ms(void)
{
    return s_cam.last_frame_ms;
}

/* ---------- 内存与 PPA 工具 ---------- */

static void *alloc_psram_aligned(size_t size)
{
    return heap_caps_aligned_calloc(s_cam.cache_align, 1, size,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void free_psram(void *p)
{
    if (p) heap_caps_free(p);
}

/* 安装方向校正（CONFIG_STORY_CAMERA_ROTATION 逆时针度数 + CONFIG_STORY_CAMERA_MIRROR）。
 * 实测 PPA SRM：rotation_angle 为逆时针，mirror 作用在旋转之后的输出画面上。 */
#if CONFIG_STORY_CAMERA_ROTATION % 90 != 0 || CONFIG_STORY_CAMERA_ROTATION > 270
#error "STORY_CAMERA_ROTATION must be 0/90/180/270"
#endif
#define CAM_PPA_ROTATION   (CONFIG_STORY_CAMERA_ROTATION / 90)
#define CAM_ROT_SWAPS_WH   (CONFIG_STORY_CAMERA_ROTATION == 90 || CONFIG_STORY_CAMERA_ROTATION == 270)
#ifdef CONFIG_STORY_CAMERA_MIRROR
#define CAM_MIRROR         true
#else
#define CAM_MIRROR         false
#endif

/** PPA 旋转+缩放+镜像。阻塞式。sw×sh 为源图；dw×dh 为输出（旋转后的方向）。
 * PPA SRM 缩放系数只有 1/16 精度、向下截断：1280→352 的 0.275 实际按 0.25
 * 输出 320x240，预览右/下出现黑边。这里把系数向上取到 1/16 的整数倍，
 * 再居中裁切输入块，使输出恰好铺满 dw×dh（精确倍率时不裁切）。
 * 旋转 90/270 时裁切在旋转后的坐标里算，再换回源图坐标（宽高互换，居中对称）。 */
static esp_err_t ppa_scale(const uint8_t *src, uint32_t sw, uint32_t sh,
                           uint8_t *dst, uint32_t dw, uint32_t dh, bool mirror, int rotation)
{
    bool swap = rotation == PPA_SRM_ROTATION_ANGLE_90 || rotation == PPA_SRM_ROTATION_ANGLE_270;
    uint32_t rw = swap ? sh : sw, rh = swap ? sw : sh;    /* 旋转后的源图尺寸 */
    uint32_t q16x = (dw * 16 + rw - 1) / rw;    /* ceil(dw/rw * 16) */
    uint32_t q16y = (dh * 16 + rh - 1) / rh;
    uint32_t q16 = q16x > q16y ? q16x : q16y;
    uint32_t bw = (dw * 16 + q16 - 1) / q16;    /* ceil(dw / scale) */
    uint32_t bh = (dh * 16 + q16 - 1) / q16;
    if (bw > rw) bw = rw;
    if (bh > rh) bh = rh;
    uint32_t ibw = swap ? bh : bw, ibh = swap ? bw : bh;   /* 源图坐标里的输入块 */
    ppa_srm_oper_config_t cfg = {
        .in = {
            .buffer = (uint8_t *)src,
            .pic_w = sw, .pic_h = sh,
            .block_w = ibw, .block_h = ibh,
            .block_offset_x = (sw - ibw) / 2, .block_offset_y = (sh - ibh) / 2,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = dst,
            .buffer_size = dw * dh * 2,
            .pic_w = dw, .pic_h = dh,
            .block_offset_x = 0, .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = (ppa_srm_rotation_angle_t)rotation,
        .scale_x = (float)q16 / 16.0f,
        .scale_y = (float)q16 / 16.0f,
        .mirror_x = mirror,
        .mirror_y = false,
        .rgb_swap = 0,
        .byte_swap = 0,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    return ppa_do_scale_rotate_mirror(s_cam.ppa, &cfg);
}

/** 硬件 JPEG 编码 RGB565 → JPEG。dst_cap 输出容量，实际长度写 *out_len。 */
static esp_err_t jpeg_encode_rgb565(const uint8_t *src, uint32_t w, uint32_t h,
                                    uint8_t *dst, size_t dst_cap, size_t *out_len)
{
    if (!s_cam.jenc) {
        jpeg_encode_engine_cfg_t eng = {
            .intr_priority = 0,
            .timeout_ms = 2000,
        };
        ESP_RETURN_ON_ERROR(jpeg_new_encoder_engine(&eng, &s_cam.jenc), TAG, "jenc engine");
    }
    jpeg_encode_cfg_t cfg = {
        .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV422,
        .image_quality = CAM_JPEG_QUALITY,
        .width = w,
        .height = h,
    };
    uint32_t out_size = 0;
    ESP_RETURN_ON_ERROR(jpeg_encoder_process(s_cam.jenc, &cfg, (const uint8_t *)src,
                                             w * h * 2, dst,
                                             dst_cap, &out_size), TAG, "jpeg process");
    *out_len = out_size;
    return ESP_OK;
}

/* ---------- 拍照流水线（采集任务上下文执行） ---------- */

static esp_err_t capture_pipeline(const uint8_t *frame, size_t frame_len)
{
    camera_capture_t *r = &s_cam.capture_result;
    memset(r, 0, sizeof(*r));
    s_cam.capture_err = ESP_OK;

    if (frame_len != s_cam.buf_size) return ESP_ERR_INVALID_SIZE;
    /* 1) 方向校正：与预览完全一致的旋转/镜像，保证所见即所得（CAM-02） */
    uint32_t rw = CAM_ROT_SWAPS_WH ? s_cam.height : s_cam.width;
    uint32_t rh = CAM_ROT_SWAPS_WH ? s_cam.width : s_cam.height;
    ESP_RETURN_ON_FALSE(ppa_scale(frame, s_cam.width, s_cam.height,
                                  s_cam.work_buf, rw, rh, CAM_MIRROR, CAM_PPA_ROTATION) == ESP_OK,
                        ESP_FAIL, TAG, "ppa capture");

    /* 2) 原图 JPEG：居中裁成 3:2，与全屏预览所见一致
     *    （不旋转 1280x960 → 1280x848；旋转 90/270 为 960x1280 → 960x640）。
     *    高度取 16 的倍数便于硬件编码；裁切起点按整行偏移，缓冲地址保持 64 字节对齐 */
    uint32_t crop_h = (rw * 2 / 3) & ~15u;
    if (crop_h == 0 || crop_h > rh) crop_h = rh;
    uint32_t crop_y = ((rh - crop_h) / 2) & ~1u;
    const uint8_t *crop = s_cam.work_buf + (size_t)crop_y * rw * 2;
    size_t jlen = 0;
    ESP_RETURN_ON_FALSE(jpeg_encode_rgb565(crop, rw, crop_h,
                                           s_cam.jpeg_buf, 4 * 1024 * 1024, &jlen) == ESP_OK,
                        ESP_FAIL, TAG, "jpeg encode");
    if (jlen < 512) return ESP_ERR_INVALID_SIZE;
    r->jpeg = s_cam.jpeg_buf;
    r->jpeg_len = jlen;
    r->jpeg_w = rw;
    r->jpeg_h = crop_h;

    /* 3) 缩略图 240x160：整帧按 3/16 缩放并居中裁成 3:2（与原图裁切相差不到 4 行） */
    ESP_RETURN_ON_FALSE(ppa_scale(s_cam.work_buf, rw, rh,
                                  s_cam.thumb_buf, THUMB_W, THUMB_H, false,
                                  PPA_SRM_ROTATION_ANGLE_0) == ESP_OK,
                        ESP_FAIL, TAG, "ppa thumb");
    size_t tlen = 0;
    ESP_RETURN_ON_FALSE(jpeg_encode_rgb565(s_cam.thumb_buf, THUMB_W, THUMB_H,
                                           s_cam.thumb_jpeg_buf, 128 * 1024, &tlen) == ESP_OK,
                        ESP_FAIL, TAG, "jpeg thumb");
    r->thumb_jpeg = s_cam.thumb_jpeg_buf;
    r->thumb_len = tlen;
    return ESP_OK;
}

/* ---------- 传感器曝光（OV5647 片上 AEC） ---------- */

/* 驱动默认 1280x960 跑 45 帧/秒（帧长 1093 行），并按帧长把 50/60 Hz 防闪烁曝光上限算成 2 档（20 ms）；
 * 室内光线下曝光顶到 20 ms 后只能把增益拉到 9 倍以上，画面颗粒感重。预览实际上屏约 27 帧/秒，
 * 这里把帧长加到约 30 帧/秒（1640 行），防闪烁上限按新帧长重算为 3 档（30 ms）：
 * 2026-10-16 实测同一场景增益由 0x90 降到 0x68（约 9 倍 → 6.5 倍）。不再更长，避免手持拍摄拖影。 */
#define CAM_SENSOR_VTS 1640

static void sensor_tune_exposure(void)
{
    uint8_t s50h = 0, s50l = 0, s60h = 0, s60l = 0;
    if (board_adapter_cam_reg_read(0x3a08, &s50h) || board_adapter_cam_reg_read(0x3a09, &s50l) ||
        board_adapter_cam_reg_read(0x3a0a, &s60h) || board_adapter_cam_reg_read(0x3a0b, &s60l)) {
        ESP_LOGW(TAG, "sensor exposure tuning skipped (sccb read failed)");
        return;
    }
    uint32_t step50 = (uint32_t)(s50h & 0x03) << 8 | s50l;   /* 每档行数（10 ms / 8.3 ms） */
    uint32_t step60 = (uint32_t)(s60h & 0x03) << 8 | s60l;
    uint32_t max50 = step50 ? (CAM_SENSOR_VTS - 4) / step50 : 0;
    uint32_t max60 = step60 ? (CAM_SENSOR_VTS - 4) / step60 : 0;
    int err = board_adapter_cam_reg_write(0x380e, CAM_SENSOR_VTS >> 8);
    err |= board_adapter_cam_reg_write(0x380f, CAM_SENSOR_VTS & 0xff);
    if (max50) err |= board_adapter_cam_reg_write(0x3a0e, (uint8_t)(max50 > 0x3f ? 0x3f : max50));
    if (max60) err |= board_adapter_cam_reg_write(0x3a0d, (uint8_t)(max60 > 0x3f ? 0x3f : max60));
    ESP_LOGI(TAG, "sensor VTS=%d bands50=%lu bands60=%lu %s", CAM_SENSOR_VTS,
             (unsigned long)max50, (unsigned long)max60, err ? "FAIL" : "ok");
}

/* ---------- V4L2 生命周期（仅采集任务/启动前调用） ---------- */

static void camera_return_held(void)
{
    xSemaphoreTake(s_stream_lock, portMAX_DELAY);
    if (s_cam.held_index >= 0 && s_cam.fd >= 0 && s_cam.streaming) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_USERPTR;
        buf.index = (uint32_t)s_cam.held_index;
        buf.m.userptr = (unsigned long)s_cam.buf[s_cam.held_index];
        buf.length = s_cam.buf_size;
        if (ioctl(s_cam.fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "QBUF[%d] failed: errno %d", s_cam.held_index, errno);
        }
        s_cam.held_index = -1;
        s_cam.bytesused = 0;
    }
    xSemaphoreGive(s_stream_lock);
}

static void camera_streamoff_locked(void)
{
    if (s_cam.fd < 0 || !s_cam.streaming) return;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_cam.fd, VIDIOC_STREAMOFF, &type) == 0) s_cam.streaming = false;
    else ESP_LOGE(TAG, "STREAMOFF failed: errno %d", errno);
}

static void camera_teardown(void)
{
    xSemaphoreTake(s_stream_lock, portMAX_DELAY);
    if (s_cam.fd < 0) { xSemaphoreGive(s_stream_lock); return; }
    camera_streamoff_locked();
    s_cam.held_index = -1;
    s_cam.bytesused = 0;
    /* REQBUFS(0) 释放驱动侧缓冲登记 */
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_USERPTR;
    req.count = 0;
    ioctl(s_cam.fd, VIDIOC_REQBUFS, &req);
    close(s_cam.fd);
    s_cam.fd = -1;
    s_cam.last_frame_ms = 0;
    xSemaphoreGive(s_stream_lock);
    ESP_LOGI(TAG, "camera stream closed");
}

static esp_err_t camera_setup_stream(void)
{
    if (s_cam.fd >= 0) return ESP_OK;

    int fd = open(CSI_DEV, O_RDWR);
    if (fd < 0) {
        ESP_LOGE(TAG, "open %s failed: errno %d", CSI_DEV, errno);
        return ESP_ERR_NOT_FOUND;
    }
    s_cam.fd = fd;

    /* G_FMT：尺寸由 sensor/ISP 协商，不可指定（官方示例同策略） */
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_G_FMT, &fmt) != 0) {
        ESP_LOGE(TAG, "G_FMT failed: errno %d", errno);
        camera_teardown();
        return ESP_FAIL;
    }
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565) {
        struct v4l2_format set;
        memset(&set, 0, sizeof(set));
        set.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        set.fmt.pix.width = fmt.fmt.pix.width;
        set.fmt.pix.height = fmt.fmt.pix.height;
        set.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
        if (ioctl(fd, VIDIOC_S_FMT, &set) != 0) {
            ESP_LOGE(TAG, "S_FMT failed: errno %d", errno);
            camera_teardown();
            return ESP_FAIL;
        }
        fmt = set;
    }
    s_cam.width = fmt.fmt.pix.width;
    s_cam.height = fmt.fmt.pix.height;
    s_cam.format = fmt.fmt.pix.pixelformat;
    s_cam.bytesperline = fmt.fmt.pix.bytesperline;
    if (!s_cam.width || !s_cam.height || s_cam.width > 1600 || s_cam.height > 1200 ||
        s_cam.format != V4L2_PIX_FMT_RGB565 ||
        (s_cam.bytesperline && s_cam.bytesperline != s_cam.width * 2)) {
        ESP_LOGE(TAG, "unsupported RGB565 size/stride");
        camera_teardown(); return ESP_ERR_NOT_SUPPORTED;
    }
    ESP_LOGI(TAG, "stream %lux%lu stride=%lu sizeimage=%lu",
             (unsigned long)s_cam.width, (unsigned long)s_cam.height,
             (unsigned long)s_cam.bytesperline, (unsigned long)fmt.fmt.pix.sizeimage);

    /* USERPTR：PSRAM cache 对齐缓冲（官方示例做法），容量以驱动返回为准 */
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = BUF_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_USERPTR;
    if (ioctl(fd, VIDIOC_REQBUFS, &req) != 0 || req.count < 2 || req.count > BUF_COUNT) {
        ESP_LOGE(TAG, "REQBUFS failed: errno %d count=%lu", errno, (unsigned long)req.count);
        camera_teardown();
        return ESP_FAIL;
    }
    /* USERPTR 缓冲大小自行计算（G_FMT 的 sizeimage 为 0；esp_video 内部
     * 同样按 width*height*bpp/8 计算元素大小，官方示例同策略） */
    s_cam.buf_size = (size_t)s_cam.width * s_cam.height * 2;   /* RGB565 */
    for (int i = 0; i < (int)req.count; i++) {
        if (!s_cam.buf[i]) {
            s_cam.buf[i] = alloc_psram_aligned(s_cam.buf_size);
            if (!s_cam.buf[i]) {
                ESP_LOGE(TAG, "frame buf[%d] alloc failed", i);
                camera_teardown();
                return ESP_ERR_NO_MEM;
            }
        }
    }
    for (int i = 0; i < (int)req.count; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_USERPTR;
        buf.index = (uint32_t)i;
        buf.m.userptr = (unsigned long)s_cam.buf[i];
        buf.length = s_cam.buf_size;
        if (ioctl(fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "QBUF[%d] failed: errno %d", i, errno);
            camera_teardown();
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

typedef struct {
    camera_frame_stats_t stats;
    uint8_t *dump;
    uint32_t width, height;
    SemaphoreHandle_t done;
} debug_request_t;
static SemaphoreHandle_t s_debug_lock;
static debug_request_t *s_debug_request;
static void luma_stats(const uint8_t *rgb565, uint32_t w, uint32_t h, uint32_t *mean, uint32_t *dark_pct);
static void satisfy_debug(const uint8_t *frame)
{
    if (!s_debug_lock || xSemaphoreTake(s_debug_lock, 0) != pdTRUE) return;
    debug_request_t *req = s_debug_request;
    if (req) {
        req->stats = (camera_frame_stats_t){ .width = s_cam.width, .height = s_cam.height,
            .bytes = s_cam.bytesused, .luma_mean = s_cam.luma_mean, .luma_dark_pct = s_cam.dark_pct };
        if (req->dump) {
            req->width = s_cam.width / 2; req->height = s_cam.height / 2;
            if (req->width > 640) { req->width = 640; req->height = s_cam.height * 640 / s_cam.width; }
            if (req->height > 480) { req->height = 480; req->width = s_cam.width * 480 / s_cam.height; }
            for (uint32_t y = 0; y < req->height; ++y) {
                for (uint32_t x = 0; x < req->width; ++x) {
                    size_t src = ((size_t)(y * s_cam.height / req->height) * s_cam.width + x * s_cam.width / req->width) * 2;
                    size_t dst = ((size_t)y * req->width + x) * 2;
                    req->dump[dst] = frame[src]; req->dump[dst + 1] = frame[src + 1];
                }
            }
        }
        s_debug_request = NULL;
        xSemaphoreGive(req->done);
    }
    xSemaphoreGive(s_debug_lock);
}

/* ---------- 采集任务 ---------- */

static void camera_task(void *arg)
{
    (void)arg;
    esp_err_t err = camera_setup_stream();
    if (err != ESP_OK) {
        s_cam.capture_err = err;
        xSemaphoreGive(s_cam.started_sem);   /* 启动失败唤醒等待者 */
        goto exit_task;
    }
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    xSemaphoreTake(s_stream_lock, portMAX_DELAY);
    int stream_result = ioctl(s_cam.fd, VIDIOC_STREAMON, &type);
    s_cam.streaming = stream_result == 0;
    xSemaphoreGive(s_stream_lock);
    if (stream_result != 0) {
        ESP_LOGE(TAG, "STREAMON failed: errno %d", errno);
        s_cam.streaming = false;
        s_cam.capture_err = ESP_FAIL;
        xSemaphoreGive(s_cam.started_sem);
        goto exit_task;
    }
    ESP_LOGI(TAG, "camera stream on");
    sensor_tune_exposure();
    s_cam.capture_err = ESP_OK;
    bool first_frame = true;

    for (;;) {
        if (s_cam.stop_req) break;
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_USERPTR;
        if (ioctl(s_cam.fd, VIDIOC_DQBUF, &buf) != 0) {
            if (s_cam.stop_req) break;        /* STREAMOFF 解除阻塞 */
            ESP_LOGE(TAG, "DQBUF failed: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (buf.index >= BUF_COUNT || !s_cam.buf[buf.index]) {
            ESP_LOGE(TAG, "invalid dequeued index %lu", (unsigned long)buf.index);
            break;
        }
        s_cam.held_index = (int)buf.index;
        s_cam.bytesused = buf.bytesused;
        if (s_cam.bytesused != s_cam.buf_size) {
            ESP_LOGE(TAG, "incomplete frame: %lu expected %lu", (unsigned long)s_cam.bytesused, (unsigned long)s_cam.buf_size);
            camera_return_held(); continue;
        }
        if (first_frame) { first_frame = false; xSemaphoreGive(s_cam.started_sem); }
        /* 亮度只用于"太暗"提示：每 4 帧稀疏统计一次（整帧逐点读 PSRAM 每帧约 69 ms，曾是预览最大瓶颈） */
        if ((s_cam.frame_seq & 3) == 0) {
            uint32_t mean, dark;
            luma_stats(s_cam.buf[buf.index], s_cam.width, s_cam.height, &mean, &dark);
            s_cam.luma_mean = mean; s_cam.dark_pct = dark;
        }
        if (s_cam.frame_seq % 60 == 0) ESP_LOGI(TAG, "frame=%lu bytes=%lu Y=%lu dark=%lu%%", (unsigned long)s_cam.frame_seq,
            (unsigned long)s_cam.bytesused, (unsigned long)s_cam.luma_mean, (unsigned long)s_cam.dark_pct);
        satisfy_debug(s_cam.buf[buf.index]);
        s_cam.frame_seq++;
        s_cam.last_frame_ms = (uint32_t)(esp_timer_get_time() / 1000);

        if (s_cam.capture_req) {
            /* 拍照：串行完成编码后回帧（不并发访问驱动缓冲） */
            s_cam.capture_err = capture_pipeline(s_cam.buf[buf.index], s_cam.bytesused);
            s_cam.capture_req = false;
            xSemaphoreGive(s_cam.capture_done);
        } else if (s_cam.cb && !s_cam.preview_pause) {
            /* 预览：PPA 写入后台缓冲，完成后交 UI 切换 */
            int back = 1 - s_cam.preview_front;
            if (ppa_scale(s_cam.buf[buf.index], s_cam.width, s_cam.height,
                          s_cam.preview_buf[back], PREVIEW_W, PREVIEW_H, CAM_MIRROR,
                          CAM_PPA_ROTATION) == ESP_OK) {
                camera_frame_t frame = {
                    .data = s_cam.preview_buf[back],
                    .width = PREVIEW_W,
                    .height = PREVIEW_H,
                    .frame_seq = s_cam.frame_seq,
                };
                s_cam.cb(&frame, s_cam.cb_user);   /* UI 锁内只换源+invalidate */
                s_cam.preview_front = back;
            }
        }
        camera_return_held();
    }

exit_task:
    camera_teardown();
    if (s_cam.capture_req) { s_cam.capture_err = ESP_ERR_INVALID_STATE; s_cam.capture_req = false; xSemaphoreGive(s_cam.capture_done); }
    s_cam.task = NULL;
    xSemaphoreTake(s_cam.stopped_sem, 0);       /* 清陈旧计数 */
    xSemaphoreGive(s_cam.stopped_sem);
    vTaskDelete(NULL);
}

/* ---------- 对外接口 ---------- */

esp_err_t camera_start_preview(camera_frame_cb cb, void *user)
{
    if (s_cam.task && s_cam.stop_req) return ESP_ERR_INVALID_STATE;
    if (s_cam.task) {                       /* 已在运行：仅更新回调 */
        s_cam.cb = cb;
        s_cam.cb_user = user;
        s_cam.preview_pause = false;
        return ESP_OK;
    }
    if (!s_cam.cache_align) {
        size_t align = 0;
        if (esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &align) != ESP_OK || align == 0) {
            align = 64;
        }
        s_cam.cache_align = align;
    }
    if (!s_cam.ppa) {
        ppa_client_config_t pcfg = { .oper_type = PPA_OPERATION_SRM };
        ESP_RETURN_ON_ERROR(ppa_register_client(&pcfg, &s_cam.ppa), TAG, "ppa");
    }
    if (!s_cam.stopped_sem) {
        s_stream_lock = xSemaphoreCreateMutex();
        s_cam.stopped_sem = xSemaphoreCreateBinary();
        s_cam.capture_done = xSemaphoreCreateBinary();
        s_cam.started_sem = xSemaphoreCreateBinary();
        s_debug_lock = xSemaphoreCreateMutex();
        if (!s_stream_lock || !s_cam.stopped_sem || !s_cam.capture_done || !s_cam.started_sem || !s_debug_lock) return ESP_ERR_NO_MEM;
    }
    if (!s_cam.preview_buf[0]) {
        s_cam.preview_buf[0] = alloc_psram_aligned(PREVIEW_W * PREVIEW_H * 2);
        s_cam.preview_buf[1] = alloc_psram_aligned(PREVIEW_W * PREVIEW_H * 2);
        s_cam.work_buf = alloc_psram_aligned((size_t)1600 * 1200 * 2);   /* 上限尺寸预留 */
        s_cam.thumb_buf = alloc_psram_aligned(THUMB_W * THUMB_H * 2);
        s_cam.jpeg_buf = alloc_psram_aligned(4 * 1024 * 1024);
        s_cam.thumb_jpeg_buf = alloc_psram_aligned(128 * 1024);
        if (!s_cam.preview_buf[0] || !s_cam.preview_buf[1] || !s_cam.work_buf ||
            !s_cam.thumb_buf || !s_cam.jpeg_buf || !s_cam.thumb_jpeg_buf) {
            free_psram(s_cam.preview_buf[0]); free_psram(s_cam.preview_buf[1]);
            free_psram(s_cam.work_buf); free_psram(s_cam.thumb_buf);
            free_psram(s_cam.jpeg_buf); free_psram(s_cam.thumb_jpeg_buf);
            s_cam.preview_buf[0] = s_cam.preview_buf[1] = NULL;
            s_cam.work_buf = s_cam.thumb_buf = s_cam.jpeg_buf = s_cam.thumb_jpeg_buf = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    s_cam.cb = cb;
    s_cam.cb_user = user;
    s_cam.stop_req = false;
    s_cam.preview_pause = false;
    s_cam.capture_req = false;
    s_cam.capture_err = ESP_ERR_INVALID_STATE;
    s_cam.last_frame_ms = 0;
    xSemaphoreTake(s_cam.started_sem, 0);
    xSemaphoreTake(s_cam.capture_done, 0);
    xSemaphoreTake(s_cam.stopped_sem, 0);
    if (xTaskCreate(camera_task, "camera", CAM_TASK_STACK, NULL, CAM_TASK_PRIO,
                    &s_cam.task) != pdPASS) {
        s_cam.task = NULL;
        return ESP_ERR_NO_MEM;
    }
    /* 等待取流就绪或失败（超时 8s：相机就绪 P95 目标） */
    if (xSemaphoreTake(s_cam.started_sem, pdMS_TO_TICKS(8000)) != pdTRUE) {
        ESP_LOGE(TAG, "camera stream start timeout");
        camera_stop_preview();
        return ESP_ERR_TIMEOUT;
    }
    return s_cam.capture_err;
}

void camera_stop_preview(void)
{
    if (!s_cam.task) return;
    s_cam.stop_req = true;
    xSemaphoreTake(s_stream_lock, portMAX_DELAY);
    camera_streamoff_locked();   /* 解除 DQBUF 阻塞；任务清理不再重复停止 */
    xSemaphoreGive(s_stream_lock);
    if (xSemaphoreTake(s_cam.stopped_sem, pdMS_TO_TICKS(3000)) != pdTRUE) {
        ESP_LOGE(TAG, "camera task stop timeout");
    }
    s_cam.cb = NULL;
}

esp_err_t camera_capture_jpeg(camera_capture_t *out)
{
    if (!s_cam.task || !s_cam.streaming) return ESP_ERR_INVALID_STATE;
    if (s_cam.capture_req) return ESP_ERR_INVALID_STATE;
    s_cam.preview_pause = true;             /* 冻结预览投递（拍照中遮罩期间 UI 不再刷新） */
    s_cam.capture_err = ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_cam.capture_done, 0);
    s_cam.capture_req = true;
    if (xSemaphoreTake(s_cam.capture_done, pdMS_TO_TICKS(3000)) != pdTRUE) {
        /* The owner may still be encoding. Keep the request busy until it acknowledges. */
        s_cam.preview_pause = false;
        return ESP_ERR_TIMEOUT;
    }
    s_cam.preview_pause = false;
    if (s_cam.capture_err != ESP_OK) return s_cam.capture_err;
    *out = s_cam.capture_result;
    return ESP_OK;
}

void camera_release_jpeg(void)
{
    s_cam.capture_result.jpeg = NULL;
    s_cam.capture_result.thumb_jpeg = NULL;
}

/* ---------- 诊断路径（独立于预览任务；调试控制台专用） ---------- */

/** 亮度均值与暗像素比例：16x16 网格稀疏采样（1280x960 约 4800 点） */
static void luma_stats(const uint8_t *rgb565, uint32_t w, uint32_t h,
                       uint32_t *mean, uint32_t *dark_pct)
{
    uint64_t sum = 0;
    uint32_t dark = 0, n = 0;
    for (uint32_t y = 8; y < h; y += 16) {
        const uint8_t *row = rgb565 + (size_t)y * w * 2;
        for (uint32_t x = 8; x < w; x += 16) {
            uint16_t v = (uint16_t)(row[2 * x] | (row[2 * x + 1] << 8));
            uint32_t r = ((v >> 11) & 0x1F) << 3;
            uint32_t g = ((v >> 5) & 0x3F) << 2;
            uint32_t b = (v & 0x1F) << 3;
            uint32_t yv = (r * 77 + g * 150 + b * 29) >> 8;
            sum += yv;
            if (yv <= 31) dark++;
            n++;
        }
    }
    *mean = n ? (uint32_t)(sum / n) : 0;
    *dark_pct = n ? dark * 100 / n : 0;
}

uint32_t camera_luma_mean(void) { return s_cam.luma_mean; }
esp_err_t camera_debug_grab_stats(camera_frame_stats_t *out, uint8_t *dump_dst,
                                  size_t dump_cap, uint32_t *dump_w, uint32_t *dump_h)
{
    if (!out || (dump_dst && dump_cap < 640 * 480 * 2)) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    bool started_here = !s_cam.task;
    if (started_here && camera_start_preview(NULL, NULL) != ESP_OK) return ESP_FAIL;
    if (!s_debug_lock || s_cam.stop_req) return ESP_ERR_INVALID_STATE;
    debug_request_t *req = calloc(1, sizeof(*req));
    if (!req) return ESP_ERR_NO_MEM;
    req->done = xSemaphoreCreateBinary();
    if (dump_dst) req->dump = heap_caps_malloc(640 * 480 * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    esp_err_t err = ESP_ERR_NO_MEM;
    if (req->done && (!dump_dst || req->dump)) {
        xSemaphoreTake(s_debug_lock, portMAX_DELAY);
        bool busy = s_debug_request != NULL;
        if (!busy) s_debug_request = req;
        xSemaphoreGive(s_debug_lock);
        err = busy ? ESP_ERR_INVALID_STATE : ESP_ERR_TIMEOUT;
        if (!busy && xSemaphoreTake(req->done, pdMS_TO_TICKS(3000)) == pdTRUE) err = ESP_OK;
        xSemaphoreTake(s_debug_lock, portMAX_DELAY);
        if (s_debug_request == req) s_debug_request = NULL;
        if (err == ESP_OK) {
            *out = req->stats;
            if (dump_dst) {
                memcpy(dump_dst, req->dump, (size_t)req->width * req->height * 2);
                if (dump_w) *dump_w = req->width;
                if (dump_h) *dump_h = req->height;
            }
        }
        xSemaphoreGive(s_debug_lock);
    }
    if (req->done) vSemaphoreDelete(req->done);
    heap_caps_free(req->dump); free(req);
    if (started_here) camera_stop_preview();
    return err;
}

