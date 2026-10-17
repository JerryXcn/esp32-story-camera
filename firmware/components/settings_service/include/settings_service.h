#pragma once
/**
 * @file settings_service.h
 * @brief NVS 设置（实施计划 6.1/9.2）：Wi-Fi 凭据、产品设置、首次说明确认、
 *        拍摄序号分配器、设备身份。TF 卡不保存任何密钥。
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SETTINGS_SSID_MAX 33
#define SETTINGS_PASSWORD_MAX 65
#define SETTINGS_DEVICE_ID_HEX 33   /* 32 hex + NUL */

typedef enum {
    AGE_GROUP_3_4 = 0,
    AGE_GROUP_5_6 = 1,
    AGE_GROUP_7_10 = 2,
} age_group_t;

typedef enum {
    STORY_LENGTH_SHORT = 0,
    STORY_LENGTH_STANDARD = 1,
    STORY_LENGTH_LONG = 2,
} story_length_t;

typedef struct {
    char ssid[SETTINGS_SSID_MAX];       /* 保留原始字节，显示缺字由 UI 回退 */
    uint8_t ssid_len;
    char password[SETTINGS_PASSWORD_MAX];
    bool valid;
} wifi_credentials_t;

typedef struct {
    age_group_t age_group;              /* 默认 5-6 */
    story_length_t story_length;        /* 默认 standard */
    uint8_t volume;                     /* 0..100，调试起点 30 */
    uint32_t consent_version;           /* 首次联网说明已确认版本，0=未确认 */
} product_settings_t;

esp_err_t settings_init(void);

void settings_load_product(product_settings_t *out);
esp_err_t settings_save_product(const product_settings_t *in);

void settings_load_wifi(wifi_credentials_t *out);
esp_err_t settings_save_wifi(const wifi_credentials_t *in);   /* 连接成功后才替换 */
void settings_clear_wifi(void);

/* 拍摄序号分配器：成批预留减少写放大，允许跳号（实施计划 6.1） */
esp_err_t settings_reserve_capture_seq(uint32_t count, uint32_t *first_out);
/** After restoring a TF card, advance the counter beyond all existing captures. */
esp_err_t settings_ensure_capture_seq(uint32_t highest_existing);

/* 设备身份：首次生成 128-bit 随机 ID 的 32 位 hex 并持久化 */
esp_err_t settings_get_device_id(char out[SETTINGS_DEVICE_ID_HEX]);
esp_err_t settings_get_device_token(char *buf, size_t len);   /* 首次生成后持久化 */

/** 后端地址：NVS 里有则用之，否则用 CONFIG_STORY_SERVER_BASE_URL（开发联调时电脑 IP 会变）。 */
void settings_get_server_url(char *buf, size_t len);
/** 保存后端地址；空串恢复为编译默认值 */
esp_err_t settings_set_server_url(const char *url);

#ifdef __cplusplus
}
#endif
