/**
 * @file ui_assets.c
 * @brief 资源表。生成目录由 tools/convert_assets.py 产出后此表才能链接。
 * 未启用功能（收藏/历史）的图标不编入（实施计划 4.2 白名单）。
 */

#include "ui_assets.h"

#include "assets.h"   /* 由 tools/convert_assets.py 生成到 generated/ */

#include "lvgl.h"
#include <string.h>
#include <stddef.h>

typedef struct {
    const char *name;
    const lv_image_dsc_t *dsc;
} asset_entry_t;

static const asset_entry_t k_table[] = {
    { UI_ASSET_FOX_READING, &img_fox_reading_scene },
    { UI_ASSET_FOX_EMPTY,   &img_fox_empty_library },
    { UI_ASSET_FOX_WIFI,    &img_fox_wifi },
    { UI_ASSET_FOX_ERROR,   &img_fox_error },
    { UI_ASSET_BOOT_SCENE,  &img_boot_fox_scene },
    { UI_ASSET_STAR_L,      &img_star_large },
    { UI_ASSET_STAR_S,      &img_star_small },
    { UI_ASSET_IC_BACK,     &img_icon_back },
    { UI_ASSET_IC_LIBRARY,  &img_icon_library },
    { UI_ASSET_IC_SETTINGS, &img_icon_settings },
    { UI_ASSET_IC_REGEN,    &img_icon_regenerate },
    { UI_ASSET_IC_DELETE,   &img_icon_delete },
    { UI_ASSET_IC_STEP_DONE, &img_icon_step_done },
    { UI_ASSET_IC_STEP_WAIT, &img_icon_step_pending },
    { UI_ASSET_IC_VOLUME,   &img_icon_volume },
    { UI_ASSET_IC_WIFI,     &img_icon_wifi },
};

const lv_image_dsc_t *ui_asset(const char *name)
{
    for (size_t i = 0; i < sizeof(k_table) / sizeof(k_table[0]); i++) {
        if (strcmp(k_table[i].name, name) == 0) return k_table[i].dsc;
    }
    return NULL;
}
