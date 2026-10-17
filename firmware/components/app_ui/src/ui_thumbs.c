/**
 * @file ui_thumbs.c
 * @brief 缩略图解码缓存（优化建议 7.2/8：最多约 8 张缓存，复用图片缓冲）。
 *
 * thumb.jpg（240x160，旧记录 240x180）经 P4 硬件 JPEG 解码为 RGB565，包装成
 * lv_image_dsc_t 直接供 lv_image 显示。LRU 缓存上限 8 张（约 675KB PSRAM）；
 * 删除/刷新后整体失效。解码在调用者上下文同步执行（单张约几十 ms）。
 */

#include "ui_assets.h"
#include "storage_service.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "lvgl.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/jpeg_decode.h"

static const char *TAG = "thumbs";

#define THUMB_CACHE_N   8
#define THUMB_MAX_BYTES STORAGE_THUMB_MAX_BYTES

typedef struct {
    char path[128];
    uint8_t *pixels;                 /* RGB565 */
    lv_image_dsc_t dsc;
    unsigned refs;
    bool stale;
    uint32_t last_use;
} thumb_entry_t;

static thumb_entry_t s_cache[THUMB_CACHE_N];
static uint32_t s_use_counter;
static jpeg_decoder_handle_t s_dec;

static void cache_init_decoder(void)
{
    if (s_dec) return;
    jpeg_decode_engine_cfg_t eng = {
        .intr_priority = 0,
        .timeout_ms = 1000,
    };
    if (jpeg_new_decoder_engine(&eng, &s_dec) != ESP_OK) {
        ESP_LOGW(TAG, "jpeg decoder engine alloc failed");
    }
}

/** 共享硬件 JPEG 解码器（P4 只有一个 JPEG 单元；全屏照片加载复用） */
void *ui_jpeg_decoder(void)
{
    cache_init_decoder();
    return s_dec;
}

const lv_image_dsc_t *ui_thumb_get(const char *path)
{
    if (!path || !path[0]) return NULL;
    s_use_counter++;

    for (int i = 0; i < THUMB_CACHE_N; i++) {
        if (s_cache[i].pixels && !s_cache[i].stale && strcmp(s_cache[i].path, path) == 0) {
            s_cache[i].last_use = s_use_counter;
            return &s_cache[i].dsc;
        }
    }

    cache_init_decoder();
    if (!s_dec) return NULL;

    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *jpeg = malloc(THUMB_MAX_BYTES);
    size_t jpeg_len = 0;
    if (jpeg) {
        jpeg_len = fread(jpeg, 1, THUMB_MAX_BYTES, f);
    }
    fclose(f);
    if (!jpeg || jpeg_len < 512) {
        free(jpeg);
        return NULL;
    }

    jpeg_decode_picture_info_t info;
    if (jpeg_decoder_get_info(jpeg, jpeg_len, &info) != ESP_OK) {
        free(jpeg);
        return NULL;
    }
    /* v0.2 起 240x160（3:2）；旧记录 240x180（4:3）继续兼容 */
    if (info.width != 240 || (info.height != 160 && info.height != 180)) {
        free(jpeg);
        return NULL;
    }
    /* 硬件解码器按 16 行对齐输出（180 行实际输出 88320B），按对齐高度分配 */
    uint32_t h_aligned = (info.height + 15) & ~15UL;
    size_t out_size = (size_t)info.width * h_aligned * 2;
    uint8_t *pixels = heap_caps_aligned_alloc(64, out_size, MALLOC_CAP_SPIRAM);
    if (!pixels) {
        free(jpeg);
        return NULL;
    }
    jpeg_decode_cfg_t dcfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_RGB,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t out_len = 0;
    esp_err_t err = jpeg_decoder_process(s_dec, &dcfg, jpeg, jpeg_len,
                                         pixels, out_size, &out_len);
    free(jpeg);
    if (err != ESP_OK || out_len > out_size || out_len < info.width * info.height * 2) {
        heap_caps_free(pixels);
        return NULL;
    }

    /* P4 JPEG RGB565 output is big endian; LVGL's RGB565 buffer is native
     * little endian. CSI/PPA preview and the saved JPEG are already correct.
     * Convert only this decoded display buffer, before publishing it. */
    for (size_t i = 0; i < (size_t)info.width * info.height * 2; i += 2) {
        uint8_t high = pixels[i];
        pixels[i] = pixels[i + 1];
        pixels[i + 1] = high;
    }

    /* LRU：找空闲槽或最久未用槽 */
    int slot = -1;
    for (int i = 0; i < THUMB_CACHE_N; i++) {
        if (!s_cache[i].pixels) { slot = i; break; }
    }
    if (slot < 0) {
        for (int i = 0; i < THUMB_CACHE_N; ++i) {
            if (!s_cache[i].refs && (slot < 0 || s_cache[i].last_use < s_cache[slot].last_use)) slot = i;
        }
        if (slot < 0) { heap_caps_free(pixels); return NULL; }
        lv_image_cache_drop(&s_cache[slot].dsc);
        heap_caps_free(s_cache[slot].pixels);
    }
    thumb_entry_t *e = &s_cache[slot];
    strlcpy(e->path, path, sizeof(e->path));
    e->pixels = pixels;
    e->refs = 0;
    e->stale = false;
    e->last_use = s_use_counter;
    memset(&e->dsc, 0, sizeof(e->dsc));
    e->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    e->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    e->dsc.header.w = info.width;
    e->dsc.header.h = info.height;
    e->dsc.header.stride = info.width * 2;
    e->dsc.data = pixels;
    e->dsc.data_size = out_size;
    return &e->dsc;
}

static void release_image(lv_obj_t *img)
{
    thumb_entry_t *e = lv_obj_get_user_data(img);
    if (e && e->refs) {
        --e->refs;
        if (!e->refs && e->stale) {
            lv_image_cache_drop(&e->dsc);
            heap_caps_free(e->pixels);
            memset(e, 0, sizeof(*e));
        }
    }
    lv_obj_set_user_data(img, NULL);
}
static void thumb_deleted(lv_event_t *ev) { release_image(lv_event_get_target(ev)); }

bool ui_thumb_bind(lv_obj_t *img, const char *path, int32_t w, int32_t h)
{
    if (!img) return false;
    lv_image_set_src(img, NULL);
    release_image(img);
    lv_obj_remove_event_cb(img, thumb_deleted);
    const lv_image_dsc_t *dsc = ui_thumb_get(path);
    if (!dsc) { lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN); return false; }
    thumb_entry_t *entry = NULL;
    for (int i = 0; i < THUMB_CACHE_N; ++i) if (&s_cache[i].dsc == dsc) entry = &s_cache[i];
    if (!entry) return false;
    ++entry->refs;
    lv_obj_set_user_data(img, entry);
    lv_obj_add_event_cb(img, thumb_deleted, LV_EVENT_DELETE, NULL);
    lv_image_set_src(img, dsc);
    ui_thumb_apply_contain(img, w, h);
    lv_obj_center(img);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_HIDDEN);
    return true;
}
void ui_thumb_invalidate_all(void)
{
    for (int i = 0; i < THUMB_CACHE_N; ++i) {
        thumb_entry_t *e = &s_cache[i];
        e->stale = true;
        if (!e->refs && e->pixels) {
            lv_image_cache_drop(&e->dsc);
            heap_caps_free(e->pixels);
            memset(e, 0, sizeof(*e));
        }
    }
}

void ui_thumb_apply_contain(lv_obj_t *img, int32_t box_w, int32_t box_h)
{
    const lv_image_dsc_t *dsc = (const lv_image_dsc_t *)lv_image_get_src(img);
    if (!dsc || dsc->header.w == 0 || dsc->header.h == 0) return;
    /* 完整显示留边：scale = min(box_w/w, box_h/h) * 256 */
    uint32_t scale_w = (uint32_t)box_w * 256 / dsc->header.w;
    uint32_t scale_h = (uint32_t)box_h * 256 / dsc->header.h;
    lv_image_set_scale(img, (uint32_t)(scale_w < scale_h ? scale_w : scale_h));
}
