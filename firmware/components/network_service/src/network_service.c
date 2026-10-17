/**
 * @file network_service.c
 * @brief Wi-Fi（ESP32-C6 经 esp_wifi_remote/esp_hosted）与 SNTP 校时实现。
 *
 * 行为契约（实施计划 9.2 / 7.5 + 优化建议 FLOW-07）：
 * - 重连/扫描/连接确认全部在独立网络任务执行；事件回调只置状态位，
 *   绝不在共享事件循环内延时退避（FLOW-07）。
 * - 有界退避（1s→2s→…→30s 封顶）；断网绝不自动触发待生成照片上传。
 * - 显式连接（用户配网）走一次性确认流程：GOT_IP 或 20s 超时后经回调
 *   报结果；凭据在 GOT_IP 确认后才允许保存。
 * - 校时成功才 system_time_mark_synced；失败允许离线使用。
 * - 扫描保留真实 SSID 字节，不按名称推断频段，不因含"5G"过滤。
 */

#include "network_service.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"

#include "settings_service.h"
#include "system_service.h"
#include "ping/ping_sock.h"

static const char *TAG = "network";

#define CONNECTED_BIT BIT0
#define GOT_IP_BIT BIT1
#define CANCEL_BIT BIT2
#define DISCONNECTED_BIT BIT3
#define RECONNECT_MAX_DELAY_MS 30000
#define CONNECT_TIMEOUT_MS 20000

/* 网络任务命令（重连/扫描/连接确认在任务内串行执行） */
typedef enum {
    NET_CMD_RECONNECT = 0,      /* 按退避计划自动重连 */
    NET_CMD_SCAN,
    NET_CMD_CONNECT,            /* 用户显式连接（带确认回调） */
    NET_CMD_PROBE_START,        /* GOT_IP 后启动网关探测 */
    NET_CMD_PROBE_STOP,
    NET_CMD_RELINK,             /* 探测连续失败：链路假在线，强制重连 */
} net_cmd_t;

typedef struct {
    uint32_t epoch;
    net_cmd_t cmd;
    wifi_scan_cb scan_cb;
    void *scan_user;
    char ssid[33];
    uint8_t ssid_len;
    char password[65];
    net_connect_cb connect_cb;
    void *connect_user;
} net_msg_t;

static EventGroupHandle_t s_events;
static net_state_cb s_cb;
static void *s_cb_user;
static net_state_t s_state = NET_STATE_IDLE;
static volatile int s_backoff_ms = 1000;
static QueueHandle_t s_cmdq;
static volatile bool s_explicit_connecting;   /* 用户配网进行中（暂停自动重连） */
static volatile bool s_user_disconnect;       /* 主动断开：暂停自动重连 */
static uint32_t s_connect_epoch;
static portMUX_TYPE s_connect_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t connect_epoch(bool increment)
{
    portENTER_CRITICAL(&s_connect_lock);
    if (increment) ++s_connect_epoch;
    uint32_t value = s_connect_epoch;
    portEXIT_CRITICAL(&s_connect_lock);
    return value;
}
/* 链路看门狗：实机见过 C6 关联仍在、数据收发全断（无 DISCONNECTED 事件，
 * 设备与后端均不可达，手动重连即恢复）。GOT_IP 后每 10 s ping 网关，
 * 连续 3 次超时判定假在线并强制重连；同时充当保活，避免 AP 空闲踢出。 */
#define PROBE_INTERVAL_MS 10000
#define PROBE_TIMEOUT_MS  2000
#define PROBE_MAX_MISSES  3
static esp_ping_handle_t s_probe;
static volatile int s_probe_misses;
static uint32_t s_gw_addr;

static void post_cmd(net_cmd_t cmd)
{
    if (!s_cmdq) return;
    net_msg_t msg = { .cmd = cmd };
    xQueueSend(s_cmdq, &msg, 0);
}

static void on_probe_success(esp_ping_handle_t hdl, void *args) { s_probe_misses = 0; }

static void on_probe_timeout(esp_ping_handle_t hdl, void *args)
{
    if (++s_probe_misses >= PROBE_MAX_MISSES) {
        s_probe_misses = 0;
        post_cmd(NET_CMD_RELINK);
    }
}

static void probe_stop(void)
{
    if (!s_probe) return;
    esp_ping_stop(s_probe);
    esp_ping_delete_session(s_probe);
    s_probe = NULL;
}

static void probe_start(void)
{
    probe_stop();
    if (!s_gw_addr) return;
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.count = ESP_PING_COUNT_INFINITE;
    cfg.interval_ms = PROBE_INTERVAL_MS;
    cfg.timeout_ms = PROBE_TIMEOUT_MS;
    cfg.data_size = 16;
    cfg.target_addr.type = IPADDR_TYPE_V4;
    cfg.target_addr.u_addr.ip4.addr = s_gw_addr;
    esp_ping_callbacks_t cbs = { .on_ping_success = on_probe_success,
                                 .on_ping_timeout = on_probe_timeout };
    s_probe_misses = 0;
    if (esp_ping_new_session(&cfg, &cbs, &s_probe) == ESP_OK) esp_ping_start(s_probe);
    else s_probe = NULL;
}

static char s_target_ssid[33];
static uint8_t s_target_len;
static volatile bool s_wifi_started;

static void set_state(net_state_t s)
{
    if (s_state == s) return;
    s_state = s;
    if (s_cb) s_cb(s, s_cb_user);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case WIFI_EVENT_STA_START:
        /* Connection is started explicitly after credentials are loaded. */
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        xEventGroupClearBits(s_events, CONNECTED_BIT | GOT_IP_BIT);
        xEventGroupSetBits(s_events, DISCONNECTED_BIT);
        set_state(NET_STATE_IDLE);
        post_cmd(NET_CMD_PROBE_STOP);
        /* 不在回调内延时重连（FLOW-07）：投递给网络任务按退避执行 */
        if (s_cmdq && !s_explicit_connecting && !s_user_disconnect) {
            net_msg_t msg = { .cmd = NET_CMD_RECONNECT };
            xQueueSend(s_cmdq, &msg, 0);
        }
        break;
    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (s_user_disconnect) return;
    if (s_explicit_connecting) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK ||
            memcmp(ap.ssid, s_target_ssid, s_target_len) != 0 ||
            (s_target_len < 32 && ap.ssid[s_target_len] != 0)) return;
    }
    xEventGroupSetBits(s_events, CONNECTED_BIT | GOT_IP_BIT);
    s_backoff_ms = 1000;
    set_state(NET_STATE_GOT_IP);
    s_gw_addr = ((ip_event_got_ip_t *)data)->ip_info.gw.addr;
    post_cmd(NET_CMD_PROBE_START);
    /* 校时：NTP 成功后标记可信时间 */
    if (!esp_sntp_enabled()) {
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "ntp.aliyun.com");
        esp_sntp_init();
    }
}

static void sntp_sync_cb(struct timeval *tv)
{
    system_time_mark_synced();
    ESP_LOGI(TAG, "time synced: %lld", (long long)tv->tv_sec);
}

/* ---------- 网络任务 ---------- */

/** 显式连接失败/取消后回到已保存网络（9.2"失败仍能返回旧设置"）：
 *  STA 配置换回保存的凭据并进入退避重连。否则 STA 一直持有输错的密码，
 *  开机时路由器未就绪也会就此停止重试。 */
static void restore_saved_network(int64_t *reconnect_due)
{
    wifi_credentials_t cred;
    settings_load_wifi(&cred);
    if (cred.valid) {
        wifi_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        memcpy(cfg.sta.ssid, cred.ssid, cred.ssid_len);
        memcpy(cfg.sta.password, cred.password, strnlen(cred.password, sizeof(cfg.sta.password)));
        esp_wifi_disconnect();
        if (esp_wifi_set_config(WIFI_IF_STA, &cfg) == ESP_OK) {
            s_user_disconnect = false;
            s_backoff_ms = 1000;
            *reconnect_due = esp_timer_get_time() + (int64_t)s_backoff_ms * 1000;
        }
        memset(&cfg, 0, sizeof(cfg));
    }
    memset(&cred, 0, sizeof(cred));
}

static void net_task(void *arg)
{
    (void)arg;
    net_msg_t msg;
    int64_t reconnect_due = 0;
    for (;;) {
        if (xQueueReceive(s_cmdq, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
            if (reconnect_due && esp_timer_get_time() >= reconnect_due) {
                reconnect_due = 0;
                if (!s_user_disconnect && !s_explicit_connecting && network_state() < NET_STATE_GOT_IP) {
                    esp_wifi_connect();
                    s_backoff_ms = s_backoff_ms * 2 > RECONNECT_MAX_DELAY_MS ? RECONNECT_MAX_DELAY_MS : s_backoff_ms * 2;
                }
            }
            continue;
        }
        switch (msg.cmd) {
        case NET_CMD_RECONNECT: {
            if (s_user_disconnect || s_explicit_connecting ||
                network_state() >= NET_STATE_GOT_IP) {
                break;                    /* 显式流程优先，不打架 */
            }
            if (!reconnect_due) reconnect_due = esp_timer_get_time() + (int64_t)s_backoff_ms * 1000;
            break;
        }
        case NET_CMD_SCAN: {
            wifi_scan_config_t cfg = { .show_hidden = false };
            wifi_scan_item_t *items = calloc(16, sizeof(wifi_scan_item_t));
            int n = 0;
            if (items && esp_wifi_scan_start(&cfg, true /* 任务内阻塞 */) == ESP_OK) {
                uint16_t found = 0;
                esp_wifi_scan_get_ap_num(&found);
                if (found > 16) found = 16;
                wifi_ap_record_t *records = calloc(found, sizeof(wifi_ap_record_t));
                if (records) {
                    esp_wifi_scan_get_ap_records(&found, records);
                    for (uint16_t i = 0; i < found; i++) {
                        uint8_t len = 0;
                        while (len < 32 && records[i].ssid[len]) len++;
                        /* 同名多 AP（双频/Mesh）对用户是同一个选择：只保留信号最强的一条 */
                        int dup = -1;
                        for (int k = 0; k < n && dup < 0; k++)
                            if (items[k].ssid_len == len && !memcmp(items[k].ssid, records[i].ssid, len)) dup = k;
                        if (len == 0) continue;
                        if (dup >= 0) {
                            if (records[i].rssi > items[dup].rssi) items[dup].rssi = records[i].rssi;
                            continue;
                        }
                        memcpy(items[n].ssid, records[i].ssid, len);
                        items[n].ssid[len] = '\0';
                        items[n].ssid_len = len;
                        items[n].rssi = records[i].rssi;
                        items[n].auth_required = records[i].authmode != WIFI_AUTH_OPEN;
                        n++;
                    }
                    free(records);
                }
            }
            if (msg.scan_cb) msg.scan_cb(items, n, msg.scan_user);
            free(items);
            break;
        }
        case NET_CMD_CONNECT: {
            if (msg.epoch != connect_epoch(false)) {
                if (msg.connect_cb) msg.connect_cb(NET_CONNECT_FAILED, msg.connect_user);
                break;
            }
            reconnect_due = 0;
            s_explicit_connecting = true;
            s_user_disconnect = false;
            memcpy(s_target_ssid, msg.ssid, sizeof(s_target_ssid));
            s_target_len = msg.ssid_len;
            xEventGroupClearBits(s_events, GOT_IP_BIT | CONNECTED_BIT | CANCEL_BIT | DISCONNECTED_BIT);
            if (esp_wifi_disconnect() == ESP_OK)
                xEventGroupWaitBits(s_events, DISCONNECTED_BIT, pdTRUE, pdTRUE, pdMS_TO_TICKS(1000));
            if (msg.epoch != connect_epoch(false)) {
                s_explicit_connecting = false;
                if (msg.connect_cb) msg.connect_cb(NET_CONNECT_FAILED, msg.connect_user);
                break;
            }
            xEventGroupClearBits(s_events, GOT_IP_BIT | CONNECTED_BIT);
            set_state(NET_STATE_IDLE);
            wifi_config_t cfg;
            memset(&cfg, 0, sizeof(cfg));
            memcpy(cfg.sta.ssid, msg.ssid, msg.ssid_len);
            memcpy(cfg.sta.password, msg.password, strnlen(msg.password, sizeof(cfg.sta.password)));
            esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
            if (err == ESP_OK) err = esp_wifi_connect();
            if (err != ESP_OK) {
                s_explicit_connecting = false;
                if (msg.connect_cb) msg.connect_cb(NET_CONNECT_FAILED, msg.connect_user);
                break;
            }
            /* 确认语义：等 GOT_IP（不是 connect() 异步受理） */
            EventBits_t bits = xEventGroupWaitBits(s_events, GOT_IP_BIT | CANCEL_BIT, pdFALSE, pdFALSE,
                                                   pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));
            bool ok = (bits & GOT_IP_BIT) && !(bits & CANCEL_BIT) && msg.epoch == connect_epoch(false);
            s_explicit_connecting = false;
            if (!ok) restore_saved_network(&reconnect_due);
            if (msg.connect_cb) msg.connect_cb(ok ? NET_CONNECT_OK : NET_CONNECT_FAILED, msg.connect_user);
            memset(&msg, 0, sizeof(msg));
            break;
        }
        case NET_CMD_PROBE_START:
            if (network_state() >= NET_STATE_GOT_IP) probe_start();
            break;
        case NET_CMD_PROBE_STOP:
            probe_stop();
            break;
        case NET_CMD_RELINK:
            if (s_explicit_connecting || s_user_disconnect || network_state() < NET_STATE_GOT_IP) break;
            ESP_LOGW(TAG, "gateway unreachable %d times while associated; forcing reconnect",
                     PROBE_MAX_MISSES);
            probe_stop();
            /* DISCONNECTED 事件走既有退避重连，STA 配置即已保存凭据 */
            esp_wifi_disconnect();
            break;
        default:
            break;
        }
    }
}

int network_scan(wifi_scan_item_t *out, int max)
{
    /* 兼容保留：同步扫描（仅诊断工具用；UI 走异步） */
    wifi_scan_config_t cfg = { .show_hidden = false };
    if (esp_wifi_scan_start(&cfg, true) != ESP_OK) return 0;
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > (uint16_t)max) n = (uint16_t)max;
    wifi_ap_record_t *records = calloc(n, sizeof(wifi_ap_record_t));
    if (!records) return 0;
    esp_wifi_scan_get_ap_records(&n, records);
    for (uint16_t i = 0; i < n; i++) {
        uint8_t len = 0;
        while (len < 32 && records[i].ssid[len]) len++;
        memcpy(out[i].ssid, records[i].ssid, len);
        out[i].ssid[len] = '\0';
        out[i].ssid_len = len;
        out[i].rssi = records[i].rssi;
        out[i].auth_required = records[i].authmode != WIFI_AUTH_OPEN;
    }
    free(records);
    return (int)n;
}

esp_err_t network_scan_async(wifi_scan_cb cb, void *user)
{
    if (!s_cmdq) return ESP_ERR_INVALID_STATE;
    net_msg_t msg = {
        .cmd = NET_CMD_SCAN,
        .scan_cb = cb,
        .scan_user = user,
    };
    return xQueueSend(s_cmdq, &msg, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t network_connect_async(const char *ssid, uint8_t ssid_len,
                                const char *password, net_connect_cb cb, void *user)
{
    if (!s_cmdq) return ESP_ERR_INVALID_STATE;
    if (!ssid || !ssid_len || ssid_len > 32 || (password && strlen(password) > 64)) return ESP_ERR_INVALID_ARG;
    net_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.cmd = NET_CMD_CONNECT;
    msg.epoch = connect_epoch(true);
    msg.connect_cb = cb;
    msg.connect_user = user;
    memcpy(msg.ssid, ssid, ssid_len);
    msg.ssid_len = ssid_len;
    strlcpy(msg.password, password ? password : "", sizeof(msg.password));
    return xQueueSend(s_cmdq, &msg, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t network_connect(const char *ssid, uint8_t ssid_len, const char *password)
{
    wifi_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    if (ssid_len > 32) return ESP_ERR_INVALID_ARG;
    memcpy(cfg.sta.ssid, ssid, ssid_len);
    strlcpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password));
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG, "set cfg");
    return esp_wifi_connect();
}

esp_err_t network_connect_saved(void)
{
    wifi_credentials_t cred;
    settings_load_wifi(&cred);
    if (!cred.valid) return ESP_ERR_INVALID_STATE;
    s_user_disconnect = false;
    return network_connect_async(cred.ssid, cred.ssid_len, cred.password, NULL, NULL);
}

void network_disconnect(void)
{
    connect_epoch(true);
    if (s_events) {
        xEventGroupClearBits(s_events, CONNECTED_BIT | GOT_IP_BIT);
        xEventGroupSetBits(s_events, CANCEL_BIT);
    }
    s_user_disconnect = true;     /* 主动断开：暂停自动重连（FLOW-07） */
    esp_wifi_disconnect();
    set_state(NET_STATE_IDLE);
}

net_state_t network_state(void)
{
    return s_state;
}

bool network_time_synced(void)
{
    return system_time_is_synced();
}

esp_err_t network_init(net_state_cb cb, void *user)
{
    s_cb = cb;
    s_cb_user = user;
    s_events = xEventGroupCreate();
    s_cmdq = xQueueCreate(8, sizeof(net_msg_t));
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "loop");
    esp_netif_create_default_wifi_sta();

    /* esp_wifi_remote：C6 协处理器由 esp_hosted 驱动 */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                   on_wifi_event, NULL), TAG, "wifi ev");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   on_ip_event, NULL), TAG, "ip ev");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    /* 关闭 STA 省电：C6 SDIO + 默认 Modem-sleep 下 TLS 握手/上传会出现
     * 30s 级延迟尖峰（实机 2026-09-13 复现）；故事机为供电设备，无需省电 */
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
    s_wifi_started = true;
    sntp_set_time_sync_notification_cb(sntp_sync_cb);

    if (xTaskCreate(net_task, "net_task", 6144, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
