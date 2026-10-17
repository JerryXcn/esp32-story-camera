/**
 * @file settings_service.c
 * @brief NVS 键值实现。所有写入都走 nvs_commit；Wi-Fi 密码只在本组件与
 *        NVS 中存在，日志与 UI 默认脱敏（实施计划 9.2）。
 */

#include "settings_service.h"

#include <string.h>
#include "sdkconfig.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "settings";
static const char *NS = "storybox";
static SemaphoreHandle_t s_seq_lock;

static const char *K_AGE = "age";
static const char *K_LEN = "len";
static const char *K_VOL = "vol";
static const char *K_CONSENT = "consent";
static const char *K_SSID = "wifi_ssid";
static const char *K_PASS = "wifi_pass";
static const char *K_SSID_LEN = "wifi_ssid_len";
static const char *K_VALID = "wifi_valid";
static const char *K_SEQ = "cap_seq";
static const char *K_DEV_ID = "dev_id";
static const char *K_TOKEN = "dev_token";
static const char *K_SERVER = "server_url";

static void gen_random_hex32(char out[33])
{
    static const char hexd[] = "0123456789abcdef";
    uint8_t raw[16];
    esp_fill_random(raw, sizeof(raw));
    for (int i = 0; i < 16; i++) {
        out[i * 2] = hexd[raw[i] >> 4];
        out[i * 2 + 1] = hexd[raw[i] & 0xf];
    }
    out[32] = '\0';
}

esp_err_t settings_init(void)
{
    if (!s_seq_lock) s_seq_lock = xSemaphoreCreateMutex();
    if (!s_seq_lock) return ESP_ERR_NO_MEM;
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase nvs");
        err = nvs_flash_init();
    }
    return err;
}

void settings_load_product(product_settings_t *out)
{
    out->age_group = AGE_GROUP_5_6;
    out->story_length = STORY_LENGTH_STANDARD;
    out->volume = 60;                 /* 出厂默认：分段音量曲线下约 -9 dB（2026-10-16 用户反馈偏小后上调） */
    out->consent_version = 0;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t u8;
    uint32_t u32;
    if (nvs_get_u8(h, K_AGE, &u8) == ESP_OK && u8 < 3) out->age_group = (age_group_t)u8;
    if (nvs_get_u8(h, K_LEN, &u8) == ESP_OK && u8 < 3) out->story_length = (story_length_t)u8;
    if (nvs_get_u8(h, K_VOL, &u8) == ESP_OK && u8 <= 100) out->volume = u8;
    if (nvs_get_u32(h, K_CONSENT, &u32) == ESP_OK) out->consent_version = u32;
    nvs_close(h);
}

esp_err_t settings_save_product(const product_settings_t *in)
{
    if (!in || in->age_group < 0 || in->age_group > 2 || in->story_length < 0 || in->story_length > 2 || in->volume > 100) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err) return err;
    err = nvs_set_u8(h, K_AGE, (uint8_t)in->age_group);
    if (err == ESP_OK) err = nvs_set_u8(h, K_LEN, (uint8_t)in->story_length);
    if (err == ESP_OK) err = nvs_set_u8(h, K_VOL, in->volume);
    if (err == ESP_OK) err = nvs_set_u32(h, K_CONSENT, in->consent_version);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

void settings_load_wifi(wifi_credentials_t *out)
{
    memset(out, 0, sizeof(*out));
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(out->ssid);
    nvs_get_blob(h, K_SSID, out->ssid, &len);
    len = sizeof(out->password);
    nvs_get_blob(h, K_PASS, out->password, &len);
    nvs_get_u8(h, K_SSID_LEN, &out->ssid_len);
    uint8_t valid = 0;
    nvs_get_u8(h, K_VALID, &valid);
    out->ssid[32] = 0;
    out->password[64] = 0;
    out->valid = valid == 1 && out->ssid_len > 0 && out->ssid_len <= 32;
    nvs_close(h);
}

esp_err_t settings_save_wifi(const wifi_credentials_t *in)
{
    if (!in || !in->ssid_len || in->ssid_len > 32 || strnlen(in->password, sizeof(in->password)) > 64) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err) return err;
    err = nvs_set_blob(h, K_SSID, in->ssid, in->ssid_len);
    if (err == ESP_OK) err = nvs_set_blob(h, K_PASS, in->password, strlen(in->password));
    if (err == ESP_OK) err = nvs_set_u8(h, K_SSID_LEN, in->ssid_len);
    if (err == ESP_OK) err = nvs_set_u8(h, K_VALID, 1);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

void settings_clear_wifi(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, K_SSID);
    nvs_erase_key(h, K_PASS);
    nvs_erase_key(h, K_SSID_LEN);
    nvs_set_u8(h, K_VALID, 0);
    nvs_commit(h);
    nvs_close(h);
}

esp_err_t settings_reserve_capture_seq(uint32_t count, uint32_t *first_out)
{
    if (!count || !first_out) return ESP_ERR_INVALID_ARG;
    if (!s_seq_lock) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_seq_lock, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        uint32_t next = 1;
        err = nvs_get_u32(h, K_SEQ, &next);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
        if (err == ESP_OK && (!next || count > UINT32_MAX - next)) err = ESP_ERR_NOT_SUPPORTED;
        if (err == ESP_OK) err = nvs_set_u32(h, K_SEQ, next + count);
        if (err == ESP_OK) err = nvs_commit(h);
        if (err == ESP_OK) *first_out = next;
        nvs_close(h);
    }
    xSemaphoreGive(s_seq_lock);
    return err;
}

esp_err_t settings_ensure_capture_seq(uint32_t highest_existing)
{
    if (highest_existing == UINT32_MAX) return ESP_ERR_NOT_SUPPORTED;
    if (!s_seq_lock) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_seq_lock, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        uint32_t next = 1;
        err = nvs_get_u32(h, K_SEQ, &next);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
        if (err == ESP_OK && next <= highest_existing) {
            err = nvs_set_u32(h, K_SEQ, highest_existing + 1);
            if (err == ESP_OK) err = nvs_commit(h);
        }
        nvs_close(h);
    }
    xSemaphoreGive(s_seq_lock);
    return err;
}

esp_err_t settings_get_device_id(char out[SETTINGS_DEVICE_ID_HEX])
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err) return err;
    size_t len = SETTINGS_DEVICE_ID_HEX;
    err = nvs_get_blob(h, K_DEV_ID, out, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        gen_random_hex32(out);
        err = nvs_set_blob(h, K_DEV_ID, out, SETTINGS_DEVICE_ID_HEX);
        if (err == ESP_OK) err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t settings_get_device_token(char *buf, size_t len)
{
    if (len < 33) return ESP_ERR_INVALID_SIZE;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err) return err;
    size_t blen = len;
    err = nvs_get_blob(h, K_TOKEN, buf, &blen);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
#ifdef CONFIG_STORY_DEV_TOKEN
        /* 开发联调：预置令牌（服务端按 SC_DEVICE_TOKENS 登记）；
         * 量产构建该宏为空字符串，走随机生成。 */
        if (sizeof(CONFIG_STORY_DEV_TOKEN) - 1 == 32) {
            strlcpy(buf, CONFIG_STORY_DEV_TOKEN, 33);
        } else
#endif
        {
            gen_random_hex32(buf);
        }
        buf[32] = '\0';
        err = nvs_set_blob(h, K_TOKEN, buf, 33);
        if (err == ESP_OK) err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

void settings_get_server_url(char *buf, size_t len)
{
    strlcpy(buf, CONFIG_STORY_SERVER_BASE_URL, len);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t n = len;
    if (nvs_get_str(h, K_SERVER, buf, &n) != ESP_OK || !buf[0]) strlcpy(buf, CONFIG_STORY_SERVER_BASE_URL, len);
    nvs_close(h);
}

esp_err_t settings_set_server_url(const char *url)
{
    if (!url || strlen(url) >= 96 || (url[0] && strncmp(url, "https://", 8) != 0)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err) return err;
    err = url[0] ? nvs_set_str(h, K_SERVER, url) : nvs_erase_key(h, K_SERVER);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}
