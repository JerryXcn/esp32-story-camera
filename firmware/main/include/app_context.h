#pragma once
/**
 * @file app_context.h
 * @brief 应用组装：启动顺序（实施计划 9.1）与全局状态发布。
 *
 * 启动顺序：NVS/板级 -> LCD/触摸 + 最小 UI -> TF/相机/音频 -> 网络后台 ->
 * 索引展示与后台恢复。显示/相机/网络就绪分别发布，无网络不挡 UI，
 * 无卡不启用快门，相机失败不挡故事库。
 */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_STAGE_BOARD = 0,     /**< NVS + 系统服务 */
    APP_STAGE_UI,            /**< 显示与触摸就绪（可操作 UI） */
    APP_STAGE_STORAGE,       /**< TF 就绪（决定快门可用性） */
    APP_STAGE_CAMERA,        /**< 预览稳定 */
    APP_STAGE_AUDIO,
    APP_STAGE_NETWORK,       /**< 后台连接，不阻塞 */
    APP_STAGE_RECOVER,       /**< 后台索引恢复 */
} app_stage_t;

typedef void (*app_stage_cb)(app_stage_t stage, bool ok, void *user);

void app_context_set_stage_cb(app_stage_cb cb, void *user);

/** 按依赖顺序执行启动；网络阶段非阻塞返回。 */
void app_context_boot(void);

#ifdef __cplusplus
}
#endif
