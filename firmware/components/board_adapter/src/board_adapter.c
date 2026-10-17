/**
 * @file board_adapter.c
 * @brief 板卡适配层实现（Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5）。
 *
 * 2026-09-12 实机 bring-up（引脚/配置来源：微雪官方 BSP esp32_p4_wifi6_touch_lcd_xc）：
 *  - LCD：ST7796S SPI（MOSI 20 / CLK 21 / CS 23 / DC 26 / RST 27），80MHz，
 *    BGR + invert，背光 LEDC PWM GPIO28；原生 320×480 竖屏，经 swap_xy+mirror
 *    转 480×320 横屏（与产品 UI 坐标一致）。
 *  - 触摸：FT5x06 电容触摸（I2C 共享总线），RST 29 / INT 50，接入 LVGL indev。
 *  - TF：SDMMC 四线（CLK 43 / CMD 44 / D0..3 39..42），挂载 /sdcard。
 *  - 音频：ES8311（I2C 共享总线），I2S MCLK 13 / BCLK 12 / LRCK 10 /
 *    DOUT 9 / DIN 11，功放 PA GPIO53（codec 驱动按播放状态自动开关），
 *    初始化完成播放提示音验证链路。
 *  - 相机：OV5647 经 esp_video（MIPI-CSI，SCCB 共享 I2C，无独立 RST/PWDN）。
 */

#include "sdkconfig.h"
#include "board_adapter.h"

#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <stdatomic.h>
#include "esp_memory_utils.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7796.h"
#include "esp_lcd_touch_ft5x06.h"
#include "driver/ledc.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "driver/sdmmc_host.h"          /* IDF v6.1 legacy 路径 */
#include "driver/sdspi_host.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "esp_vfs_fat.h"
#include "lvgl.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#include "esp_video_init.h"
#include "linux/videodev2.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "board";

static board_status_t s_status;
static const char *s_sd_mount = "/sdcard";

/* ---- 共享 I2C（触摸 FT5x06 + ES8311 + 相机 SCCB） ---- */
#define BSP_I2C_SDA        GPIO_NUM_7
#define BSP_I2C_SCL        GPIO_NUM_8
#define BSP_I2C_SPEED_HZ   (200 * 1000)

/* ---- LCD ST7796S SPI ---- */
#define BSP_LCD_SPI_MOSI   GPIO_NUM_20
#define BSP_LCD_SPI_CLK    GPIO_NUM_21
#define BSP_LCD_SPI_CS     GPIO_NUM_23
#define BSP_LCD_DC         GPIO_NUM_26
#define BSP_LCD_RST        GPIO_NUM_27
#define BSP_LCD_BL         GPIO_NUM_28
#define BSP_LCD_PCLK_HZ    (80 * 1000 * 1000)

/* ---- 触摸 FT5x06 ---- */
#define BSP_TP_RST         GPIO_NUM_29
#define BSP_TP_INT         GPIO_NUM_50

/* ---- TF SDMMC ---- */
#define BSP_SD_CLK         GPIO_NUM_43
#define BSP_SD_CMD         GPIO_NUM_44
#define BSP_SD_D0          GPIO_NUM_39
#define BSP_SD_D1          GPIO_NUM_40
#define BSP_SD_D2          GPIO_NUM_41
#define BSP_SD_D3          GPIO_NUM_42

/* ---- 音频 ES8311 ---- */
#define BSP_I2S_MCLK       GPIO_NUM_13
#define BSP_I2S_BCLK       GPIO_NUM_12
#define BSP_I2S_WS         GPIO_NUM_10
#define BSP_I2S_DOUT       GPIO_NUM_9
#define BSP_I2S_DIN        GPIO_NUM_11
#define BSP_PA             GPIO_NUM_53

static i2c_master_bus_handle_t s_i2c;
static esp_lcd_panel_io_handle_t s_lcd_io;
static esp_lcd_panel_handle_t s_lcd;
static esp_lcd_touch_handle_t s_tp;
static sdmmc_card_t *s_sd_card;
static i2s_chan_handle_t s_i2s_tx;
static esp_codec_dev_handle_t s_codec;
static lv_display_t *s_disp;
static lv_indev_t *s_indev;
static void *s_canvas_buf;             /* 拍摄页预览画布（PSRAM） */
static portMUX_TYPE s_touch_lock = portMUX_INITIALIZER_UNLOCKED;
static board_touch_status_t s_touch_status;
static bool s_touch_trace;
static uint32_t s_touch_trace_ms;

const char *board_adapter_sd_mount_point(void)
{
    return s_sd_mount;
}

board_status_t board_adapter_status_snapshot(void)
{
    return s_status;
}

void *board_adapter_canvas_buffer(void)
{
    return s_canvas_buf;
}

void *board_adapter_audio_codec(void)
{
    return s_codec;
}

/* =====================================================================
 * LVGL 显示通路（ST7796S 真实面板）
 * ===================================================================== */

#define LVGL_TASK_STACK  8192
#define LVGL_TASK_PRIO   8
#define LVGL_BUF_LINES   16   /* 2×15.4KB 内部 DMA；30 行曾加剧内部 RAM 紧张 */

static uint8_t *s_flush_a;   /* DMA 缓冲必须内部 RAM 且对齐：PSRAM 非对齐 EDMA 曾致总线静默挂起 */
static uint8_t *s_flush_b;
/* 刷屏（v0.3.1 性能）：
 * - LVGL：flush_cb 只发起 DMA 立即返回，LVGL 渲染下一块与 SPI 发送重叠；
 *   发送完成由中断计数，flush_wait_cb 在复用缓冲前等待（原先同步 1 ms 轮询，每帧多耗约 16 ms）。
 * - 旁路直绘（拍照页）：LVGL 的输出不再上屏（只记"界面有变化"），由调用方整帧合成后
 *   board_adapter_display_blit 直接写屏；两块内部 DMA 缓冲交替"字节交换+拷贝"与发送。 */
static SemaphoreHandle_t s_flush_done;     /* 每完成一次 draw_bitmap 由中断释放一次（计数） */
static atomic_int s_inflight;              /* 已发起、未完成的 draw_bitmap 数（中断与任务共用） */
static volatile bool s_bypass, s_bypass_dirty;
static SemaphoreHandle_t s_panel_mutex;    /* 旁路直绘与退出旁路互斥（同一时刻只有一方写屏） */
static uint8_t *s_blit_buf[2];
static SemaphoreHandle_t s_lvgl_mutex;   /* LVGL 非线程安全：渲染/截图/注入 必须串行 */

bool board_adapter_lvgl_lock(uint32_t timeout_ms)
{
    if (!s_lvgl_mutex) return true;
    return xSemaphoreTakeRecursive(s_lvgl_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

/* 相机 sensor 直接寄存器访问（OV5647 SCCB 0x36，16 位寄存器地址 + 8 位数据） */
#define CAM_SCCB_ADDR 0x36

int board_adapter_cam_reg_write(uint16_t reg, uint8_t val)
{
    if (!s_i2c) return -1;
    i2c_master_dev_handle_t dev = NULL;
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CAM_SCCB_ADDR,
        .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(s_i2c, &cfg, &dev) != ESP_OK) return -1;
    uint8_t buf[3] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xff), val };
    esp_err_t r = i2c_master_transmit(dev, buf, 3, 100);
    i2c_master_bus_rm_device(dev);
    return r == ESP_OK ? 0 : -1;
}

int board_adapter_cam_reg_read(uint16_t reg, uint8_t *val)
{
    if (!s_i2c || !val) return -1;
    i2c_master_dev_handle_t dev = NULL;
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CAM_SCCB_ADDR,
        .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(s_i2c, &cfg, &dev) != ESP_OK) return -1;
    uint8_t addr[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xff) };
    esp_err_t r = i2c_master_transmit_receive(dev, addr, 2, val, 1, 100);
    i2c_master_bus_rm_device(dev);
    return r == ESP_OK ? 0 : -1;
}

void *board_adapter_display(void)
{
    return s_disp;
}

bool board_adapter_touch_poll(uint16_t *x, uint16_t *y)
{
    board_touch_status_t status = board_adapter_touch_snapshot();
    if (x) *x = status.x;
    if (y) *y = status.y;
    return status.pressed;
}

board_touch_status_t board_adapter_touch_snapshot(void)
{
    portENTER_CRITICAL(&s_touch_lock);
    board_touch_status_t status = s_touch_status;
    portEXIT_CRITICAL(&s_touch_lock);
    return status;
}

void board_adapter_touch_trace(bool enable)
{
    portENTER_CRITICAL(&s_touch_lock);
    s_touch_trace = enable;
    s_touch_trace_ms = 0;
    portEXIT_CRITICAL(&s_touch_lock);
}

void board_adapter_lvgl_unlock(void)
{
    if (s_lvgl_mutex) xSemaphoreGiveRecursive(s_lvgl_mutex);
}

static void swap_rgb565_bytes(uint8_t *p, uint32_t len)
{
    for (uint32_t i = 0; i + 1 < len; i += 2) {
        uint8_t t = p[i];
        p[i] = p[i + 1];
        p[i + 1] = t;
    }
}

/* SPI 面板 IO 发送完成中断：计数减一并唤醒等待方 */
static bool IRAM_ATTR on_color_trans_done(esp_lcd_panel_io_handle_t io,
                                          esp_lcd_panel_io_event_data_t *edata,
                                          void *user_ctx)
{
    (void)io; (void)edata; (void)user_ctx;
    if (atomic_load(&s_inflight) > 0) atomic_fetch_sub(&s_inflight, 1);
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done, &woken);
    return woken == pdTRUE;
}

/** 等到最多 max_inflight 个发送未完成（带上限，异常时不卡死看门狗） */
static void wait_inflight(int max_inflight)
{
    for (int i = 0; i < 20 && atomic_load(&s_inflight) > max_inflight; i++) {
        xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(10));
    }
    if (atomic_load(&s_inflight) > max_inflight) {
        ESP_LOGW(TAG, "lcd dma timeout (inflight=%d)", atomic_load(&s_inflight));
        atomic_store(&s_inflight, max_inflight);
    }
}

static esp_err_t lcd_draw_async(int x1, int y1, int x2, int y2, const void *px)
{
    atomic_fetch_add(&s_inflight, 1);
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_lcd, x1, y1, x2, y2, px);
    if (err != ESP_OK) atomic_fetch_sub(&s_inflight, 1);
    return err;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    if (s_bypass) {
        /* 旁路中：界面由拍照页整帧合成上屏，这里只记录"界面有变化" */
        s_bypass_dirty = true;
        lv_display_flush_ready(disp);
        return;
    }
    /* LVGL 输出小端 RGB565；SPI 面板按大端发送，先做字节对调 */
    uint32_t size = (uint32_t)lv_area_get_width(area) * lv_area_get_height(area) * 2;
    swap_rgb565_bytes(px_map, size);
    if (lcd_draw_async(area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map) != ESP_OK) {
        lv_display_flush_ready(disp);
    }
    /* 发送完成前不调用 flush_ready：LVGL 复用该缓冲前会经 flush_wait_cb 等待 */
}

static void flush_wait_cb(lv_display_t *disp)
{
    (void)disp;
    wait_inflight(0);
}

void board_adapter_display_bypass(bool on)
{
    if (!s_panel_mutex) return;
    xSemaphoreTake(s_panel_mutex, portMAX_DELAY);
    s_bypass = on;
    s_bypass_dirty = true;
    xSemaphoreGive(s_panel_mutex);
    wait_inflight(0);
}

bool board_adapter_display_bypass_active(void)
{
    return s_bypass;
}

bool board_adapter_display_bypass_take_dirty(void)
{
    bool d = s_bypass_dirty;
    s_bypass_dirty = false;
    return d;
}

/* 两个像素一组的 RGB565 字节交换拷贝（小端 → 面板大端） */
static void copy_swap_rgb565(uint32_t *dst, const uint32_t *src, uint32_t words)
{
    for (uint32_t i = 0; i < words; i++) {
        uint32_t w = src[i];
        dst[i] = ((w & 0x00FF00FFu) << 8) | ((w >> 8) & 0x00FF00FFu);
    }
}

esp_err_t board_adapter_display_blit(const void *rgb565_le)
{
    if (!s_bypass || !s_panel_mutex || !s_blit_buf[0] || !rgb565_le) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_panel_mutex, portMAX_DELAY);
    if (!s_bypass) {
        xSemaphoreGive(s_panel_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    wait_inflight(0);
    const uint8_t *src = rgb565_le;
    const uint32_t row_bytes = BOARD_LCD_W * 2;
    esp_err_t err = ESP_OK;
    for (int y = 0, i = 0; y < BOARD_LCD_H && err == ESP_OK; y += LVGL_BUF_LINES, i++) {
        int rows = BOARD_LCD_H - y < LVGL_BUF_LINES ? BOARD_LCD_H - y : LVGL_BUF_LINES;
        uint8_t *buf = s_blit_buf[i & 1];
        wait_inflight(1);   /* 两块缓冲交替：写这一块前，用它的上上块必须已发完 */
        copy_swap_rgb565((uint32_t *)buf, (const uint32_t *)(src + (size_t)y * row_bytes),
                         rows * row_bytes / 4);
        err = lcd_draw_async(0, y, BOARD_LCD_W, y + rows, buf);
    }
    wait_inflight(0);
    xSemaphoreGive(s_panel_mutex);
    return err;
}

/* Controller reports portrait 320x480. Match the panel's landscape rotation
 * exactly once, with inclusive pixel endpoints and no unsigned underflow. */
static bool touch_map_native_to_display(uint16_t raw_x, uint16_t raw_y,
                                        uint16_t *x, uint16_t *y)
{
    if (raw_x >= BOARD_LCD_H || raw_y >= BOARD_LCD_W || !x || !y) return false;
    *x = BOARD_LCD_W - 1 - raw_y;
    /* 2026-10-16 用户实测：短边方向反向（点上方触发下方按钮），短边不取反 */
    *y = raw_x;
    return true;
}

/* 按下/抬起转换队列：采样任务 100 Hz 写入，LVGL 读回调按序回放。
 * 实机相机页 LVGL 渲染一轮约 75 ms，indev 只有约 13 Hz，短按整段落在
 * 两次读取之间就会丢失（"点了没反应"）；转换入队后不再丢。 */
#define TOUCH_EVENT_SLOTS 16
typedef struct { bool pressed; uint16_t x, y; } touch_event_t;
static touch_event_t s_touch_events[TOUCH_EVENT_SLOTS];
static unsigned s_touch_ev_head, s_touch_ev_count;

static void touch_event_push(bool pressed, uint16_t x, uint16_t y)
{
    portENTER_CRITICAL(&s_touch_lock);
    if (s_touch_ev_count == TOUCH_EVENT_SLOTS) {          /* 满：丢最旧的 */
        s_touch_ev_head = (s_touch_ev_head + 1) % TOUCH_EVENT_SLOTS;
        s_touch_ev_count--;
    }
    touch_event_t *ev = &s_touch_events[(s_touch_ev_head + s_touch_ev_count) % TOUCH_EVENT_SLOTS];
    ev->pressed = pressed; ev->x = x; ev->y = y;
    s_touch_ev_count++;
    portEXIT_CRITICAL(&s_touch_lock);
}

static bool touch_event_pop(touch_event_t *out, bool *more)
{
    portENTER_CRITICAL(&s_touch_lock);
    bool got = s_touch_ev_count > 0;
    if (got) {
        *out = s_touch_events[s_touch_ev_head];
        s_touch_ev_head = (s_touch_ev_head + 1) % TOUCH_EVENT_SLOTS;
        s_touch_ev_count--;
    }
    *more = s_touch_ev_count > 0;
    portEXIT_CRITICAL(&s_touch_lock);
    return got;
}

/* Sole consumer of FT5x06 data (touch task); UART diagnostics read the cached sample. */
static void touch_sample(void)
{
    uint16_t raw_x[1] = { 0 }, raw_y[1] = { 0 }, x = 0, y = 0;
    uint8_t cnt = 0;
    esp_err_t err = esp_lcd_touch_read_data(s_tp);
    bool touched = err == ESP_OK &&
        esp_lcd_touch_get_coordinates(s_tp, raw_x, raw_y, NULL, &cnt, 1) && cnt > 0;
    bool pressed = touched && touch_map_native_to_display(raw_x[0], raw_y[0], &x, &y);
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    portENTER_CRITICAL(&s_touch_lock);
    bool changed = pressed != s_touch_status.pressed;
    s_touch_status.initialized = true;
    s_touch_status.polls++;
    s_touch_status.last_error = err;
    if (err != ESP_OK) s_touch_status.read_errors++;
    if (touched && !pressed) s_touch_status.invalid_points++;
    if (touched) {
        s_touch_status.raw_x = raw_x[0];
        s_touch_status.raw_y = raw_y[0];
    }
    if (pressed) {
        s_touch_status.x = x;
        s_touch_status.y = y;
    }
    s_touch_status.pressed = pressed;
    s_touch_status.sample_ms = now;
    bool trace = s_touch_trace && (changed || (pressed && now - s_touch_trace_ms >= 100));
    if (trace) s_touch_trace_ms = now;
    board_touch_status_t status = s_touch_status;
    portEXIT_CRITICAL(&s_touch_lock);
    if (changed) touch_event_push(pressed, status.x, status.y);
    if (trace) ESP_LOGI(TAG, "TOUCH raw=%u,%u mapped=%u,%u state=%s error=%d",
                         status.raw_x, status.raw_y, status.x, status.y,
                         pressed ? "down" : "up", err);
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    touch_event_t ev;
    bool more = false;
    if (touch_event_pop(&ev, &more)) {
        data->point.x = ev.x;
        data->point.y = ev.y;
        data->state = ev.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        data->continue_reading = more;      /* 同一轮内把积压的转换读完 */
        return;
    }
    board_touch_status_t status = board_adapter_touch_snapshot();   /* 拖动取最新坐标 */
    data->point.x = status.x;
    data->point.y = status.y;
    data->state = status.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void touch_task(void *arg)
{
    (void)arg;
    for (;;) {
        touch_sample();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static uint32_t lvgl_tick_get_cb(void)
{
    /* LVGL 时基（毫秒）。用 esp_timer 而非 FreeRTOS tick，避免 tick rate 变化影响。 */
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void lvgl_task(void *arg)
{
    for (;;) {
        /* 与截图/注入命令互斥：LVGL 非线程安全（M0 实测对象树损坏） */
        board_adapter_lvgl_lock(portMAX_DELAY);
        uint32_t until = lv_task_handler();
        board_adapter_lvgl_unlock();
        vTaskDelay(pdMS_TO_TICKS(until < 2 ? 2 : (until > 30 ? 30 : until)));
    }
}

static esp_err_t board_i2c_init(void)
{
    if (s_i2c) return ESP_OK;
    i2c_master_bus_config_t cfg = {
        .i2c_port = -1,
        .sda_io_num = BSP_I2C_SDA,
        .scl_io_num = BSP_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&cfg, &s_i2c);
}

static esp_err_t board_backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_1,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "bl timer");
    const ledc_channel_config_t ch = {
        .gpio_num = BSP_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_1,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_1,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ch), TAG, "bl ch");
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 800);   /* 80% 亮度 */
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
    return ESP_OK;
}

#ifndef BOARD_STAGE
#define BOARD_STAGE 9   /* 二分：1=i2c 2=+spi 3=+panel io 4=+panel init 5=+ops 6=+LVGL 9=全量 */
#endif

int board_adapter_init_display(void)
{
    /* 1) I2C（触摸依赖） */
#if BOARD_STAGE != 2   /* 二分实验：stage2 跳过 I2C，隔离 SPI 的影响 */
    ESP_RETURN_ON_ERROR(board_i2c_init(), TAG, "i2c");
#endif

#if BOARD_STAGE < 2
    ESP_LOGI(TAG, "stage 1 done（仅 I2C）");
    return ESP_OK;
#endif
#if BOARD_STAGE >= 2

    /* 2) SPI 总线 */
    spi_bus_config_t buscfg = {
        .mosi_io_num = BSP_LCD_SPI_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = BSP_LCD_SPI_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BOARD_LCD_W * LVGL_BUF_LINES * 2,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO), TAG, "spi");


#if BOARD_STAGE < 3
    ESP_LOGI(TAG, "stage 2 done（i2c+spi，无 DMA）");
    return ESP_OK;
#endif

    /* 3) 面板 IO（SPI mode 3，命令/参数 8bit，微雪同配置） */
    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = BSP_LCD_DC,
        .cs_gpio_num = BSP_LCD_SPI_CS,
        .pclk_hz = BSP_LCD_PCLK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 3,
        .trans_queue_depth = 10,
        .on_color_trans_done = on_color_trans_done,
    };
    if (!s_flush_done) s_flush_done = xSemaphoreCreateCounting(8, 0);
    if (!s_panel_mutex) s_panel_mutex = xSemaphoreCreateMutex();
    if (!s_flush_done || !s_panel_mutex) return ESP_ERR_NO_MEM;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,
                                                 &io_config, &s_lcd_io), TAG, "io");

    /* 4) ST7796S 面板（BGR + 反色 + 横屏变换，与微雪 BSP 一致） */
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BSP_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7796(s_lcd_io, &panel_config, &s_lcd), TAG, "panel");
#if BOARD_STAGE >= 4
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_lcd), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_lcd), TAG, "init");
#endif
#if BOARD_STAGE >= 5
    esp_lcd_panel_invert_color(s_lcd, true);
    /* 原生 320×480 竖屏 → 480×320 横屏（产品 UI 坐标系）；
     * 若实机方向颠倒，调整 mirror 组合并同步触摸 flags */
    esp_lcd_panel_swap_xy(s_lcd, true);
    esp_lcd_panel_mirror(s_lcd, true, true);
    esp_lcd_panel_disp_on_off(s_lcd, true);
#endif

#if BOARD_STAGE >= 6
    /* 5) LVGL 显示（部分刷新双缓冲，内部 DMA 缓冲；flush 内字节交换后异步发送） */
    if (!s_lvgl_mutex) {
        s_lvgl_mutex = xSemaphoreCreateRecursiveMutex();
        if (!s_lvgl_mutex) return ESP_ERR_NO_MEM;
    }
    lv_init();   /* LVGL 核心初始化——必须先于任何 lv_* 调用（M0 实测踩坑） */
    /* 时基：没有它 LVGL 定时器（indev 读取/动画/刷新）永不触发（M0 实测踩坑） */
    lv_tick_set_cb(lvgl_tick_get_cb);
    s_flush_a = heap_caps_malloc(BOARD_LCD_W * LVGL_BUF_LINES * 2,
                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    s_flush_b = heap_caps_malloc(BOARD_LCD_W * LVGL_BUF_LINES * 2,
                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    s_blit_buf[0] = heap_caps_malloc(BOARD_LCD_W * LVGL_BUF_LINES * 2,
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    s_blit_buf[1] = heap_caps_malloc(BOARD_LCD_W * LVGL_BUF_LINES * 2,
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!s_flush_a || !s_flush_b || !s_blit_buf[0] || !s_blit_buf[1]) return ESP_ERR_NO_MEM;
    s_disp = lv_display_create(BOARD_LCD_W, BOARD_LCD_H);
    if (!s_disp) return ESP_FAIL;
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_set_flush_wait_cb(s_disp, flush_wait_cb);
    lv_display_set_buffers(s_disp, s_flush_a, s_flush_b, BOARD_LCD_W * LVGL_BUF_LINES * 2,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
#else
    return ESP_OK;
#endif

    /* 6) Keep FT5x06 data native; touch_read_cb applies the panel mapping.
     * Driver mirrors before swapping, so landscape maxima in its flags can
     * underflow portrait coordinates. Do not apply a second rotation there. */
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = BOARD_LCD_H,
        .y_max = BOARD_LCD_W,
        .rst_gpio_num = BSP_TP_RST,
        .int_gpio_num = BSP_TP_INT,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
    };
    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    tp_io_config.scl_speed_hz = BSP_I2C_SPEED_HZ;
    esp_err_t tp_err = esp_lcd_new_panel_io_i2c(s_i2c, &tp_io_config, &tp_io);
    if (tp_err == ESP_OK) {
        tp_err = esp_lcd_touch_new_i2c_ft5x06(tp_io, &tp_cfg, &s_tp);
    }
    if (tp_err == ESP_OK) {
        s_indev = lv_indev_create();
        if (!s_indev) return ESP_ERR_NO_MEM;
        lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(s_indev, touch_read_cb);
        lv_indev_set_display(s_indev, s_disp);
        /* 采样与渲染解耦：高于 LVGL 任务的优先级、固定 100 Hz */
        if (xTaskCreate(touch_task, "touch", 3072, NULL, LVGL_TASK_PRIO + 1, NULL) != pdPASS)
            return ESP_ERR_NO_MEM;
        ESP_LOGI(TAG, "touch ready (FT5x06, 100 Hz sampling)");
    } else {
        ESP_LOGW(TAG, "touch init failed: %d（UI 无触摸输入，其余功能不受影响）", tp_err);
    }

    /* 7) 背光点亮（最后开，避免初始化花屏可见） */
    ESP_RETURN_ON_ERROR(board_backlight_init(), TAG, "backlight");

    /* 8) LVGL 任务 + 预览画布（PSRAM） */
    s_canvas_buf = heap_caps_malloc(BOARD_LCD_W * BOARD_LCD_H * 2, MALLOC_CAP_SPIRAM);
    s_status.display_ready = true;
    ESP_LOGI(TAG, "display ready: ST7796S 480x320 landscape + LVGL9%s",
             s_tp ? " + touch" : "");
    return ESP_OK;
#endif   /* BOARD_STAGE >= 6 */
}

/* =====================================================================
 * TF 卡（SDMMC 四线）
 * ===================================================================== */

int board_adapter_init_sd(void)
{
    /* 微雪 BSP：TF 在 SDMMC 槽 0（IO MUX 引脚），卡电源由片上 LDO 通道 4 供给。
     * 注意：esp_hosted 的 C6 SDIO 亦用 SDMMC 控制器（先建立，本挂载复用其句柄）。
     * TF 文件 IO 与 Wi-Fi 流量的共存需在 M0 压力用例中持续验证。 */
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;        /* 40MHz */

    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = 4,
    };
    sd_pwr_ctrl_handle_t pwr_ctrl = NULL;
    ESP_RETURN_ON_ERROR(sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &pwr_ctrl), TAG, "ldo");
    host.pwr_ctrl_handle = pwr_ctrl;

    const sdmmc_slot_config_t slot = {
        .cd = SDMMC_SLOT_NO_CD,
        .wp = SDMMC_SLOT_NO_WP,
        .width = 4,
        .flags = 0,
    };

    esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,             /* 绝不自动格式化（产品文档 6.2） */
        .max_files = 5,
        .allocation_unit_size = 64 * 1024,
    };
    esp_err_t err = esp_vfs_fat_sdmmc_mount(s_sd_mount, &host, &slot, &mount, &s_sd_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sd mount failed: %s（请检查 TF 卡是否插入/格式化为 FAT32）",
                 esp_err_to_name(err));
        s_status.sd_ready = false;
        return err;
    }
    s_status.sd_ready = true;
    uint64_t mb = (uint64_t)s_sd_card->csd.capacity * s_sd_card->csd.sector_size >> 20;
    ESP_LOGI(TAG, "sd mounted at %s: %s, %llu MB, %d kHz", s_sd_mount, s_sd_card->cid.name,
             (unsigned long long)mb, s_sd_card->csd.tr_speed / 1000);

    /* 文件 IO 冒烟：写入-回读-删除（验证 FAT + 驱动链路真实可写） */
    char smoke[64];
    snprintf(smoke, sizeof(smoke), "%.32s/.storycam_smoke", s_sd_mount);
    FILE *f = fopen(smoke, "wb");
    if (!f) { ESP_LOGW(TAG, "sd smoke: create failed"); return ESP_FAIL; }
    const char *msg = "story-camera-tf-ok";
    size_t need = strlen(msg) + 1;
    if (fwrite(msg, 1, need, f) != need || fflush(f) != 0 || fsync(fileno(f)) != 0) {
        fclose(f); ESP_LOGW(TAG, "sd smoke: write failed"); return ESP_FAIL;
    }
    fclose(f);
    uint8_t rbuf[32] = { 0 };
    f = fopen(smoke, "rb");
    if (!f) { ESP_LOGW(TAG, "sd smoke: reopen failed"); return ESP_FAIL; }
    size_t rn = fread(rbuf, 1, sizeof(rbuf) - 1, f);
    fclose(f);
    unlink(smoke);
    if (rn != need || memcmp(rbuf, msg, need) != 0) {
        ESP_LOGW(TAG, "sd smoke: readback mismatch");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "sd smoke ok（写入/回读/删除通过）");
    return ESP_OK;
}

/* =====================================================================
 * 音频（ES8311 + 功放）
 * ===================================================================== */

int board_adapter_speaker_enable(bool enable)
{
#if CONFIG_STORY_DEBUG_SILENT
    (void)enable;
    return gpio_set_level(BSP_PA, 0);          /* 静音调试构建：功放永远关闭 */
#else
    /* The codec configured this pin once. Reconfiguring it on every write
     * claims GPIO53 repeatedly and masks genuine ownership conflicts. */
    return gpio_set_level(BSP_PA, enable ? 1 : 0);
#endif
}

int board_adapter_speaker_level(void)
{
    /* Enable pad readback without changing the amplifier output level. */
    if (gpio_input_enable(BSP_PA) != ESP_OK) return -1;
    return gpio_get_level(BSP_PA);
}

static esp_err_t board_audio_init(void)
{
    /* I2S 标准模式主设备（立体声槽位；MCLK 256fs 自 MCLK 引脚） */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_i2s_tx, NULL), TAG, "i2s chan");
    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(24000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_BCLK,
            .ws = BSP_I2S_WS,
            .dout = BSP_I2S_DOUT,
            .din = BSP_I2S_DIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    std.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s_tx, &std), TAG, "i2s std");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_tx), TAG, "i2s en");

    /* ES8311（esp_codec_dev），PA 由 codec 驱动按播放状态自动开关 */
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = 0,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = s_i2c,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!ctrl_if) return ESP_FAIL;
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_TYPE_OUT,
#if CONFIG_STORY_DEBUG_SILENT
        .pa_pin = -1,                 /* 静音调试：codec 不接管功放脚，由本组件常拉低 */
#else
        .pa_pin = BSP_PA,
#endif
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        /* U12 VDD is ESP_3V3 in the project's board schematic. */
        .hw_gain = { .pa_voltage = 3.3, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    if (!codec_if) return ESP_FAIL;
    audio_codec_i2s_cfg_t i2s_data_cfg = {
        .port = I2S_NUM_0,
        .tx_handle = s_i2s_tx,
        .rx_handle = NULL,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_data_cfg);
    if (!data_if) return ESP_FAIL;
    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_codec = esp_codec_dev_new(&dev_cfg);
    if (!s_codec) return ESP_FAIL;
    /* 默认曲线 0→-50 dB、100→0 dB 线性：音量 30 只有 -35 dB，用户反馈"声音小"。
     * 换成按语音听感分段的曲线；最大值仍为 0 dB（后端已响度归一，不再加增益防削波）。 */
    static esp_codec_dev_vol_map_t vol_points[] = {
        { 0, -95.5f }, { 1, -40.0f }, { 30, -20.0f }, { 60, -9.0f }, { 100, 0.0f },
    };
    esp_codec_dev_vol_curve_t curve = {
        .vol_map = vol_points,
        .count = sizeof(vol_points) / sizeof(vol_points[0]),
    };
    if (esp_codec_dev_set_vol_curve(s_codec, &curve) != ESP_CODEC_DEV_OK)
        ESP_LOGW(TAG, "volume curve not applied; using library default");
    return ESP_OK;
}

/* 上电提示音：验证 codec/PA/I2S 链路（约 1s，双音，音量 40） */
static __attribute__((unused)) esp_err_t board_audio_boot_chime(void)
{
    if (!s_codec) return ESP_ERR_INVALID_STATE;
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = 24000,
        .channel = 2,
        .bits_per_sample = 16,
    };
    if (esp_codec_dev_open(s_codec, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "codec open failed（音频链路不可用，后续播放将失败）");
        return ESP_FAIL;
    }
    if (esp_codec_dev_set_out_vol(s_codec, 40) != ESP_CODEC_DEV_OK) return ESP_FAIL;
    /* 分块生成写入：避免 96KB 静态缓冲永久占用内部 RAM
     * （曾致硬件 AES 分配失败、TLS 全部失败，实机 2026-09-13） */
    enum { HALF = 12000, CHUNK = 1200 };      /* 每音 0.5s，共 1s，50ms 分块 */
    static int16_t buf[CHUNK * 2];            /* 4.8KB 静态（曾用 96KB 静态/
                                               栈上 9.6KB：前者耗尽内部 RAM，
                                               后者溢出 main 栈，均实机复现） */
    const float freqs[2] = { 392.0f, 523.25f };   /* G4 → C5 */
    for (int h = 0; h < 2; h++) {
        for (int off = 0; off < HALF; off += CHUNK) {
            for (int i = 0; i < CHUNK; i++) {
                float t = (float)(off + i) / 24000.0f;
                float env = 0.85f * sinf(3.1415926f * t / 0.5f);   /* 半正弦包络 */
                int16_t s = (int16_t)(9000 * env * sinf(2 * 3.1415926f * freqs[h] * t));
                buf[i * 2] = s;
                buf[i * 2 + 1] = s;
            }
            if (esp_codec_dev_write(s_codec, buf, sizeof(buf)) != ESP_CODEC_DEV_OK) return ESP_FAIL;
        }
    }
    /* PCM submission does not prove that the physical speaker is audible. */
    ESP_LOGI(TAG, "audio chime PCM submitted; physical sound requires listening check");
    return ESP_OK;
}

/* =====================================================================
 * 相机（esp_video + OV5647 探测）
 * ===================================================================== */

int board_adapter_init_camera(const struct board_camera_config *cfg)
{
    (void)cfg;
    ESP_RETURN_ON_ERROR(board_i2c_init(), TAG, "i2c");

    esp_video_init_csi_config_t csi = {
        .sccb_config = {
            .init_sccb = false,
            .i2c_handle = s_i2c,
            .freq = BSP_I2C_SPEED_HZ,
        },
        .reset_pin = -1,
        .pwdn_pin = -1,
    };
    esp_video_init_config_t cam_cfg = {
        .csi = &csi,
    };
    esp_err_t err = esp_video_init(&cam_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_video init failed: %d", err);
        s_status.camera_ready = false;
        return err;
    }
    /* 打开 CSI 设备读能力，验证传感器识别 */
    int fd = open("/dev/video0", O_RDONLY);
    if (fd < 0) {
        ESP_LOGW(TAG, "open /dev/video0 failed: errno %d（摄像头未接或传感器未识别）", errno);
        return ESP_ERR_NOT_FOUND;
    }
    struct v4l2_capability cap;
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
        ESP_LOGI(TAG, "camera: card=%s driver=%s bus=%s",
                 (char *)cap.card, (char *)cap.driver, (char *)cap.bus_info);
    }
    close(fd);
    s_status.camera_ready = true;
    /* 探测结果：ISP pipeline 是否初始化（AGC/AWB 依赖它） */
    extern bool esp_video_isp_pipeline_is_initialized(void);
    ESP_LOGI(TAG, "camera ready; ISP pipeline initialized=%d",
             (int)esp_video_isp_pipeline_is_initialized());
    return ESP_OK;
}

void board_adapter_camera_deinit(void)
{
    s_status.camera_ready = false;
}

/* =====================================================================
 * 便捷组合初始化（供 app_context 简化调用；各函数仍可单独调用）
 * ===================================================================== */

void board_adapter_lvgl_start(void)
{
    if (xTaskCreate(lvgl_task, "lvgl", LVGL_TASK_STACK, NULL, LVGL_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "lvgl task create failed");
    }
}

int board_adapter_init_audio(void)
{
    ESP_RETURN_ON_ERROR(board_i2c_init(), TAG, "i2c");
#if CONFIG_STORY_DEBUG_SILENT
    /* 先于任何音频初始化把功放关死，codec 初始化期间也不会出声 */
    const gpio_config_t pa_cfg = { .pin_bit_mask = 1ULL << BSP_PA, .mode = GPIO_MODE_INPUT_OUTPUT };
    gpio_config(&pa_cfg);
    gpio_set_level(BSP_PA, 0);
    ESP_LOGW(TAG, "*** SILENT DEBUG BUILD: amplifier forced off, DAC muted, no boot chime ***");
#endif
    esp_err_t err = board_audio_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "audio init failed: %d（播放不可用，其余功能不受影响）", err);
        return err;
    }
#if CONFIG_STORY_DEBUG_SILENT
    s_status.audio_ready = true;
    return ESP_OK;
#endif
    err = board_audio_boot_chime();
    if (err != ESP_OK) {
        board_adapter_speaker_enable(false);
        esp_codec_dev_close(s_codec);
        ESP_LOGW(TAG, "audio chime PCM submission failed: %d", err);
        return err;
    }
    s_status.audio_ready = true;
    return ESP_OK;
}

void board_adapter_report_fault(const char *who, int err)
{
    ESP_LOGE(TAG, "fault: %s err=0x%x", who, err);
}
