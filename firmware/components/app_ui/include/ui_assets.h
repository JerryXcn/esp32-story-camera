#pragma once
/**
 * @file ui_assets.h
 * @brief 运行时资源白名单（实施计划 4.2）。只暴露 P0 必需图；
 *        reference/、review/、演示封面不进入固件资源包。
 *
 * 符号由 tools/convert_assets.py 在锁定 LVGL 版本后生成
 * （main/components/app_ui/generated/），输入 hash 与色彩格式记录在
 * generated/ASSETS.md。生成目录不存在时构建失败，并提示先运行转换脚本
 * ——不允许直接把 PNG 塞进固件假定可解码。
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

const lv_image_dsc_t *ui_asset(const char *name);

/* 缩略图解码缓存（ui_thumbs.c）：thumb.jpg -> RGB565 lv_image 描述符。
 * 失败返回 NULL（UI 显示占位）；ui_thumb_invalidate_all 在删除/刷新后调用。 */
const lv_image_dsc_t *ui_thumb_get(const char *path);
void ui_thumb_invalidate_all(void);
/** Pins pixels until rebind or image deletion; always use this for displayed thumbnails. */
bool ui_thumb_bind(lv_obj_t *img, const char *path, int32_t box_w, int32_t box_h);

/** 按 box 尺寸等比 contain 缩放（LVGL9.2 无 CONTAIN，用 scale 实现） */
void ui_thumb_apply_contain(lv_obj_t *img, int32_t box_w, int32_t box_h);

/* 全屏照片（ui_photo.c）：读取与缩略图同目录的 original.jpg，硬件解码后用 PPA
 * 按 3:2 铺满裁切成 480x320 RGB565；blur 版为 60x40，放大 8 倍显示即模糊背景。
 * 各缓存一张（按路径），在 LVGL 线程调用（同步解码约几十毫秒）。失败返回 NULL。 */
const lv_image_dsc_t *ui_photo_full(const char *thumb_path);
const lv_image_dsc_t *ui_photo_blur(const char *thumb_path);
void *ui_jpeg_decoder(void);

/* 名称常量（与 assets/ 下文件一致） */
#define UI_ASSET_FOX_READING   "fox_reading_scene"    /* 232x208 不透明 */
#define UI_ASSET_FOX_EMPTY     "fox_empty_library"    /* 168x116 透明 */
#define UI_ASSET_FOX_WIFI      "fox_wifi"             /* 136x68 透明，底边贴面板 */
#define UI_ASSET_FOX_ERROR     "fox_error"            /* 96x100 透明，底边平切 */
#define UI_ASSET_BOOT_SCENE    "boot_fox_scene"       /* 232x304 左侧矩形场景 */
#define UI_ASSET_STAR_L        "star_large"
#define UI_ASSET_STAR_S        "star_small"
#define UI_ASSET_IC_BACK       "icon_back"
#define UI_ASSET_IC_LIBRARY    "icon_library"
#define UI_ASSET_IC_SETTINGS   "icon_settings"
#define UI_ASSET_IC_REGEN      "icon_regenerate"
#define UI_ASSET_IC_DELETE     "icon_delete"
#define UI_ASSET_IC_STEP_DONE  "icon_step_done"
#define UI_ASSET_IC_STEP_WAIT  "icon_step_pending"
#define UI_ASSET_IC_VOLUME     "icon_volume"
#define UI_ASSET_IC_WIFI       "icon_wifi"

#ifdef __cplusplus
}
#endif
