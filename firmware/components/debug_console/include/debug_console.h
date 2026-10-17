#pragma once
/**
 * @file debug_console.h
 * @brief UART-C 调试命令行（M0 实机诊断用，协议与项目参考工具 device.py 兼容）。
 *
 * 命令（单行文本，\n 结尾）：
 *   info                     一行状态汇总
 *   screenshot               LVGL 场景快照（RGB565 base64 回传，见 device.py）
 *   tap <x> <y>              模拟点击（press 100ms 后 release）
 *   touch <x> <y> <0|1>      模拟按下/松开（可组合模拟滑动）
 *   ui-reset                 回到开机页
 *
 * 注意：截图是 LVGL 界面快照，不是摄像头拍照；模拟触摸验证界面逻辑，
 * 不能替代实体触摸屏灵敏度/准确性验收。
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 启动调试控制台任务（UART0 + USB Serial/JTAG 双通道）。 */
void debug_console_start(void);

#ifdef __cplusplus
}
#endif
