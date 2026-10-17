/**
 * @file ui_screens_settings_net.c
 * @brief 设置（画面8）、Wi-Fi（画面9）与密码（画面10）。
 *
 * 设置（v0.2）：每行彩色图标；年龄/故事长度为直接点选的分段按钮，音量滑条，
 * 改动即保存 NVS 并即时生效（下一次生成读取新值）；声音只读（P0 固定）；
 * Wi-Fi 行显示网络名；存储行显示真实剩余空间与比例条；最后一行为版本号。
 * Wi-Fi：on_enter 异步扫描（不阻塞 UI/事件循环），列表选中态 + 信号/锁，
 * "下一步"进入密码页；连接经 GOT_IP 确认后才保存凭据（9.2）。
 * 密码（7.1 修复）：真正的 lv_textarea（密码模式）+ lv_keyboard，
 * 显示/隐藏切换、取消返回；不再把 label 传给 textarea 接口。
 */

#include "ui_screens.h"
#include "ui_assets.h"
#include "ui_widgets.h"
#include "assets.h"
#include "app_ui.h"
#include "esp_app_desc.h"
#include "settings_service.h"
#include "network_service.h"
#include "board_adapter.h"
#include "audio_service.h"

#include "lvgl.h"
#include "esp_vfs_fat.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

void on_back_camera(lv_event_t *e);
void ui_dialog_show_error(const char *code, bool retryable);
void on_wifi_row_tap(lv_event_t *e);
void on_ssid_select(lv_event_t *e);
void on_wifi_next(lv_event_t *e);

/* ---------------- 设置（v0.2） ---------------- */

lv_obj_t *style_slider(lv_obj_t *slider);     /* ui_screens_generate_player.c */
void on_go_home(lv_event_t *e);

static lv_obj_t *s_wifi_val, *s_age_seg, *s_len_seg, *s_len_hint, *s_vol_slider_set, *s_vol_val;
static lv_obj_t *s_store_hint, *s_store_fill, *s_server_val;
static product_settings_t s_set;

static const char *len_hint(story_length_t l)
{
    return l == STORY_LENGTH_SHORT ? "约 1 分钟" : l == STORY_LENGTH_LONG ? "约 3 分钟" : "约 2 分钟";
}

/** 设置行：左侧彩色图标 + 名称（可带小字说明），返回行对象（右侧控件由调用方加） */
static lv_obj_t *add_row(lv_obj_t *list, const lv_image_dsc_t *icon, const char *name, lv_obj_t **hint)
{
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 56);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(row, 16, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(UI_COL_WHITE), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(UI_COL_BORDER), 0);
    lv_obj_set_style_border_width(row, 2, 0);
    lv_obj_set_style_pad_hor(row, 14, 0);
    if (icon) {
        lv_obj_t *ic = lv_image_create(row);
        lv_image_set_src(ic, icon);
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, 0, 0);
    }
    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, name);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, icon ? 40 : 0, hint ? -9 : 0);
    if (hint) {
        *hint = lv_label_create(row);
        lv_obj_set_style_text_color(*hint, lv_color_hex(UI_COL_TEXT_DIM), 0);
        lv_obj_align(*hint, LV_ALIGN_LEFT_MID, icon ? 40 : 0, 12);
    }
    return row;
}

static lv_obj_t *add_value(lv_obj_t *row, const char *text, bool chevron)
{
    /* 直接贴行右侧对齐（不用自适应宽度的容器，避免文字改变后左侧被裁） */
    if (chevron) {
        lv_obj_t *ic = ui_icon_create(row, UI_IC_NEXT, 20, UI_COL_TEXT_DIM);
        lv_obj_align(ic, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    lv_obj_t *v = lv_label_create(row);
    lv_obj_set_style_text_color(v, lv_color_hex(UI_COL_TEXT_DIM), 0);
    ui_label_fit(v, text, 200);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, chevron ? -24 : 0, 0);
    return v;
}

static void on_age_changed(lv_event_t *e)
{
    settings_load_product(&s_set);
    s_set.age_group = (age_group_t)(intptr_t)lv_event_get_param(e);
    settings_save_product(&s_set);                 /* 下一次生成使用新值 */
}

static void on_len_changed(lv_event_t *e)
{
    settings_load_product(&s_set);
    s_set.story_length = (story_length_t)(intptr_t)lv_event_get_param(e);
    lv_label_set_text(s_len_hint, len_hint(s_set.story_length));
    settings_save_product(&s_set);
}

static void on_vol_changed(lv_event_t *e)
{
    settings_load_product(&s_set);
    s_set.volume = (uint8_t)lv_slider_get_value(s_vol_slider_set);
    lv_label_set_text_fmt(s_vol_val, "%d", s_set.volume);
    audio_set_volume(s_set.volume);                 /* 即时生效 */
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) settings_save_product(&s_set);
}

static void refresh_storage(void)
{
    uint64_t total = 0, free_b = 0;
    if (esp_vfs_fat_info(board_adapter_sd_mount_point(), &total, &free_b) == ESP_OK && total) {
        lv_label_set_text_fmt(s_store_hint, "剩余 %lu.%lu GB / 共 %lu.%lu GB",
                              (unsigned long)(free_b >> 30), (unsigned long)((free_b >> 20) % 1024 * 10 / 1024),
                              (unsigned long)(total >> 30), (unsigned long)((total >> 20) % 1024 * 10 / 1024));
        lv_obj_set_width(s_store_fill, (int32_t)(140 * (total - free_b) / total) + 4);
    } else {
        lv_label_set_text(s_store_hint, "没有检测到存储卡");
        lv_obj_set_width(s_store_fill, 0);
    }
}

void screen_settings_on_enter(void)
{
    settings_load_product(&s_set);
    ui_seg_select(s_age_seg, s_set.age_group);
    ui_seg_select(s_len_seg, s_set.story_length);
    lv_label_set_text(s_len_hint, len_hint(s_set.story_length));
    lv_slider_set_value(s_vol_slider_set, s_set.volume, LV_ANIM_OFF);
    lv_label_set_text_fmt(s_vol_val, "%d", s_set.volume);
    wifi_credentials_t cred;
    settings_load_wifi(&cred);
    bool online = network_state() >= NET_STATE_GOT_IP;
    ui_label_fit(s_wifi_val, online && cred.valid ? cred.ssid : online ? "已连接" : "未连接", 200);
    memset(&cred, 0, sizeof(cred));               /* 不在栈上残留密码 */
    char url[96], host[64];
    int port;
    settings_get_server_url(url, sizeof(url));
    ui_server_split(url, host, sizeof(host), &port);
    snprintf(url, sizeof(url), "%s:%d", host, port);
    ui_label_fit(s_server_val, url, 230);
    refresh_storage();
}

static void on_server_row_tap(lv_event_t *e)
{
    (void)e;
    ui_navigator_go(SCREEN_SERVER);
}

lv_obj_t *screen_settings_create(void)
{
    lv_obj_t *root = lv_obj_create(ui_screens_parent());
    lv_obj_set_style_bg_color(root, lv_color_hex(UI_COL_BG), 0);
    settings_load_product(&s_set);
    ui_page_header(root, UI_IC_HOME, "设置", on_go_home);

    lv_obj_t *list = lv_obj_create(root);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, 480, 320 - 58);
    lv_obj_set_pos(list, 0, 58);
    lv_obj_set_style_pad_hor(list, 16, 0);
    lv_obj_set_style_pad_bottom(list, 16, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *row = add_row(list, &img_icon_wifi, "Wi-Fi", NULL);
    s_wifi_val = add_value(row, "未连接", true);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(row, lv_color_hex(UI_COL_BORDER), LV_STATE_PRESSED);
    lv_obj_add_event_cb(row, on_wifi_row_tap, LV_EVENT_CLICKED, NULL);
    ui_add_click_sound(row);

    static const char *const ages[] = { "3-4岁", "5-6岁", "7-10岁" };
    row = add_row(list, &img_icon_age, "孩子年龄", NULL);
    s_age_seg = ui_seg(row, ages, 3, s_set.age_group, on_age_changed);
    lv_obj_align(s_age_seg, LV_ALIGN_RIGHT_MID, 0, 0);

    static const char *const lens[] = { "短", "中", "长" };
    row = add_row(list, &img_icon_duration, "故事长度", &s_len_hint);
    s_len_seg = ui_seg(row, lens, 3, s_set.story_length, on_len_changed);
    lv_obj_align(s_len_seg, LV_ALIGN_RIGHT_MID, 0, 0);

    row = add_row(list, &img_icon_voice, "故事声音", NULL);
    add_value(row, "温柔姐姐", false);

    row = add_row(list, &img_icon_volume, "音量", NULL);
    s_vol_slider_set = style_slider(lv_slider_create(row));
    lv_obj_set_size(s_vol_slider_set, 160, 10);
    lv_obj_align(s_vol_slider_set, LV_ALIGN_RIGHT_MID, -44, 0);
    lv_slider_set_range(s_vol_slider_set, 0, 100);
    lv_obj_add_event_cb(s_vol_slider_set, on_vol_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_vol_slider_set, on_vol_changed, LV_EVENT_RELEASED, NULL);
    s_vol_val = lv_label_create(row);
    lv_obj_set_style_text_color(s_vol_val, lv_color_hex(UI_COL_TEXT_DIM), 0);
    lv_obj_align(s_vol_val, LV_ALIGN_RIGHT_MID, 0, 0);

    row = add_row(list, &img_icon_storage, "存储空间", &s_store_hint);
    lv_obj_t *track = lv_obj_create(row);
    lv_obj_remove_style_all(track);
    lv_obj_set_size(track, 144, 10);
    lv_obj_align(track, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_radius(track, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(track, lv_color_hex(UI_COL_BORDER), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_clip_corner(track, true, 0);
    s_store_fill = lv_obj_create(track);
    lv_obj_remove_style_all(s_store_fill);
    lv_obj_set_size(s_store_fill, 0, 10);
    lv_obj_set_style_radius(s_store_fill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_store_fill, lv_color_hex(UI_COL_GREEN), 0);
    lv_obj_set_style_bg_opa(s_store_fill, LV_OPA_COVER, 0);

    row = add_row(list, &img_icon_wifi, "服务器", NULL);
    s_server_val = add_value(row, "", true);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(row, lv_color_hex(UI_COL_BORDER), LV_STATE_PRESSED);
    lv_obj_add_event_cb(row, on_server_row_tap, LV_EVENT_CLICKED, NULL);
    ui_add_click_sound(row);

    row = add_row(list, NULL, "关于", NULL);
    char ver[48];
    snprintf(ver, sizeof(ver), "版本 %s", esp_app_get_description()->version);
    add_value(row, ver, false);

    screen_settings_on_enter();
    return root;
}

void on_wifi_row_tap(lv_event_t *e)
{
    (void)e;
    ui_navigator_go(SCREEN_WIFI);
}

/* ---------------- Wi-Fi ---------------- */

static lv_obj_t *s_ssid_list;
static lv_obj_t *s_scan_hint;
static char s_selected_ssid[33];
static uint8_t s_selected_len;
static int s_selected_index = -1;
static wifi_scan_item_t s_scan_items[16];
static int s_scan_count;
static lv_obj_t *s_wifi_next;
static uint32_t s_scan_epoch;

static void scan_result_cb(const wifi_scan_item_t *items, int count, void *user)
{
    uint32_t epoch = (uint32_t)(uintptr_t)user;
    /* 网络任务上下文 → 转投 LVGL 线程渲染 */
    if (!board_adapter_lvgl_lock(1000)) return;
    if (epoch != s_scan_epoch || ui_navigator_current() != SCREEN_WIFI) {
        board_adapter_lvgl_unlock(); return;
    }
    s_scan_count = count > 16 ? 16 : count;
    if (s_scan_count > 0) memcpy(s_scan_items, items, sizeof(*items) * s_scan_count);
    lv_obj_add_state(s_wifi_next, LV_STATE_DISABLED);
    if (s_scan_hint) lv_obj_add_flag(s_scan_hint, LV_OBJ_FLAG_HIDDEN);
    if (s_ssid_list) {
        lv_obj_clean(s_ssid_list);
        s_selected_index = -1;
        if (count == 0 && s_scan_hint) {
            lv_label_set_text(s_scan_hint, "未找到网络，请返回重试");
            lv_obj_remove_flag(s_scan_hint, LV_OBJ_FLAG_HIDDEN);
        }
        wifi_credentials_t connected_cred;
        settings_load_wifi(&connected_cred);
        bool connected_ssid = connected_cred.valid && network_state() >= NET_STATE_GOT_IP;
        for (int i = 0; i < s_scan_count; i++) {
            lv_obj_t *row = lv_obj_create(s_ssid_list);
            lv_obj_remove_style_all(row);
            lv_obj_set_size(row, LV_PCT(100), 48);
            lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_radius(row, 14, 0);
            lv_obj_set_style_bg_color(row, lv_color_hex(UI_COL_WHITE), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(row, lv_color_hex(UI_COL_BORDER), 0);
            lv_obj_set_style_border_width(row, 2, 0);
            lv_obj_set_style_bg_color(row, lv_color_hex(UI_COL_BORDER), LV_STATE_PRESSED);
            ui_add_click_sound(row);
            bool connected = connected_ssid && items[i].ssid_len == connected_cred.ssid_len &&
                             !memcmp(items[i].ssid, connected_cred.ssid, items[i].ssid_len);
            lv_obj_t *ic = ui_icon_create(row, UI_IC_WIFI, 20, connected ? UI_COL_GREEN : UI_COL_SKY_DK);
            lv_obj_align(ic, LV_ALIGN_LEFT_MID, 12, 0);
            lv_obj_t *lbl = lv_label_create(row);
            /* SSID 原始字节直接显示，缺字由字体回退；不用占位串冒充 SSID */
            ui_label_fit(lbl, items[i].ssid, 150);
            lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 40, 0);
            lv_obj_t *info = lv_label_create(row);
            /* 家长看得懂的信号档位；已连接/开放网络单独标注 */
            const char *level = items[i].rssi >= -60 ? "信号强" : items[i].rssi >= -75 ? "信号中" : "信号弱";
            if (connected) lv_label_set_text(info, "已连接");
            else lv_label_set_text_fmt(info, "%s%s", items[i].auth_required ? "" : "开放 · ", level);
            lv_obj_set_style_text_color(info, lv_color_hex(connected ? UI_COL_GREEN : UI_COL_TEXT_DIM), 0);
            lv_obj_align(info, LV_ALIGN_RIGHT_MID, -12, 0);
            lv_obj_add_event_cb(row, on_ssid_select, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }
        memset(&connected_cred, 0, sizeof(connected_cred));   /* 不在栈上残留密码 */
    }
    board_adapter_lvgl_unlock();
}

void on_ssid_select(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_scan_count) return;
    s_selected_index = idx;
    memcpy(s_selected_ssid, s_scan_items[idx].ssid, sizeof(s_selected_ssid));
    s_selected_len = s_scan_items[idx].ssid_len;
    lv_obj_remove_state(s_wifi_next, LV_STATE_DISABLED);
    /* 高亮选中 */
    uint32_t n = lv_obj_get_child_cnt(s_ssid_list);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_set_style_border_width(lv_obj_get_child(s_ssid_list, i), 2, 0);
        lv_obj_set_style_border_color(lv_obj_get_child(s_ssid_list, i),
                                      lv_color_hex(UI_COL_BORDER), 0);
    }
    lv_obj_set_style_border_width(row, 3, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(UI_COL_ORANGE), 0);
}

void screen_wifi_on_enter(void);
void screen_wifi_on_exit(void);

lv_obj_t *screen_wifi_create(void)
{
    lv_obj_t *root = lv_obj_create(ui_screens_parent());
    lv_obj_set_style_bg_color(root, lv_color_hex(UI_COL_BG), 0);
    ui_page_header(root, UI_IC_BACK, "连接 Wi-Fi", on_back_camera);

    /* 左侧扫描列表（异步填充，可上下滚动） */
    s_ssid_list = lv_obj_create(root);
    lv_obj_remove_style_all(s_ssid_list);
    lv_obj_set_size(s_ssid_list, 296, 320 - 58);
    lv_obj_set_pos(s_ssid_list, 12, 58);
    lv_obj_set_style_pad_bottom(s_ssid_list, 12, 0);
    lv_obj_set_flex_flow(s_ssid_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_ssid_list, 8, 0);
    lv_obj_set_scroll_dir(s_ssid_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_ssid_list, LV_SCROLLBAR_MODE_OFF);

    s_scan_hint = lv_label_create(root);
    lv_label_set_text(s_scan_hint, "正在扫描附近网络…");
    lv_obj_set_style_text_color(s_scan_hint, lv_color_hex(UI_COL_TEXT_DIM), 0);
    lv_obj_align(s_scan_hint, LV_ALIGN_TOP_LEFT, 60, 150);

    /* 右侧：气泡提示 + 狐狸（底边贴按钮上沿）+ 下一步 */
    lv_obj_t *tip = lv_label_create(root);
    lv_label_set_text(tip, "选好网络，\n再点下一步");
    lv_obj_set_style_text_color(tip, lv_color_hex(UI_COL_TEXT_DIM), 0);
    lv_obj_set_style_text_align(tip, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(tip, LV_ALIGN_TOP_RIGHT, -40, 92);
    const lv_image_dsc_t *fox = ui_asset(UI_ASSET_FOX_WIFI);
    if (fox) {
        lv_obj_t *img = lv_image_create(root);
        lv_image_set_src(img, fox);
        lv_obj_align(img, LV_ALIGN_BOTTOM_RIGHT, -14, -78);
    }
    s_wifi_next = ui_chunky_btn(root, UI_IC_NEXT, "下一步", UI_COL_SKY, UI_COL_SKY_DK, UI_COL_WHITE,
                                148, 52, on_wifi_next, NULL);
    lv_obj_align(s_wifi_next, LV_ALIGN_BOTTOM_RIGHT, -12, -20);
    return root;
}

void screen_wifi_on_enter(void)
{
    /* 进入页面触发异步扫描（FLOW-07：不在事件回调/LVGL 线程同步扫描） */
    s_selected_ssid[0] = '\0';
    s_selected_len = 0;
    s_selected_index = -1;
    s_scan_count = 0;
    lv_obj_clean(s_ssid_list);
    lv_obj_add_state(s_wifi_next, LV_STATE_DISABLED);
    ++s_scan_epoch;
    if (s_scan_hint) {
        lv_label_set_text(s_scan_hint, "正在扫描附近网络…");
        lv_obj_remove_flag(s_scan_hint, LV_OBJ_FLAG_HIDDEN);
    }
    if (network_scan_async(scan_result_cb, (void *)(uintptr_t)s_scan_epoch) != ESP_OK && s_scan_hint) {
        lv_label_set_text(s_scan_hint, "网络服务未就绪");
    }
}

void screen_wifi_on_exit(void)
{
    ++s_scan_epoch;
}

void on_wifi_next(lv_event_t *e)
{
    (void)e;
    if (s_selected_index < 0 || !s_ssid_list) {
        ui_dialog_show_error("NETWORK_TIMEOUT", false);   /* 提示先选择网络 */
        return;
    }
    ui_navigator_go(SCREEN_PASSWORD);
}

/* ---------------- Password: immutable request credentials, UI result queue ---------------- */
static lv_obj_t *s_pwd_textarea, *s_pwd_title, *s_pwd_toggle, *s_connect_mask, *s_pwd_kbd;
static QueueHandle_t s_connect_results;
static uint32_t s_pwd_epoch;
static bool s_connecting;
typedef struct {
    uint32_t epoch;
    wifi_credentials_t cred;
    net_connect_result_t result;
} password_request_t;

static void clear_connect_mask(void)
{
    if (s_connect_mask) { lv_obj_delete(s_connect_mask); s_connect_mask = NULL; }
    s_connecting = false;
}
static void on_pwd_show_toggle(lv_event_t *e)
{
    bool hidden = lv_textarea_get_password_mode(s_pwd_textarea);
    lv_textarea_set_password_mode(s_pwd_textarea, !hidden);
    lv_label_set_text(lv_obj_get_child(lv_event_get_target(e), 0),
                      hidden ? LV_SYMBOL_EYE_OPEN : LV_SYMBOL_EYE_CLOSE);
}
static void pwd_connect_result_cb(net_connect_result_t result, void *user)
{
    password_request_t *request = user;
    request->result = result;
    /* No LVGL access on the network task. The timer owns mask/navigation. */
    if (xQueueSend(s_connect_results, &request, pdMS_TO_TICKS(100)) != pdTRUE) {
        memset(request, 0, sizeof(*request)); free(request);
    }
}
static void poll_connect_result(lv_timer_t *timer)
{
    (void)timer;
    password_request_t *request;
    while (xQueueReceive(s_connect_results, &request, 0) == pdTRUE) {
        if (request->epoch == s_pwd_epoch && ui_navigator_current() == SCREEN_PASSWORD) {
            clear_connect_mask();
            if (request->result == NET_CONNECT_OK && settings_save_wifi(&request->cred) == ESP_OK)
                ui_navigator_go(SCREEN_SETTINGS);
            else ui_dialog_show_error("NETWORK_TIMEOUT", false);
        }
        memset(request, 0, sizeof(*request)); free(request);
    }
}
static void on_pwd_cancel(lv_event_t *e)
{
    (void)e;
    if (s_connecting) network_disconnect();
    ui_navigator_back();
}
static void on_pwd_done(lv_event_t *e)
{
    (void)e;
    if (s_connecting || !s_selected_len) return;
    const char *pwd = lv_textarea_get_text(s_pwd_textarea);
    bool auth = s_selected_index >= 0 && s_scan_items[s_selected_index].auth_required;
    size_t len = strlen(pwd);
    if (auth && (len < 8 || len > 64)) { ui_dialog_show_error("WIFI_PASSWORD_INVALID", false); return; }
    password_request_t *request = calloc(1, sizeof(*request));
    if (!request) { ui_dialog_show_error("NETWORK_TIMEOUT", false); return; }
    request->epoch = s_pwd_epoch;
    memcpy(request->cred.ssid, s_selected_ssid, sizeof(request->cred.ssid));
    request->cred.ssid_len = s_selected_len;
    strlcpy(request->cred.password, auth ? pwd : "", sizeof(request->cred.password));
    request->cred.valid = true;
    s_connect_mask = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_connect_mask);
    lv_obj_set_size(s_connect_mask, 480, 320);
    lv_obj_add_flag(s_connect_mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(s_connect_mask, lv_color_hex(UI_COL_SMOKE), 0);
    lv_obj_set_style_bg_opa(s_connect_mask, 120, 0);
    lv_obj_t *card = lv_obj_create(s_connect_mask);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 280, 180);
    lv_obj_center(card);
    lv_obj_set_style_radius(card, 26, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(UI_COL_BG), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, 12, 0);
    lv_obj_t *sp = lv_spinner_create(card);
    lv_obj_set_size(sp, 36, 36);
    lv_obj_set_style_arc_width(sp, 5, 0);
    lv_obj_set_style_arc_width(sp, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(sp, lv_color_hex(UI_COL_BORDER), 0);
    lv_obj_set_style_arc_color(sp, lv_color_hex(UI_COL_SKY), LV_PART_INDICATOR);
    lv_obj_t *label = lv_label_create(card);
    lv_label_set_text(label, "正在连接…");
    lv_obj_set_style_text_font(label, ui_font_kid(24), 0);
    ui_chunky_btn(card, UI_IC_NONE, "取消", UI_COL_WHITE, UI_COL_BORDER, UI_COL_TEXT, 112, 42, on_pwd_cancel, NULL);
    s_connecting = true;
    if (network_connect_async(request->cred.ssid, request->cred.ssid_len, request->cred.password,
                              pwd_connect_result_cb, request) != ESP_OK) {
        clear_connect_mask();
        memset(request, 0, sizeof(*request)); free(request);
        ui_dialog_show_error("NETWORK_TIMEOUT", false);
    }
}
void screen_password_on_enter(void)
{
    ++s_pwd_epoch;
    clear_connect_mask();
    lv_label_set_text_fmt(s_pwd_title, "连接：%s", s_selected_ssid);
    lv_textarea_set_text(s_pwd_textarea, "");
    lv_textarea_set_password_mode(s_pwd_textarea, true);
    lv_label_set_text(lv_obj_get_child(s_pwd_toggle, 0), LV_SYMBOL_EYE_CLOSE);
    if (s_pwd_kbd) lv_keyboard_set_mode(s_pwd_kbd, LV_KEYBOARD_MODE_TEXT_LOWER);  /* 不沿用上次的数字/大写页 */
}
void screen_password_on_exit(void)
{
    ++s_pwd_epoch;
    if (s_connecting) network_disconnect();
    clear_connect_mask();
    lv_textarea_set_text(s_pwd_textarea, "");
}
lv_obj_t *screen_password_create(void)
{
    lv_obj_t *root = lv_obj_create(ui_screens_parent());
    lv_obj_set_style_bg_color(root, lv_color_hex(UI_COL_BG), 0);
    lv_obj_t *back = ui_round_btn(root, UI_IC_BACK, 40, UI_ROUND_OUTLINE, on_pwd_cancel, NULL);
    lv_obj_set_pos(back, 12, 6);
    s_pwd_title = lv_label_create(root);
    lv_obj_set_pos(s_pwd_title, 62, 14); lv_obj_set_width(s_pwd_title, 290);
    lv_label_set_long_mode(s_pwd_title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(s_pwd_title, ui_font_cn(22), 0);
    lv_obj_t *done = ui_chunky_btn(root, UI_IC_NONE, "连接", UI_COL_SKY, UI_COL_SKY_DK, UI_COL_WHITE,
                                   104, 42, on_pwd_done, NULL);
    lv_obj_set_pos(done, 364, 6);
    s_pwd_textarea = lv_textarea_create(root);
    lv_obj_set_size(s_pwd_textarea, 382, 46); lv_obj_set_pos(s_pwd_textarea, 12, 54);
    lv_textarea_set_one_line(s_pwd_textarea, true);
    lv_textarea_set_password_mode(s_pwd_textarea, true);
    lv_textarea_set_max_length(s_pwd_textarea, 64);
    lv_textarea_set_placeholder_text(s_pwd_textarea, "输入 Wi-Fi 密码");
    lv_obj_set_style_radius(s_pwd_textarea, 14, 0);
    lv_obj_set_style_bg_color(s_pwd_textarea, lv_color_hex(UI_COL_WHITE), 0);
    lv_obj_set_style_border_color(s_pwd_textarea, lv_color_hex(UI_COL_BORDER), 0);
    lv_obj_set_style_border_color(s_pwd_textarea, lv_color_hex(UI_COL_ORANGE), LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(s_pwd_textarea, 2, 0);
    lv_obj_set_style_pad_hor(s_pwd_textarea, 14, 0);
    lv_obj_set_style_pad_ver(s_pwd_textarea, 9, 0);
    lv_obj_set_style_text_font(s_pwd_textarea, ui_font_cn(22), 0);
    s_pwd_toggle = lv_obj_create(root);
    lv_obj_remove_style_all(s_pwd_toggle);
    lv_obj_set_size(s_pwd_toggle, 62, 46); lv_obj_set_pos(s_pwd_toggle, 404, 54);
    lv_obj_add_flag(s_pwd_toggle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(s_pwd_toggle, 14, 0);
    lv_obj_set_style_bg_color(s_pwd_toggle, lv_color_hex(UI_COL_WHITE), 0);
    lv_obj_set_style_bg_color(s_pwd_toggle, lv_color_hex(UI_COL_BORDER), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(s_pwd_toggle, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_pwd_toggle, lv_color_hex(UI_COL_BORDER), 0);
    lv_obj_set_style_border_width(s_pwd_toggle, 2, 0);
    lv_obj_t *toggle_label = lv_label_create(s_pwd_toggle);
    lv_label_set_text(toggle_label, LV_SYMBOL_EYE_CLOSE); lv_obj_center(toggle_label);
    lv_obj_add_event_cb(s_pwd_toggle, on_pwd_show_toggle, LV_EVENT_CLICKED, NULL);
    lv_obj_t *kbd = lv_keyboard_create(root);
    s_pwd_kbd = kbd;
    /* lv_keyboard 默认 BOTTOM_MID 对齐，set_pos 会变成相对偏移把下两排键推出屏外；
     * 显式贴底对齐：480x216 占 y=104..320 */
    lv_obj_set_size(kbd, 480, 216);
    lv_obj_align(kbd, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kbd, s_pwd_textarea);
    /* 键盘：奶油底、白色圆角键（底边加厚）、按下变橙 */
    lv_obj_set_style_bg_color(kbd, lv_color_hex(0xF3E9D8), 0);
    lv_obj_set_style_pad_all(kbd, 5, 0);
    lv_obj_set_style_pad_gap(kbd, 5, 0);
    lv_obj_set_style_radius(kbd, 0, 0);
    lv_obj_set_style_border_width(kbd, 0, 0);
    lv_obj_set_style_bg_color(kbd, lv_color_hex(UI_COL_WHITE), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(kbd, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_radius(kbd, 10, LV_PART_ITEMS);
    lv_obj_set_style_border_side(kbd, LV_BORDER_SIDE_BOTTOM, LV_PART_ITEMS);
    lv_obj_set_style_border_color(kbd, lv_color_hex(UI_COL_BORDER), LV_PART_ITEMS);
    lv_obj_set_style_border_width(kbd, 3, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(kbd, 0, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kbd, lv_color_hex(UI_COL_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_text_font(kbd, ui_font_cn(22), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kbd, lv_color_hex(UI_COL_ORANGE), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(kbd, lv_color_hex(UI_COL_WHITE), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(kbd, lv_color_hex(UI_COL_BORDER), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(kbd, lv_color_hex(UI_COL_TEXT), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_add_event_cb(kbd, on_pwd_done, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kbd, on_pwd_cancel, LV_EVENT_CANCEL, NULL);
    s_connect_results = xQueueCreate(4, sizeof(password_request_t *));
    lv_timer_create(poll_connect_result, 50, NULL);
    return root;
}
