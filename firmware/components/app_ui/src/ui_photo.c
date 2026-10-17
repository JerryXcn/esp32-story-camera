/**
 * @file ui_photo.c
 * @brief 全屏照片与模糊背景（v0.2 全屏播放 / 生成中页面）。
 *
 * original.jpg（3:2：1280x848，或相机旋转 90/270 时 960x640；旧记录 1280x960）→ 硬件 JPEG 解码（大端 RGB565）
 * → 字节交换为 LVGL 小端 → PPA 缩放并居中裁切为 480x320；再缩成 60x40 作模糊背景。
 * PPA 缩放系数只有 1/16 精度：取最接近的整数档，输出不足的 1~2 行由背景色补齐。
 */

#include "ui_assets.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "lvgl.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/jpeg_decode.h"
#include "driver/ppa.h"

static const char *TAG = "photo";

#define FULL_W 480
#define FULL_H 320
#define BLUR_W 60
#define BLUR_H 40
#define ORIGINAL_MAX_BYTES (1536 * 1024)

typedef struct {
    char path[160];
    uint8_t *pixels;
    lv_image_dsc_t dsc;
} photo_slot_t;

static photo_slot_t s_full, s_blur;
static ppa_client_handle_t s_ppa;

static bool original_path(const char *thumb_path, char *out, size_t n)
{
    const char *slash = thumb_path ? strrchr(thumb_path, '/') : NULL;
    if (!slash) return false;
    int dir_len = (int)(slash - thumb_path);
    return snprintf(out, n, "%.*s/original.jpg", dir_len, thumb_path) < (int)n;
}

/* 等比缩放到 dw x dh 并居中裁切；系数取最接近的 1/16 档 */
static esp_err_t scale_cover(const uint8_t *src, uint32_t sw, uint32_t sh,
                             uint8_t *dst, uint32_t dw, uint32_t dh)
{
    if (!s_ppa) {
        ppa_client_config_t cfg = { .oper_type = PPA_OPERATION_SRM };
        esp_err_t err = ppa_register_client(&cfg, &s_ppa);
        if (err != ESP_OK) return err;
    }
    uint32_t qx = (dw * 16 + sw / 2) / sw, qy = (dh * 16 + sh / 2) / sh;
    uint32_t q = qx > qy ? qx : qy;
    if (q == 0) q = 1;
    uint32_t bw = (dw * 16 + q - 1) / q, bh = (dh * 16 + q - 1) / q;
    if (bw > sw) bw = sw;
    if (bh > sh) bh = sh;
    uint32_t ow = bw * q / 16, oh = bh * q / 16;
    if (ow > dw) ow = dw;
    if (oh > dh) oh = dh;
    ppa_srm_oper_config_t op = {
        .in = {
            .buffer = (void *)src, .pic_w = sw, .pic_h = sh,
            .block_w = bw, .block_h = bh,
            .block_offset_x = (sw - bw) / 2, .block_offset_y = (sh - bh) / 2,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = dst, .buffer_size = dw * dh * 2, .pic_w = dw, .pic_h = dh,
            .block_offset_x = (dw - ow) / 2, .block_offset_y = (dh - oh) / 2,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = (float)q / 16.0f,
        .scale_y = (float)q / 16.0f,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    return ppa_do_scale_rotate_mirror(s_ppa, &op);
}

static void slot_publish(photo_slot_t *slot, const char *path, uint8_t *pixels, uint32_t w, uint32_t h)
{
    if (slot->pixels) {
        lv_image_cache_drop(&slot->dsc);
        heap_caps_free(slot->pixels);
    }
    memset(slot, 0, sizeof(*slot));
    strlcpy(slot->path, path, sizeof(slot->path));
    slot->pixels = pixels;
    slot->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    slot->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    slot->dsc.header.w = w;
    slot->dsc.header.h = h;
    slot->dsc.header.stride = w * 2;
    slot->dsc.data = pixels;
    slot->dsc.data_size = w * h * 2;
}

/* 背景色（深棕）预填，PPA 输出不足的边行不会露出未初始化内存 */
static uint8_t *alloc_frame(uint32_t w, uint32_t h)
{
    uint8_t *p = heap_caps_aligned_alloc(64, (size_t)w * h * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) {
        uint16_t fill = 0x2104;    /* 接近黑色的深灰，RGB565 小端 */
        for (size_t i = 0; i < (size_t)w * h; i++) ((uint16_t *)p)[i] = fill;
    }
    return p;
}

static bool load(const char *thumb_path)
{
    char path[160];
    if (!original_path(thumb_path, path, sizeof(path))) return false;
    if (s_full.pixels && strcmp(s_full.path, path) == 0) return true;

    jpeg_decoder_handle_t dec = (jpeg_decoder_handle_t)ui_jpeg_decoder();
    struct stat st;
    if (!dec || stat(path, &st) != 0 || st.st_size < 512 || st.st_size > ORIGINAL_MAX_BYTES) return false;
    uint8_t *jpeg = heap_caps_malloc(st.st_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    FILE *f = jpeg ? fopen(path, "rb") : NULL;
    size_t len = f ? fread(jpeg, 1, st.st_size, f) : 0;
    if (f) fclose(f);
    jpeg_decode_picture_info_t info;
    if (!jpeg || len != (size_t)st.st_size || jpeg_decoder_get_info(jpeg, len, &info) != ESP_OK ||
        info.width == 0 || info.width > 1600 || info.height == 0 || info.height > 1200) {
        heap_caps_free(jpeg);
        return false;
    }
    uint32_t h_aligned = (info.height + 15) & ~15UL;
    size_t raw_size = (size_t)info.width * h_aligned * 2;
    uint8_t *raw = heap_caps_aligned_alloc(64, raw_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    jpeg_decode_cfg_t cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_RGB,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t out_len = 0;
    esp_err_t err = raw ? jpeg_decoder_process(dec, &cfg, jpeg, len, raw, raw_size, &out_len) : ESP_ERR_NO_MEM;
    heap_caps_free(jpeg);
    if (err != ESP_OK || out_len < (size_t)info.width * info.height * 2) {
        ESP_LOGW(TAG, "decode %s failed: %s", path, esp_err_to_name(err));
        heap_caps_free(raw);
        return false;
    }
    /* 硬件解码输出大端 RGB565；PPA 插值前转成小端，否则颜色分量会被混合 */
    uint16_t *px = (uint16_t *)raw;
    for (size_t i = 0; i < (size_t)info.width * info.height; i++) px[i] = __builtin_bswap16(px[i]);

    uint8_t *full = alloc_frame(FULL_W, FULL_H);
    uint8_t *blur = alloc_frame(BLUR_W, BLUR_H);
    bool ok = full && blur &&
              scale_cover(raw, info.width, info.height, full, FULL_W, FULL_H) == ESP_OK &&
              scale_cover(full, FULL_W, FULL_H, blur, BLUR_W, BLUR_H) == ESP_OK;
    heap_caps_free(raw);
    if (!ok) {
        heap_caps_free(full);
        heap_caps_free(blur);
        return false;
    }
    slot_publish(&s_full, path, full, FULL_W, FULL_H);
    slot_publish(&s_blur, path, blur, BLUR_W, BLUR_H);
    return true;
}

const lv_image_dsc_t *ui_photo_full(const char *thumb_path)
{
    return load(thumb_path) ? &s_full.dsc : NULL;
}

const lv_image_dsc_t *ui_photo_blur(const char *thumb_path)
{
    return load(thumb_path) ? &s_blur.dsc : NULL;
}
