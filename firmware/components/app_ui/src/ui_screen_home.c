/**
 * @file ui_screen_home.c
 * @brief 首页（v0.2）：开机后进入，四个大入口——拍照、故事库、收藏、设置。
 *
 * "收藏"打开只列已收藏故事的故事库视图。
 * 入口插画为 Codex 生成的狐狸贴纸图（assets/v0.2/），背景为奶油天空。
 */

#include "ui_screens.h"
#include "ui_widgets.h"
#include "app_ui.h"
#include "assets.h"

#include "lvgl.h"
#include <stdio.h>

void on_library_open_all(lv_event_t *e);
void on_settings_open(lv_event_t *e);

static lv_obj_t *s_tile_library, *s_tile_fav;

static void on_favorites_open(lv_event_t *e)
{
    (void)e;
    screen_library_open(true);
}

static void on_camera_open(lv_event_t *e)
{
    (void)e;
    ui_navigator_go(SCREEN_CAMERA);
}

lv_obj_t *screen_home_create(void)
{
    lv_obj_t *root = lv_obj_create(ui_screens_parent());
    lv_obj_set_style_bg_color(root, lv_color_hex(UI_COL_BG), 0);

    lv_obj_t *bg = lv_image_create(root);
    lv_image_set_src(bg, &img_home_bg);
    lv_obj_set_pos(bg, 0, 0);

    lv_obj_t *star = lv_image_create(root);
    lv_image_set_src(star, &img_star_large);
    lv_image_set_scale(star, 150);
    lv_obj_set_pos(star, 8, 2);
    lv_obj_t *brand = lv_label_create(root);
    lv_label_set_text(brand, "小故事大世界");
    lv_obj_set_style_text_font(brand, ui_font_kid(24), 0);
    lv_obj_set_style_text_color(brand, lv_color_hex(UI_COL_TEXT), 0);
    lv_obj_set_pos(brand, 46, 12);

    lv_obj_t *pill = ui_status_pill(root, false);
    lv_obj_align(pill, LV_ALIGN_TOP_RIGHT, -14, 12);

    lv_obj_t *t;
    t = ui_tile(root, &img_tile_camera, "拍照", "拍下一页绘本", UI_COL_ORANGE, UI_COL_ORANGE_DK, on_camera_open, NULL);
    lv_obj_set_pos(t, 16, 52);
    s_tile_library = ui_tile(root, &img_tile_library, "故事库", "", UI_COL_SKY, UI_COL_SKY_DK, on_library_open_all, NULL);
    lv_obj_set_pos(s_tile_library, 248, 52);
    s_tile_fav = ui_tile(root, &img_tile_favorite, "收藏", "喜欢的故事", UI_COL_PINK, UI_COL_PINK_DK, on_favorites_open, NULL);
    lv_obj_set_pos(s_tile_fav, 16, 184);
    t = ui_tile(root, &img_tile_settings, "设置", "Wi-Fi · 音量", UI_COL_VIOLET, UI_COL_VIOLET_DK, on_settings_open, NULL);
    lv_obj_set_pos(t, 248, 184);
    return root;
}

void screen_home_on_enter(void)
{
    if (!s_tile_library) return;
    char sub[24];
    int n = app_ui_list_count();
    if (app_ui_scan_pending()) snprintf(sub, sizeof(sub), "正在查找…");
    else if (n == 0) snprintf(sub, sizeof(sub), "还没有故事");
    else snprintf(sub, sizeof(sub), "%d 个故事", n);
    ui_tile_set_sub(s_tile_library, sub);
    int f = app_ui_favorite_count();
    if (f > 0) snprintf(sub, sizeof(sub), "%d 个收藏", f);
    else snprintf(sub, sizeof(sub), "喜欢的故事");
    ui_tile_set_sub(s_tile_fav, sub);
}
