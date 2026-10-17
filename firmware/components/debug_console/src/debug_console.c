/**
 * @file debug_console.c
 * @brief UART-C 调试命令行实现（M0 实机诊断）。
 *
 * 协议（与参考工具 device.py 兼容）：
 *   info                  -> INFO phase=... heap=... 等一行状态
 *   screenshot            -> SCREEN_BEGIN <w> <h> <stride> / PX <base64>... / SCREEN_END
 *   tap <x> <y>           -> 模拟点击（按下 100ms 后松开）
 *   touch <x> <y> <0|1>   -> 模拟按下/松开
 *   ui-reset              -> 回到开机页
 *
 * 说明：截图是 LVGL 场景快照（lv_snapshot_take），用于诊断界面渲染，
 * 不代表物理屏输出的像素；模拟触摸验证界面逻辑，不代表触摸屏灵敏度。
 * 所有 LVGL 访问都在 board_adapter_lvgl_lock() 保护下进行。
 */

#include "debug_console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"
#include "lvgl.h"

#include "board_adapter.h"
#include "app_ui.h"
#include "ui_screens.h"
#include "ui_camera_direct.h"
#include "camera_service.h"
#include "audio_service.h"
#include "network_service.h"
#include "storage_service.h"
#include "settings_service.h"
#include "psa/crypto.h"
#include "api_client.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include <sys/stat.h>
#include "linux/videodev2.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_flash.h"
#include "esp_hosted_ota.h"


/* nettest：分层诊断 —— 明文 HTTP / HTTPS 小响应 / 音频端点（带鉴权、读完整正文） */
static char s_nt_http[128], s_nt_health[128], s_nt_audio[160];

static void cmd_nettest(const char *job_id)
{
    char base[96];
    settings_get_server_url(base, sizeof(base));
    snprintf(s_nt_health, sizeof(s_nt_health), "%s/v1/health", base);
    snprintf(s_nt_audio, sizeof(s_nt_audio), "%s/v1/jobs/%.40s/audio", base,
             job_id && job_id[0] ? job_id : "job_fb8217141ee649ee98f12272af62e408");
    /* 明文探测：同一主机 8000 端口（无监听时失败属正常） */
    const char *host = strstr(base, "://");
    snprintf(s_nt_http, sizeof(s_nt_http), "http://%.60s", host ? host + 3 : base);
    char *colon = strrchr(s_nt_http + 7, ':');
    if (colon) *colon = '\0';
    strlcat(s_nt_http, ":8000/nettest", sizeof(s_nt_http));
    char token[33];
    settings_get_device_token(token, sizeof(token));

    /* 1) 明文 HTTP */
    esp_http_client_config_t cfg1 = {
        .url = s_nt_http,
        .method = HTTP_METHOD_GET, .timeout_ms = 8000,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg1);
    esp_err_t err = cli ? esp_http_client_perform(cli) : ESP_FAIL;
    printf("NETTEST http -> %s\n", err == ESP_OK ? "OK" : esp_err_to_name(err));
    if (cli) esp_http_client_cleanup(cli);

    /* 2) HTTPS health（小响应） */
    esp_http_client_config_t cfg2 = {
        .url = s_nt_health,
        .method = HTTP_METHOD_GET, .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    cli = esp_http_client_init(&cfg2);
    err = cli ? esp_http_client_perform(cli) : ESP_FAIL;
    printf("NETTEST https health -> %s\n",
           err == ESP_OK ? "OK" : esp_err_to_name(err));
    if (cli) esp_http_client_cleanup(cli);

    /* 3) 音频端点：读完整正文（多 TLS 记录响应验证） */
    esp_http_client_config_t cfg3 = {
        .url = s_nt_audio,
        .method = HTTP_METHOD_GET, .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    cli = esp_http_client_init(&cfg3);
    if (!cli) { puts("NETTEST audio init_fail"); puts("NETTEST_DONE"); return; }
    char auth[64];
    snprintf(auth, sizeof(auth), "Bearer %.32s", token);
    esp_http_client_set_header(cli, "Authorization", auth);
    err = esp_http_client_open(cli, 0);
    if (err != ESP_OK) {
        printf("NETTEST audio open -> %s\n", esp_err_to_name(err));
        esp_http_client_cleanup(cli);
        puts("NETTEST_DONE");
        return;
    }
    esp_http_client_fetch_headers(cli);
    int code = esp_http_client_get_status_code(cli);
    int total = 0;
    char buf[2048];
    int n;
    int64_t t0 = esp_timer_get_time();
    while ((n = esp_http_client_read(cli, buf, sizeof(buf))) > 0) total += n;
    int64_t dt = esp_timer_get_time() - t0;
    printf("NETTEST audio -> %d bytes=%d in %lldms last_read=%d\n",
           code, total, (long long)(dt / 1000), n);
    esp_http_client_cleanup(cli);

    /* 4) HTTPS 大 POST（上传路径复刻：先 PSA 哈希，再 27KB multipart POST） */
    char *auth_outer = NULL;
    (void)auth_outer;
    size_t post_len = 27 * 1024;
    char *post = malloc(post_len);
    if (!post) { puts("NETTEST_DONE"); return; }
    memset(post, 'A', post_len);
    {
        /* 复刻 prepare_upload_image：POST 前 PSA SHA-256 */
        uint8_t dg[32];
        size_t dlen = 0;
        psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t *)post, post_len, dg, 32, &dlen);
        printf("NETTEST psa sha done dlen=%d\n", (int)dlen);
    }
    esp_http_client_config_t cfg4 = {
        .url = s_nt_health,
        .method = HTTP_METHOD_POST, .timeout_ms = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 32 * 1024,
    };
    cli = esp_http_client_init(&cfg4);
    if (!cli) { free(post); puts("NETTEST_DONE"); return; }
    char auth2[64];
    char token2[33];
    settings_get_device_token(token2, sizeof(token2));
    snprintf(auth2, sizeof(auth2), "Bearer %.32s", token2);
    esp_http_client_set_header(cli, "Authorization", auth2);
    esp_http_client_set_header(cli, "Idempotency-Key", "00000000000000000000000000000000");
    esp_http_client_set_header(cli, "Content-Type", "multipart/form-data; boundary=storycam-7e21a-boundary");
    esp_http_client_set_post_field(cli, post, (int)post_len);
    int64_t t1 = esp_timer_get_time();
    err = esp_http_client_perform(cli);
    printf("NETTEST post27k -> %s code=%d in %lldms\n", esp_err_to_name(err),
           err == ESP_OK ? esp_http_client_get_status_code(cli) : 0,
           (long long)((esp_timer_get_time() - t1) / 1000));
    esp_http_client_cleanup(cli);
    free(post);

    /* 5) 直接调用 api_post_job（与生成 worker 完全同路径/同参数） */
    {
        extern const char *board_adapter_sd_mount_point(void);
        char op[33], sid[33];
        strlcpy(op, "00000000000000000000000000000000", sizeof(op));
        strlcpy(sid, "00000000000000000000000000000000", sizeof(sid));
        char img[256];
        snprintf(img, sizeof(img), "%.64s/storybox/stories/35278331d0227a8164245add5b2c2bb8/staging/05f3f660e7f7d4e00317f0d06d957396/ai_input.jpg",
                 board_adapter_sd_mount_point());
        /* 用真实存在的原图：扫描第一个 story 目录（递归一层找 staging） */
        DIR *sd = opendir(board_adapter_sd_mount_point());
        if (sd) {
            struct dirent *e;
            char dirbuf[192];
            while ((e = readdir(sd)) != NULL) {
                snprintf(dirbuf, sizeof(dirbuf), "%.64s/storybox/stories/%.64s",
                         board_adapter_sd_mount_point(), e->d_name);
                DIR *sdir = opendir(dirbuf);
                if (!sdir) continue;
                struct dirent *se;
                bool have_orig = false;
                while ((se = readdir(sdir)) != NULL) {
                    if (!strcmp(se->d_name, "original.jpg")) have_orig = true;
                    if (!strcmp(se->d_name, "staging")) {
                        char stgdir[224];
                        snprintf(stgdir, sizeof(stgdir), "%.150s/staging", dirbuf);
                        DIR *std = opendir(stgdir);
                        if (std) {
                            struct dirent *oe;
                            while ((oe = readdir(std)) != NULL) {
                                char cand[300];
                                snprintf(cand, sizeof(cand), "%.200s/%.40s/ai_input.jpg",
                                         stgdir, oe->d_name);
                                FILE *tf = fopen(cand, "rb");
                                if (tf) { fclose(tf); strlcpy(img, cand, sizeof(img)); }
                            }
                            closedir(std);
                        }
                    }
                }
                closedir(sdir);
                if (have_orig) {
                    strlcpy(sid, e->d_name, sizeof(sid));
                    break;
                }
            }
            closedir(sd);
        }
        api_job_status_t st2;
        int http2 = 0;
        int64_t t2 = esp_timer_get_time();
        esp_err_t e2 = api_post_job(op, sid, "generate", 0, "5-6", "standard",
                                    "warm_female_01", "zh-CN",
                                    img, "0000000000000000000000000000000000000000000000000000000000000000",
                                    &st2, &http2);
        printf("NETTEST api_post_job -> %s http=%d in %lldms\n",
               esp_err_to_name(e2), http2,
               (long long)((esp_timer_get_time() - t2) / 1000));
    }
    puts("NETTEST_DONE");
}

static const char *TAG = "console";

#define CONSOLE_LINE_MAX   512
#define CONSOLE_BAUD       115200
#define SCREEN_CHUNK       768          /* 每次 base64 编码的原始字节数 */

/* ---- 注入触摸状态（独立 pointer indev，与实体 FT5x06 并行） ---- */
static volatile int s_inj_x;
static volatile int s_inj_y;
static volatile bool s_inj_pressed;
static lv_indev_t *s_inj_indev;

static volatile uint32_t s_inj_polls;   /* 注入 indev 被轮询次数（诊断用） */

static void inject_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    s_inj_polls++;
    data->point.x = s_inj_x;
    data->point.y = s_inj_y;
    data->state = s_inj_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void inject_create_indev(void)
{
    if (!board_adapter_lvgl_lock(1000)) return;
    lv_display_t *disp = (lv_display_t *)board_adapter_display();
    if (!disp) disp = lv_display_get_default();
    lv_indev_t *indev = lv_indev_create();
    if (indev && disp) {
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_display(indev, disp);
        lv_indev_set_read_cb(indev, inject_read_cb);
        s_inj_indev = indev;
    }
    board_adapter_lvgl_unlock();
}

/* ---- 截图：LVGL 场景 → RGB565 → base64 ---- */
static void cmd_screenshot(void)
{
    if (!board_adapter_lvgl_lock(2000)) {
        puts("SCREEN_ERROR lock");
        return;
    }
    lv_draw_buf_t *buf = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    /* 拍照页旁路直绘中：屏幕内容是 PPA 合成帧，不是 LVGL 场景 */
    const void *direct = ui_camera_direct_last_frame();
    if (buf && direct && buf->header.w == 480 && buf->header.h == 320 && buf->header.stride == 960) {
        memcpy(buf->data, direct, 480 * 320 * 2);
    }
    board_adapter_lvgl_unlock();
    if (!buf) {
        puts("SCREEN_ERROR memory");
        return;
    }
    uint32_t w = buf->header.w, h = buf->header.h, stride = buf->header.stride;
    /* 传输约 40 秒：其间其他任务的日志会插进 PX 行中间导致帧损坏，先静音 */
    esp_log_level_set("*", ESP_LOG_NONE);
    printf("SCREEN_BEGIN %lu %lu %lu\n", (unsigned long)w, (unsigned long)h,
           (unsigned long)stride);
    size_t total = (size_t)stride * h;
    unsigned char out[SCREEN_CHUNK * 2];
    for (size_t i = 0; i < total; i += SCREEN_CHUNK) {
        size_t n = total - i;
        if (n > SCREEN_CHUNK) n = SCREEN_CHUNK;
        size_t written = 0;
        if (mbedtls_base64_encode(out, sizeof(out), &written, buf->data + i, n) != 0) {
            break;
        }
        out[written] = 0;
        printf("PX %s\n", out);
        vTaskDelay(1);          /* 让出串口与渲染 */
    }
    puts("SCREEN_END");
    fflush(stdout);
    esp_log_level_set("*", CONFIG_LOG_DEFAULT_LEVEL);
    if (board_adapter_lvgl_lock(2000)) {
        lv_draw_buf_destroy(buf);
        board_adapter_lvgl_unlock();
    }
}

/* ---- info：一行状态 ---- */
static void cmd_info(void)
{
    uint32_t internal = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t psram = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    board_status_t st = board_adapter_status_snapshot();
    product_settings_t settings;
    settings_load_product(&settings);
    printf("INFO heap=%lu internal=%lu psram=%lu display=%d sd=%d camera=%d audio=%d up_ms=%lu "
           "page=%d net=%d time_synced=%d audio_state=%d progress=%d duration_ms=%lu "
           "camera_fresh=%d luma=%lu stories=%d volume=%u preview_frames=%lu\n",
           (unsigned long)esp_get_free_heap_size(), (unsigned long)internal,
           (unsigned long)psram, st.display_ready,
           st.sd_ready, st.camera_ready, st.audio_ready,
           (unsigned long)(esp_timer_get_time() / 1000),
           (int)ui_navigator_current(), (int)network_state(), (int)network_time_synced(),
           (int)audio_get_state(), audio_progress_permille(), (unsigned long)audio_duration_ms(),
           (int)camera_service_ready(), (unsigned long)camera_luma_mean(), app_ui_list_count(),
           (unsigned)settings.volume, (unsigned long)ui_camera_direct_frames_shown());
    fflush(stdout);
}

/* Read a stable UI index, then perform disk validation outside the LVGL lock. */
static void cmd_storylist(void)
{
    story_summary_t *list = heap_caps_malloc(sizeof(*list) * STORAGE_MAX_STORIES,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!list) { puts("STORYLIST_ERROR memory"); return; }
    if (!board_adapter_lvgl_lock(1000)) {
        free(list); puts("STORYLIST_ERROR lock"); return;
    }
    int count = app_ui_list_count();
    if (count > STORAGE_MAX_STORIES) count = STORAGE_MAX_STORIES;
    if (count > 0) memcpy(list, app_ui_list(), sizeof(*list) * count);
    board_adapter_lvgl_unlock();
    for (int i = 0; i < count; ++i) {
        char path[256];
        uint32_t duration = 0;
        if (list[i].current_version > 0)
            storage_get_playback_info(list[i].story_id, path, sizeof(path), &duration);
        printf("STORY index=%d id=%s version=%d duration_ms=%lu seq=%lu title=%s\n",
               i, list[i].story_id, list[i].current_version, (unsigned long)duration,
               (unsigned long)list[i].capture_seq, list[i].title);
    }
    free(list);
    printf("STORYLIST_DONE %d\n", count);
}

static void cmd_photo_dump(bool thumb)
{
    char id[STORAGE_ID_HEX] = {0}, path[256];
    if (!board_adapter_lvgl_lock(1000)) { puts("FILE_ERROR lock"); return; }
    strlcpy(id, app_ui_current_story()->story_id, sizeof(id));
    board_adapter_lvgl_unlock();
    esp_err_t err = thumb ? storage_get_thumb_path(id, path, sizeof(path))
                         : storage_get_photo_path(id, path, sizeof(path));
    FILE *f = err == ESP_OK ? fopen(path, "rb") : NULL;
    if (!f) { puts("FILE_ERROR missing"); return; }
    fseek(f, 0, SEEK_END);
    long length = ftell(f);
    rewind(f);
    if (length <= 0 || length > 4 * 1024 * 1024) {
        fclose(f); puts("FILE_ERROR size"); return;
    }
    printf("FILE_BEGIN %s %ld\n", thumb ? "thumb.jpg" : "original.jpg", length);
    unsigned char data[SCREEN_CHUNK], b64[SCREEN_CHUNK * 2];
    size_t n;
    bool failed = false;
    while ((n = fread(data, 1, sizeof(data), f)) > 0) {
        size_t written = 0;
        if (mbedtls_base64_encode(b64, sizeof(b64), &written, data, n) != 0) { failed = true; break; }
        b64[written] = 0;
        printf("FX %s\n", b64);
        vTaskDelay(1);
    }
    failed |= ferror(f) != 0;
    fclose(f);
    puts(failed ? "FILE_ERROR read" : "FILE_END");
}

/* ---- C6 协处理器 OTA ----
 * 镜像由 PC 端 esptool 写入未分区的裸 Flash 区（实机分区表 factory 止于
 * 0xA10000），本命令按 esp_hosted 四段 RPC（begin/write/end/activate）推送。 */
#define C6FW_FLASH_OFFSET 0xC00000u
#define C6FW_MAX_SIZE     (2u * 1024 * 1024)

static void cmd_c6ota(const char *line)
{
    unsigned size = 0;
    if (sscanf(line, "c6ota %u", &size) != 1 || size == 0 || size > C6FW_MAX_SIZE) {
        puts("C6OTA_ERROR size");
        return;
    }
    uint8_t *chunk = malloc(1500);
    if (!chunk) { puts("C6OTA_ERROR nomem"); return; }
    uint8_t magic = 0;
    if (esp_flash_read(NULL, &magic, C6FW_FLASH_OFFSET, 1) != ESP_OK || magic != 0xE9) {
        printf("C6OTA_ERROR image magic=0x%02x\n", magic);
        free(chunk);
        return;
    }
    esp_err_t err = esp_hosted_slave_ota_begin();
    if (err != ESP_OK) {
        printf("C6OTA_ERROR begin %s\n", esp_err_to_name(err));
        free(chunk);
        return;
    }
    unsigned done = 0, last_pct = 101;
    while (done < size && err == ESP_OK) {
        unsigned n = size - done > 1500 ? 1500 : size - done;
        err = esp_flash_read(NULL, chunk, C6FW_FLASH_OFFSET + done, n);
        if (err == ESP_OK) err = esp_hosted_slave_ota_write(chunk, n);
        if (err == ESP_OK) done += n;
        unsigned pct = done * 100u / size;
        if (pct / 10 != last_pct / 10) { printf("C6OTA_PROGRESS %u%%\n", pct); last_pct = pct; }
    }
    free(chunk);
    esp_err_t end_err = esp_hosted_slave_ota_end();
    if (err != ESP_OK || end_err != ESP_OK) {
        printf("C6OTA_ERROR write=%s end=%s at %u\n", esp_err_to_name(err),
               esp_err_to_name(end_err), done);
        return;
    }
    /* activate 仅 v2.6.0+ 支持；旧固件在 end 时已切换启动分区，失败可忽略。 */
    err = esp_hosted_slave_ota_activate();
    printf("C6OTA_DONE activate=%s, restarting\n", esp_err_to_name(err));
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}

/* ---- wifi <ssid> <password>：调试配网，成功(GOT_IP)后才保存凭据 ---- */
static wifi_credentials_t s_console_cred;

static void on_console_connect(net_connect_result_t result, void *user)
{
    (void)user;
    if (result == NET_CONNECT_OK) {
        esp_err_t err = settings_save_wifi(&s_console_cred);
        printf("WIFI_OK saved=%s\n", esp_err_to_name(err));
    } else {
        puts("WIFI_FAIL");
    }
    memset(&s_console_cred, 0, sizeof(s_console_cred));
    fflush(stdout);
}

static void cmd_wifi(const char *line)
{
    const char *ssid = line + 5;
    const char *sp = strchr(ssid, ' ');
    size_t ssid_len = sp ? (size_t)(sp - ssid) : strlen(ssid);
    const char *pass = sp ? sp + 1 : "";
    if (ssid_len == 0 || ssid_len >= sizeof(s_console_cred.ssid) ||
        strlen(pass) >= sizeof(s_console_cred.password)) {
        puts("WIFI_ERROR args");
        return;
    }
    memset(&s_console_cred, 0, sizeof(s_console_cred));
    memcpy(s_console_cred.ssid, ssid, ssid_len);
    s_console_cred.ssid_len = (uint8_t)ssid_len;
    strcpy(s_console_cred.password, pass);
    s_console_cred.valid = true;
    esp_err_t err = network_connect_async(s_console_cred.ssid, s_console_cred.ssid_len,
                                          s_console_cred.password, on_console_connect, NULL);
    printf("WIFI_CONNECTING %s\n", esp_err_to_name(err));
}

/* ---- 命令分发 ---- */

/* 各子命令实现（前置声明，保持分发函数简洁） */
static void cmd_camera(const char *line);

static void handle_command(char *line)
{
    if (!strcmp(line, "tasks")) {
        /* 各任务剩余栈（字节，历史最低）：-O2 后栈用量变化，越小越危险 */
        UBaseType_t n = uxTaskGetNumberOfTasks();
        TaskStatus_t *st = calloc(n + 4, sizeof(TaskStatus_t));
        if (st) {
            n = uxTaskGetSystemState(st, n + 4, NULL);
            for (UBaseType_t i = 0; i < n; i++) {
                printf("TASK %-16s prio=%2u stack_free=%lu\n", st[i].pcTaskName,
                       (unsigned)st[i].uxCurrentPriority, (unsigned long)st[i].usStackHighWaterMark);
            }
            free(st);
        }
        puts("TASKS_END");
    } else if (!strcmp(line, "info")) {
        cmd_info();
    } else if (!strcmp(line, "storylist")) {
        cmd_storylist();
    } else if (!strcmp(line, "audio tone")) {
        printf("AUDIO_TONE %s\n", esp_err_to_name(audio_play_test_tone()));
    } else if (!strcmp(line, "audio mp3")) {
        printf("AUDIO_MP3 %s\n", esp_err_to_name(audio_play_test_mp3()));
    } else if (!strcmp(line, "photo dump thumb")) {
        cmd_photo_dump(true);
    } else if (!strcmp(line, "photo dump original")) {
        cmd_photo_dump(false);
    } else if (!strncmp(line, "open story ", 11)) {
        int index = -1;
        if (sscanf(line + 11, "%d", &index) != 1 || index < 0 || index >= app_ui_list_count()) {
            puts("OPEN_STORY_ERROR index");
        } else if (board_adapter_lvgl_lock(1000)) {
            app_ui_debug_open_story(index);
            board_adapter_lvgl_unlock();
            printf("OPEN_STORY %d\n", index);
        } else puts("OPEN_STORY_ERROR lock");
    } else if (!strcmp(line, "screenshot")) {
        cmd_screenshot();
    } else if (!strncmp(line, "camreg", 6)) {
        /* camreg r <reg> / camreg w <reg> <val>：直接读写 OV5647 寄存器（诊断） */
        char op = 0;
        unsigned reg = 0, val = 0;
        if (sscanf(line, "camreg %c 0x%x 0x%x", &op, &reg, &val) >= 2 ||
            sscanf(line, "camreg %c %x %x", &op, &reg, &val) >= 2) {
            if (op == 'r') {
                uint8_t v = 0;
                int r = board_adapter_cam_reg_read((uint16_t)reg, &v);
                printf("REG 0x%04x = 0x%02x (%d) %s\n", reg, v, v, r == 0 ? "ok" : "FAIL");
            } else if (op == 'w') {
                int r = board_adapter_cam_reg_write((uint16_t)reg, (uint8_t)val);
                uint8_t v = 0;
                board_adapter_cam_reg_read((uint16_t)reg, &v);
                printf("WREG 0x%04x <- 0x%02x %s readback=0x%02x\n", reg, val,
                       r == 0 ? "ok" : "FAIL", v);
            } else {
                puts("UNKNOWN_COMMAND");
            }
        } else {
            puts("UNKNOWN_COMMAND");
        }
    } else if (!strncmp(line, "camera", 6)) {
        cmd_camera(line);
    } else if (!strcmp(line, "ispstate")) {
        int fd = open("/dev/video20", O_RDWR);
        if (fd < 0) { puts("ISP_OPEN_FAIL"); }
        else {
            const uint32_t ids[] = { V4L2_CID_EXPOSURE, V4L2_CID_GAIN };
            for (int i = 0; i < 2; i++) {
                struct v4l2_control c = { .id = ids[i] };
                if (ioctl(fd, VIDIOC_G_CTRL, &c) == 0)
                    printf("ISP_CTRL 0x%08lx = %ld\n", (unsigned long)ids[i], (long)c.value);
                else
                    printf("ISP_CTRL 0x%08lx unsupported\n", (unsigned long)ids[i]);
            }
            close(fd);
            puts("ISP_DONE");
        }
    } else if (!strcmp(line, "touchstate")) {
        board_touch_status_t t = board_adapter_touch_snapshot();
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        printf("TOUCHSTATE %s %u %u raw=%u,%u ready=%u polls=%lu errors=%lu "
               "invalid=%lu last_error=%d age_ms=%lu inj_polls=%lu\n",
               t.pressed ? "down" : "up", t.x, t.y, t.raw_x, t.raw_y, t.initialized,
               (unsigned long)t.polls, (unsigned long)t.read_errors,
               (unsigned long)t.invalid_points, t.last_error,
               (unsigned long)(now - t.sample_ms), (unsigned long)s_inj_polls);
    } else if (!strcmp(line, "touchtrace on") || !strcmp(line, "touchtrace off")) {
        bool enable = !strcmp(line, "touchtrace on");
        board_adapter_touch_trace(enable);
        printf("TOUCHTRACE %s\n", enable ? "on" : "off");
    } else if (!strncmp(line, "goto ", 5)) {
        int id = atoi(line + 5);
        if (board_adapter_lvgl_lock(1000)) {
            app_ui_debug_goto(id);
            board_adapter_lvgl_unlock();
            printf("GOTO %d\n", id);
        } else {
            puts("GOTO_ERROR lock");
        }
    } else if (!strcmp(line, "ui-reset")) {
        if (board_adapter_lvgl_lock(1000)) {
            app_ui_debug_reset();
            board_adapter_lvgl_unlock();
        }
        puts("UI_RESET");
    } else if (!strncmp(line, "touch ", 6)) {
        int x, y, state;
        if (sscanf(line, "touch %d %d %d", &x, &y, &state) == 3) {
            s_inj_x = x; s_inj_y = y; s_inj_pressed = (state != 0);
            puts("TOUCH_OK");
        } else {
            puts("UNKNOWN_COMMAND");
        }
    } else if (!strncmp(line, "nettest", 7)) {
        cmd_nettest(line[7] == ' ' ? line + 8 : NULL);   /* nettest [job_id]：测指定任务的音频下载 */
    } else if (!strncmp(line, "c6ota ", 6)) {
        cmd_c6ota(line);
    } else if (!strcmp(line, "server") || !strncmp(line, "server ", 7)) {
        /* server               查看后端地址
         * server <https://..>  保存并立即生效；server - 恢复编译默认值 */
        if (line[6] == ' ') {
            const char *url = line + 7;
            esp_err_t err = settings_set_server_url(strcmp(url, "-") ? url : "");
            char now[96];
            settings_get_server_url(now, sizeof(now));
            if (err == ESP_OK) api_client_init(now);
            printf("SERVER %s %s\n", err == ESP_OK ? "saved" : esp_err_to_name(err), now);
        } else {
            char now[96];
            settings_get_server_url(now, sizeof(now));
            printf("SERVER %s\n", now);
        }
    } else if (!strncmp(line, "wifi ", 5)) {
        cmd_wifi(line);
    } else if (!strncmp(line, "ls ", 3)) {
        /* 只读诊断：列出 storybox 目录内容（不跟随无关路径） */
        const char *sub = line + 3;
        char path[192];
        if (sub[0] == '/') {
            snprintf(path, sizeof(path), "%.190s", sub);
        } else {
            snprintf(path, sizeof(path), "%.64s/%.120s", board_adapter_sd_mount_point(), sub);
        }
        DIR *d = opendir(path);
        if (!d) {
            puts("LS_OPEN_FAIL");
        } else {
            struct dirent *e;
            int n = 0;
            while ((e = readdir(d)) != NULL && n < 64) {
                if (e->d_name[0] == '.') continue;
                char f[256];
                struct stat st;
                snprintf(f, sizeof(f), "%.190s/%.64s", path, e->d_name);
                if (stat(f, &st) == 0 && S_ISDIR(st.st_mode)) {
                    printf("LS %s/\n", e->d_name);
                } else {
                    printf("LS %s (%ld bytes)\n", e->d_name, (long)st.st_size);
                }
                n++;
            }
            closedir(d);
            printf("LS_DONE %d\n", n);
        }
    } else if (!strncmp(line, "tap ", 4)) {
        int x, y, hold_ms = 100;   /* tap x y [按住毫秒]：模拟长按 */
        if (sscanf(line, "tap %d %d %d", &x, &y, &hold_ms) >= 2) {
            if (hold_ms < 50) hold_ms = 50;
            if (hold_ms > 10000) hold_ms = 10000;
            if (x < 0 || x >= 480 || y < 0 || y >= 320 || !s_inj_indev) {
                puts("TAP_ERROR coordinates");
            } else if (!board_adapter_lvgl_lock(2000)) {
                puts("TAP_ERROR lock");
            } else {
                s_inj_x = x; s_inj_y = y; s_inj_pressed = true;
                lv_indev_read(s_inj_indev);
                board_adapter_lvgl_unlock();
                vTaskDelay(pdMS_TO_TICKS(hold_ms));
                if (board_adapter_lvgl_lock(2000)) {
                    s_inj_pressed = false;
                    lv_indev_read(s_inj_indev);
                    board_adapter_lvgl_unlock();
                    /* Release/click callbacks have completed before acknowledging. */
                    puts("TAP_OK");
                } else {
                    s_inj_pressed = false;
                    puts("TAP_ERROR release_lock");
                }
            }
        } else {
            puts("UNKNOWN_COMMAND");
        }
    } else if (line[0]) {
        puts("UNKNOWN_COMMAND");
    }
    fflush(stdout);
}

/* ---- camera 子命令 ---- */
static void cmd_camera(const char *line)
{
    if (!strcmp(line, "camera grab")) {
        /* 亮度统计：解码 RGB 后计算真实 Y（77R+150G+29B），不是字节均值 */
        camera_frame_stats_t st;
        esp_err_t e = camera_debug_grab_stats(&st, NULL, 0, NULL, NULL);
        if (e == ESP_OK) {
            printf("CAMERA_FRAME %lux%lu bytes=%lu luma_mean=%lu dark_pct=%lu%%\n",
                   (unsigned long)st.width, (unsigned long)st.height,
                   (unsigned long)st.bytes, (unsigned long)st.luma_mean,
                   (unsigned long)st.luma_dark_pct);
        } else {
            printf("CAMERA_GRAB %s\n", esp_err_to_name(e));
        }
    } else if (!strcmp(line, "camera dump")) {
        static EXT_RAM_BSS_ATTR uint8_t outbuf[640 * 480 * 2];
        uint32_t ow = 0, oh = 0;
        camera_frame_stats_t st;
        esp_err_t e = camera_debug_grab_stats(&st, outbuf, sizeof(outbuf), &ow, &oh);
        if (e != ESP_OK || ow == 0) {
            puts("CAM_ERROR grab");
        } else {
            size_t on = (size_t)ow * oh * 2;
            printf("CAM_BEGIN %lu %lu %lu\n", (unsigned long)ow, (unsigned long)oh,
                   (unsigned long)on);
            unsigned char b64[1025];
            for (size_t i = 0; i < on; i += 768) {
                size_t k = on - i; if (k > 768) k = 768; size_t written = 0;
                if (mbedtls_base64_encode(b64, sizeof(b64), &written, outbuf + i, k) != 0) break;
                b64[written] = 0;
                printf("CX %s\n", b64);
                vTaskDelay(1);
            }
            puts("CAM_END");
            fflush(stdout);
        }
    } else if (!strcmp(line, "camera jpg")) {
        /* 拍照链路自证：编码一帧 JPEG 并输出长度 + 基础统计 */
        camera_capture_t cap;
        esp_err_t e = camera_capture_jpeg(&cap);
        if (e != ESP_OK) {
            printf("CAMERA_JPG %s\n", esp_err_to_name(e));
        } else {
            printf("CAMERA_JPEG %lux%lu jpg=%lu thumb=%lu\n",
                   (unsigned long)cap.jpeg_w, (unsigned long)cap.jpeg_h,
                   (unsigned long)cap.jpeg_len, (unsigned long)cap.thumb_len);
            camera_release_jpeg();
        }
    } else if (!strncmp(line, "camera ctrl", 11)) {
        unsigned set_id = 0; int set_val = 0;
        int n = sscanf(line, "camera ctrl %x %d", &set_id, &set_val);
        int fd = open("/dev/video0", O_RDWR);
        if (fd < 0) { puts("CTRL_OPEN_FAIL"); }
        else {
            if (n >= 1 && set_id) {
                struct v4l2_ext_control ec = { .id = set_id, .value = set_val };
                struct v4l2_ext_controls ecs = {
                    .ctrl_class = V4L2_CTRL_ID2CLASS(set_id), .count = 1, .controls = &ec,
                };
                int r = ioctl(fd, VIDIOC_S_EXT_CTRLS, &ecs);
                printf("SET 0x%08x = %d -> %s\n", set_id, set_val, r == 0 ? "OK" : "FAIL");
            }
            struct v4l2_query_ext_ctrl q = { .id = V4L2_CTRL_FLAG_NEXT_CTRL };
            int found = 0;
            while (ioctl(fd, VIDIOC_QUERY_EXT_CTRL, &q) == 0) {
                printf("CTRL 0x%08lx min=%lld max=%lld def=%lld\n",
                       (unsigned long)q.id, (long long)q.minimum, (long long)q.maximum,
                       (long long)q.default_value);
                if (++found > 30) break;
                q.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
            }
            if (!found) puts("CTRL none");
            close(fd);
            puts("CTRL_DONE");
        }
    } else if (!strcmp(line, "camera enum")) {
        const char *devs[] = { "/dev/video0", "/dev/video20" };
        for (int di = 0; di < 2; di++) {
            printf("DEV %s\n", devs[di]);
            int fd = open(devs[di], O_RDWR);
            if (fd < 0) { puts("ENUM_OPEN_FAIL"); continue; }
            struct v4l2_fmtdesc f = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
            for (f.index = 0; ioctl(fd, VIDIOC_ENUM_FMT, &f) == 0; f.index++) {
                printf("FMT[%lu] fourcc=0x%08lx %s\n", (unsigned long)f.index,
                       (unsigned long)f.pixelformat, f.description);
            }
            close(fd);
        }
        puts("ENUM_DONE");
    } else {
        puts("UNKNOWN_COMMAND");
    }
}

static void console_task(void *arg)
{
    (void)arg;
    /* UART0 可能已被控制台占用；失败不致命，仍保留 USB Serial/JTAG 通道 */
    esp_err_t uerr = uart_driver_install(UART_NUM_0, 2048, 0, 0, NULL, 0);
    if (uerr != ESP_OK) {
        ESP_LOGW(TAG, "uart0 driver install: %s（仅用 USB Serial/JTAG）", esp_err_to_name(uerr));
    }
    usb_serial_jtag_driver_config_t jc = { .rx_buffer_size = 2048, .tx_buffer_size = 2048 };
    esp_err_t jerr = usb_serial_jtag_driver_install(&jc);
    if (jerr != ESP_OK) {
        ESP_LOGW(TAG, "usb_serial_jtag driver install: %s", esp_err_to_name(jerr));
    }

    static char line[CONSOLE_LINE_MAX];
    size_t pos = 0;
    bool discard = false;

    for (;;) {
        uint8_t c;
        int n = 0;
        if (uerr == ESP_OK) {
            n = uart_read_bytes(UART_NUM_0, &c, 1, 0);
        }
        if (n != 1 && jerr == ESP_OK) {
            n = usb_serial_jtag_read_bytes(&c, 1, 0);
        }
        if (n != 1) {
            vTaskDelay(1);
            continue;
        }
        if (c == '\n' || c == '\r') {
            if (!discard && pos > 0) {
                line[pos] = 0;
                handle_command(line);
            }
            pos = 0;
            discard = false;
        } else if (!discard) {
            if (pos < sizeof(line) - 1) {
                line[pos++] = (char)c;
            } else {
                discard = true;
                puts("COMMAND_TOO_LONG");
            }
        }
    }
}

void debug_console_start(void)
{
    inject_create_indev();
    if (xTaskCreate(console_task, "console", 20 * 1024, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "console task create failed");
    }
}
