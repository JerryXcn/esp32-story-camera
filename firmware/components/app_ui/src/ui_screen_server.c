/**
 * @file ui_screen_server.c
 * @brief 服务器地址（设置 → 服务器）：主机（域名或 IP）与端口，测试连通、保存即生效。
 *
 * 地址保存为 https://<主机>:<端口>（settings_set_server_url），api_client 立即切换。
 * TLS 统一走证书包（公共根证书 + 开发 CA），域名后端用公共证书、局域网 IP 后端用开发证书都能校验；
 * 开发证书须包含所填的 IP/域名（server/scripts/gen_dev_certs.sh --keep-ca <主机...>）。
 * 测试连通在后台任务执行（最多约 10 秒），结果回到 LVGL 线程弹窗。
 */

#include "ui_screens.h"
#include "ui_widgets.h"
#include "app_ui.h"
#include "api_client.h"
#include "settings_service.h"
#include "board_adapter.h"

#include "esp_tls_errors.h"
#include "lvgl.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static lv_obj_t *s_host_ta, *s_port_ta, *s_kbd, *s_test_btn, *s_testing_mask;
static volatile bool s_testing;

/** https://host[:port][/] → host, port（缺省 443） */
void ui_server_split(const char *url, char *host, size_t host_len, int *port)
{
    const char *p = strncmp(url, "https://", 8) == 0 ? url + 8 : url;
    const char *colon = strrchr(p, ':');
    const char *slash = strchr(p, '/');
    size_t n = colon ? (size_t)(colon - p) : slash ? (size_t)(slash - p) : strlen(p);
    if (n >= host_len) n = host_len - 1;
    memcpy(host, p, n);
    host[n] = '\0';
    *port = colon ? atoi(colon + 1) : 443;
}

/** 主机：域名或 IPv4，只允许字母、数字、点、连字符；不能以点或连字符开头结尾 */
static bool host_valid(const char *h)
{
    size_t n = strlen(h);
    if (n == 0 || n > 63 || h[0] == '.' || h[0] == '-' || h[n - 1] == '.' || h[n - 1] == '-') return false;
    for (size_t i = 0; i < n; i++) {
        if (!isalnum((unsigned char)h[i]) && h[i] != '.' && h[i] != '-') return false;
    }
    return true;
}

static bool read_fields(char *url, size_t len)
{
    const char *host = lv_textarea_get_text(s_host_ta);
    const char *port_s = lv_textarea_get_text(s_port_ta);
    int port = atoi(port_s);
    if (!host_valid(host) || port < 1 || port > 65535) {
        ui_dialog_spec_t spec = {
            .title = "地址不对",
            .body = "地址只能有字母、数字、点和横线（例如 192.168.1.10 或 story.example.com），端口是 1 到 65535 的数字。",
        };
        ui_dialog(&spec);
        return false;
    }
    snprintf(url, len, "https://%s:%d", host, port);
    return true;
}

/* ---- 测试连通（后台任务 → LVGL 线程） ---- */

static const char *probe_reason(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME:
        return "找不到这个域名，检查拼写或网络";
    case ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED:
    case ESP_ERR_MBEDTLS_X509_CRT_PARSE_FAILED:
        return "证书校验没通过，服务器证书里要包含这个地址";
    case ESP_ERR_INVALID_RESPONSE:
        return "连上了，但它不是故事服务器";
    default:
        return "服务器没有回应，检查地址、端口，以及服务器是否已开启";
    }
}

typedef struct { char url[96]; esp_err_t result; } probe_job_t;

static void show_probe_result(void *arg)
{
    probe_job_t *job = arg;
    s_testing = false;
    if (s_testing_mask) { lv_obj_delete(s_testing_mask); s_testing_mask = NULL; }
    if (ui_navigator_current() == SCREEN_SERVER) {
        static char body[320];
        ui_dialog_spec_t spec = { .body = body };
        if (job->result == ESP_OK) {
            spec.title = "连上啦";
            snprintf(body, sizeof(body), "%s 可以正常使用。记得点\"保存\"。", job->url);
            spec.right_face = UI_COL_GREEN;
            spec.right_edge = UI_COL_GREEN_DK;
        } else {
            spec.title = job->result == ESP_ERR_INVALID_RESPONSE ? "地址不对" : "连不上";
            snprintf(body, sizeof(body), "%s\n%s。", job->url, probe_reason(job->result));
        }
        ui_dialog(&spec);
    }
    free(job);
}

static void probe_task(void *arg)
{
    probe_job_t *job = arg;
    job->result = api_client_probe(job->url);
    if (board_adapter_lvgl_lock(2000)) {
        show_probe_result(job);
        board_adapter_lvgl_unlock();
    } else {
        s_testing = false;
        free(job);
    }
    vTaskDelete(NULL);
}

static void on_test(lv_event_t *e)
{
    (void)e;
    if (s_testing) return;
    probe_job_t *job = calloc(1, sizeof(*job));
    if (!job || !read_fields(job->url, sizeof(job->url))) { free(job); return; }
    s_testing = true;
    s_testing_mask = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_testing_mask);
    lv_obj_set_size(s_testing_mask, 480, 320);
    lv_obj_add_flag(s_testing_mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(s_testing_mask, lv_color_hex(UI_COL_SMOKE), 0);
    lv_obj_set_style_bg_opa(s_testing_mask, 120, 0);
    lv_obj_t *card = lv_obj_create(s_testing_mask);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 260, 140);
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
    lv_obj_t *l = lv_label_create(card);
    lv_label_set_text(l, "正在测试连接…");
    if (xTaskCreate(probe_task, "srv_probe", 6144, job, 4, NULL) != pdPASS) {
        free(job);
        s_testing = false;
        lv_obj_delete(s_testing_mask);
        s_testing_mask = NULL;
    }
}

/* ---- 保存 ---- */

static void on_save(lv_event_t *e)
{
    (void)e;
    char url[96];
    if (!read_fields(url, sizeof(url))) return;
    esp_err_t err = settings_set_server_url(url);
    if (err == ESP_OK) api_client_init(url);
    ui_dialog_spec_t spec = {
        .title = err == ESP_OK ? "已保存" : "保存失败",
        .body = err == ESP_OK ? "之后生成故事都会连这个服务器。" : "请再试一次。",
    };
    ui_dialog(&spec);
}

static void on_reset_default(lv_event_t *e)
{
    (void)e;
    settings_set_server_url("");
    screen_server_on_enter();
}

static void on_back(lv_event_t *e)
{
    (void)e;
    ui_navigator_go(SCREEN_SETTINGS);
}

/* 主机键盘：数字与字母同屏，带 . 和 -（填 IP 或域名都不用切换布局）；端口键盘只有数字 */
#define KEY_FLAGS (LV_BUTTONMATRIX_CTRL_NO_REPEAT | LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_CHECKED)
static const char *const s_host_map[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", "-", "\n",
    "z", "x", "c", "v", "b", "n", "m", ".", LV_SYMBOL_OK, "",
};
static const lv_buttonmatrix_ctrl_t s_host_ctrl[] = {
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, KEY_FLAGS | 6,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, KEY_FLAGS | 8,
};
static const char *const s_port_map[] = {
    "1", "2", "3", "4", "5", "\n",
    "6", "7", "8", "9", "0", "\n",
    LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT, LV_SYMBOL_BACKSPACE, LV_SYMBOL_OK, "",
};
static const lv_buttonmatrix_ctrl_t s_port_ctrl[] = {
    1, 1, 1, 1, 1,
    1, 1, 1, 1, 1,
    KEY_FLAGS | 1, KEY_FLAGS | 1, KEY_FLAGS | 1, KEY_FLAGS | 2,
};

static void focus_field(lv_obj_t *ta)
{
    lv_obj_t *other = ta == s_port_ta ? s_host_ta : s_port_ta;
    lv_obj_remove_state(other, LV_STATE_FOCUSED);
    lv_obj_add_state(ta, LV_STATE_FOCUSED);
    lv_keyboard_set_textarea(s_kbd, ta);
    lv_keyboard_set_mode(s_kbd, ta == s_port_ta ? LV_KEYBOARD_MODE_USER_2 : LV_KEYBOARD_MODE_USER_1);
}

static void on_ta_focus(lv_event_t *e)
{
    focus_field(lv_event_get_target(e));
}

/* 键盘上的 ✓：主机 → 跳到端口；端口 → 收起焦点（再点"测试"或"保存"） */
static void on_ta_ready(lv_event_t *e)
{
    if (lv_event_get_target(e) == s_host_ta) focus_field(s_port_ta);
}

static lv_obj_t *make_ta(lv_obj_t *root, int x, int w, int max_len, const char *placeholder)
{
    lv_obj_t *ta = lv_textarea_create(root);
    lv_obj_set_size(ta, w, 46);
    lv_obj_set_pos(ta, x, 54);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, max_len);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_obj_set_style_radius(ta, 14, 0);
    lv_obj_set_style_bg_color(ta, lv_color_hex(UI_COL_WHITE), 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(UI_COL_BORDER), 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(UI_COL_ORANGE), LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(ta, 2, 0);
    lv_obj_set_style_pad_hor(ta, 12, 0);
    lv_obj_set_style_pad_ver(ta, 10, 0);
    lv_obj_add_event_cb(ta, on_ta_focus, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(ta, on_ta_focus, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ta, on_ta_ready, LV_EVENT_READY, NULL);
    return ta;
}

lv_obj_t *screen_server_create(void)
{
    lv_obj_t *root = lv_obj_create(ui_screens_parent());
    lv_obj_set_style_bg_color(root, lv_color_hex(UI_COL_BG), 0);
    lv_obj_t *back = ui_round_btn(root, UI_IC_BACK, 40, UI_ROUND_OUTLINE, on_back, NULL);
    lv_obj_set_pos(back, 12, 6);
    lv_obj_t *title = lv_label_create(root);
    lv_label_set_text(title, "服务器");
    lv_obj_set_style_text_font(title, ui_font_kid(24), 0);
    lv_obj_set_pos(title, 64, 12);
    lv_obj_t *reset = ui_chunky_btn(root, UI_IC_REFRESH, "默认", UI_COL_WHITE, UI_COL_BORDER, UI_COL_TEXT,
                                    92, 42, on_reset_default, NULL);
    lv_obj_set_pos(reset, 160, 6);
    s_test_btn = ui_chunky_btn(root, UI_IC_WIFI, "测试", UI_COL_SKY, UI_COL_SKY_DK, UI_COL_WHITE,
                               100, 42, on_test, NULL);
    lv_obj_set_pos(s_test_btn, 260, 6);
    lv_obj_t *save = ui_chunky_btn(root, UI_IC_CHECK, "保存", UI_COL_ORANGE, UI_COL_ORANGE_DK, UI_COL_WHITE,
                                   100, 42, on_save, NULL);
    lv_obj_set_pos(save, 368, 6);

    s_host_ta = make_ta(root, 12, 330, 63, "域名或 IP");
    lv_obj_t *colon = lv_label_create(root);
    lv_label_set_text(colon, ":");
    lv_obj_set_style_text_font(colon, ui_font_cn(22), 0);
    lv_obj_set_pos(colon, 348, 64);
    s_port_ta = make_ta(root, 362, 106, 5, "端口");
    lv_textarea_set_accepted_chars(s_port_ta, "0123456789");

    s_kbd = lv_keyboard_create(root);
    lv_obj_set_size(s_kbd, 480, 212);
    lv_obj_align(s_kbd, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_kbd, lv_color_hex(0xF3E9D8), 0);
    lv_obj_set_style_pad_all(s_kbd, 5, 0);
    lv_obj_set_style_pad_gap(s_kbd, 5, 0);
    lv_obj_set_style_radius(s_kbd, 0, 0);
    lv_obj_set_style_border_width(s_kbd, 0, 0);
    lv_obj_set_style_bg_color(s_kbd, lv_color_hex(UI_COL_WHITE), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(s_kbd, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_radius(s_kbd, 10, LV_PART_ITEMS);
    lv_obj_set_style_border_side(s_kbd, LV_BORDER_SIDE_BOTTOM, LV_PART_ITEMS);
    lv_obj_set_style_border_color(s_kbd, lv_color_hex(UI_COL_BORDER), LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_kbd, 3, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(s_kbd, 0, LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_kbd, lv_color_hex(UI_COL_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_text_font(s_kbd, ui_font_cn(22), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_kbd, lv_color_hex(UI_COL_ORANGE), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(s_kbd, lv_color_hex(UI_COL_WHITE), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(s_kbd, lv_color_hex(UI_COL_BORDER), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_keyboard_set_map(s_kbd, LV_KEYBOARD_MODE_USER_1, s_host_map, s_host_ctrl);
    lv_keyboard_set_map(s_kbd, LV_KEYBOARD_MODE_USER_2, s_port_map, s_port_ctrl);
    focus_field(s_host_ta);
    return root;
}

void screen_server_on_enter(void)
{
    char url[96], host[64];
    int port = 443;
    settings_get_server_url(url, sizeof(url));
    ui_server_split(url, host, sizeof(host), &port);
    lv_textarea_set_text(s_host_ta, host);
    char p[8];
    snprintf(p, sizeof(p), "%d", port);
    lv_textarea_set_text(s_port_ta, p);
    focus_field(s_host_ta);
}
