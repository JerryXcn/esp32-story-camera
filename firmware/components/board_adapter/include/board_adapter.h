#pragma once
/**
 * @file board_adapter.h
 * @brief 板卡适配层（实施计划 2.2 / 4.1）。
 *
 * 显示、触摸、相机、音频功放等硬件差异全部收敛在这里。
 * 具体引脚/驱动句柄必须在 M0 用实机测量后冻结（实施计划第 2 节）：
 * 本头文件只定义应用层依赖的能力接口与逻辑坐标约定（480x320 横屏）。
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOARD_LCD_W 480
#define BOARD_LCD_H 320

/** 板卡上电后各外设的就绪状态（实施计划 9.1：分阶段发布） */
typedef struct {
    bool display_ready;   /**< LCD + 触摸可用 */
    bool sd_ready;        /**< TF 挂载成功 */
    bool camera_ready;    /**< 预览稳定且快门可用 */
    bool audio_ready;     /**< codec/I2S 就绪 */
} board_status_t;

/** 初始化显示与触摸（横屏一次旋转，触摸映射同一坐标）。
 *  返回 0 成功；触摸与显示不一致属于 M0 放行门槛，不允许双重旋转。 */
int board_adapter_init_display(void);

/** 初始化 TF（FAT32，挂载点由 BSP 决定，返回逻辑根路径）。失败不重试格式化。 */
int board_adapter_init_sd(void);
const char *board_adapter_sd_mount_point(void);

/** 初始化相机并启动 320x240 预览（实施计划 5.1 初始契约）。
 *  预览帧通过 frame_cb 交付，缓冲所有权归 camera_service。 */
struct board_camera_config {
    int preview_w;
    int preview_h;
};
int board_adapter_init_camera(const struct board_camera_config *cfg);
void board_adapter_camera_deinit(void);

/** 功放使能（避免爆音：先静音再上电，播放链路负责时序）。 */
int board_adapter_speaker_enable(bool enable);
/** 功放 GPIO53 的 pad 电平读回（不代表已经实际出声）。 */
int board_adapter_speaker_level(void);

/** 直接读写相机 sensor(SCCB 0x36) 寄存器（调试用，16 位寄存器地址）。 */
int board_adapter_cam_reg_read(uint16_t reg, uint8_t *val);
int board_adapter_cam_reg_write(uint16_t reg, uint8_t val);

/** LVGL 显示句柄（调试/注入输入设备绑定用）。 */
void *board_adapter_display(void);

/** 最近一次 LVGL 实体触摸采样；诊断不再读取/消耗控制器数据。 */
bool board_adapter_touch_poll(uint16_t *x, uint16_t *y);

typedef struct {
    bool initialized;
    bool pressed;
    uint16_t raw_x, raw_y;
    uint16_t x, y;
    uint32_t polls, read_errors, invalid_points;
    uint32_t sample_ms;
    int last_error;
} board_touch_status_t;
board_touch_status_t board_adapter_touch_snapshot(void);
/** 临时记录实体触摸状态及坐标；默认关闭，移动日志限为每 100ms 一条。 */
void board_adapter_touch_trace(bool enable);

/** 各外设就绪状态快照（调试/UI 使用）。 */
board_status_t board_adapter_status_snapshot(void);

/** LVGL 互斥：截图/注入等非渲染线程访问 LVGL 时必须成对调用。 */
bool board_adapter_lvgl_lock(uint32_t timeout_ms);
void board_adapter_lvgl_unlock(void);

/** 拍照页旁路直绘：开启后 LVGL 的输出不再上屏（只记录"界面有变化"），
 *  由调用方把相机画面与界面合成为 480x320 小端 RGB565 整帧，经 board_adapter_display_blit 写屏。
 *  开关须在持有 LVGL 锁时调用；关闭后调用方应整屏 invalidate 让 LVGL 重画。 */
void board_adapter_display_bypass(bool on);
bool board_adapter_display_bypass_active(void);
/** 旁路期间 LVGL 是否画过东西（界面变化），读取即清除 */
bool board_adapter_display_bypass_take_dirty(void);
/** 整帧写屏（阻塞到发送完成，约 31 ms）；未开启旁路时返回 ESP_ERR_INVALID_STATE */
esp_err_t board_adapter_display_blit(const void *rgb565_le);

/** 启动 LVGL 渲染任务。必须在所有初始屏幕构建完成之后调用
 *  （LVGL 非线程安全：初始化与渲染并发会损坏对象树，M0 实测）。 */
void board_adapter_lvgl_start(void);

/** 初始化 ES8311 音频链路并播放上电提示音（验证 codec/PA/I2S）。 */
int board_adapter_init_audio(void);

/** 拍摄页预览画布缓冲（PSRAM，480×320 RGB565）。 */
void *board_adapter_canvas_buffer(void);

/** ES8311 codec 句柄（esp_codec_dev），audio_service 播放用。 */
void *board_adapter_audio_codec(void);

/** 板级统一错误上报（诊断用，不直接弹 UI）。 */
void board_adapter_report_fault(const char *who, int err);

#ifdef __cplusplus
}
#endif
