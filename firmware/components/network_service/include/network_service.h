#pragma once
/**
 * @file network_service.h
 * @brief Wi-Fi 与校时（实施计划 9.2）。
 * 网络状态必须区分：Wi-Fi 已连接 / 获得IP / 后端可达；
 * 不能仅凭 Wi-Fi CONNECTED 宣称生成可用。
 * 新凭据在连接确认成功后才替换保存；失败仍能返回旧设置。
 * 重连/扫描在独立任务执行，绝不阻塞 Wi-Fi 事件回调（FLOW-07）。
 */

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NET_STATE_IDLE = 0,
    NET_STATE_WIFI_CONNECTED,   /* 关联成功 */
    NET_STATE_GOT_IP,           /* 已获得 IP，可以校时 */
    NET_STATE_BACKEND_OK,       /* /v1/health 探测通过（由 api_client 标记） */
} net_state_t;

typedef struct {
    char ssid[33];      /* 原始字节；显示缺字由 UI 回退处理 */
    uint8_t ssid_len;
    int8_t rssi;
    bool auth_required;
} wifi_scan_item_t;

typedef void (*net_state_cb)(net_state_t state, void *user);

/** 异步扫描完成回调（网络任务上下文；结果缓冲在回调返回后失效）。 */
typedef void (*wifi_scan_cb)(const wifi_scan_item_t *items, int count, void *user);

/** 连接流程结果（实施计划 9.2：GOT_IP 确认成功后才允许保存凭据）。 */
typedef enum {
    NET_CONNECT_FAILED = 0,     /* 超时/认证失败（旧凭据不动） */
    NET_CONNECT_OK = 1,         /* GOT_IP */
} net_connect_result_t;

typedef void (*net_connect_cb)(net_connect_result_t result, void *user);

esp_err_t network_init(net_state_cb cb, void *user);

/** 异步扫描：立即返回；完成后在扫描任务回调（结果临时有效）。 */
esp_err_t network_scan_async(wifi_scan_cb cb, void *user);

/** 连接指定凭据（不落盘）；GOT_IP 或 20s 超时后经 cb 报结果。 */
esp_err_t network_connect_async(const char *ssid, uint8_t ssid_len,
                                const char *password, net_connect_cb cb, void *user);

/** 用已保存凭据后台连接（启动路径）；有界退避重连，不冻结 UI。 */
esp_err_t network_connect_saved(void);

void network_disconnect(void);
net_state_t network_state(void);
bool network_time_synced(void);

#ifdef __cplusplus
}
#endif
