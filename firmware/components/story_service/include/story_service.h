#pragma once
/**
 * @file story_service.h
 * @brief 业务协调（实施计划 3/5.3/6.3/6.4）。
 *
 * 并发规则：
 * - 首版同时只允许一个生成操作或一个播放操作；
 * - 生成期间不接收新的快门/删除/重新生成请求；
 * - 返回/取消：自动播放令牌失效 -> 停止本机轮询/下载 -> 保留记录；
 *   服务器继续执行是允许情况。
 */

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "storage_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 操作状态（实施计划 6.3） */
typedef enum {
    OP_IDLE = 0,
    OP_CAPTURE_SAVING,       /* 拍照与落盘进行中（工作线程） */
    OP_CAPTURE_DONE,         /* 照片已保存（未自动进入生成：离线/未确认） */
    OP_WAITING_NETWORK,
    OP_NEEDS_CONFIRMATION,   /* 首次联网说明未确认 */
    OP_UPLOADING,
    OP_REMOTE_PENDING,
    OP_DOWNLOADING,
    OP_COMMITTING,
    OP_COMPLETED,
    OP_WAITING_USER,         /* 离开等待/超时：保留 job 信息 */
    OP_FAILED,
    OP_CONTENT_REJECTED,
} operation_state_t;

/** 生成阶段事件：UI 按 story_id+generation 核对，迟到事件丢弃 */
typedef struct {
    char story_id[STORAGE_ID_HEX];
    char operation_id[STORAGE_ID_HEX];
    operation_state_t state;
    int attempt;
    int revision;
    char error_code[32];
    bool error_retryable;
    char stage[24];
    uint32_t sequence;
    uint32_t autoplay_token;
    bool has_summary;
    story_summary_t summary;
} story_event_t;

typedef void (*story_event_cb)(const story_event_t *ev, void *user);

esp_err_t story_service_init(story_event_cb cb, void *user);

/* ---------- 拍摄（产品文档 6.1） ---------- */
/** 异步拍照：立即返回，过程经事件 OP_CAPTURE_SAVING -> OP_CAPTURE_DONE
 *  （或 OP_FAILED）通知。捕获/编码/写盘全部在后台任务，不阻塞 LVGL。
 *  成功保存且“已确认联网”时自动进入生成流程（在线拍照自动生成）。 */
esp_err_t story_capture_async(void);

/** 同步版本（测试/调试用）：保存原图 + 锚点 + 缩略图后才返回成功。 */
esp_err_t story_capture_and_save(story_summary_t *out_summary);

/* ---------- 生成（实施计划 6.2/6.4） ---------- */
/** mode=generate 首次；regenerate 读取原图，不重新拍照。
 *  consent_version 未确认 -> NEEDS_CONFIRMATION，允许离线保存与回听。 */
esp_err_t story_start_generation(const char *story_id, bool regenerate);
bool story_generation_active(void);

/** 用户离开等待：令牌失效 + 停止本机轮询；云端继续执行被允许。 */
void story_cancel_wait(void);
bool story_claim_autoplay(uint32_t token);

/** 显式继续：读取持久化 operation，先查原任务（by-operation/job），
 *  仅确认从未提交（404）才按原 ID/原字节重发 POST；不重复新建。 */
esp_err_t story_resume_generation(const char *story_id);

/** 家长确认首次联网说明：持久化 consent 版本，允许后续在线生成。 */
esp_err_t story_confirm_consent(void);

/* ---------- 播放 ---------- */
esp_err_t story_play(const char *story_id);     /* 离开播放页时 UI 调 story_stop_playback */
void story_stop_playback(void);
esp_err_t story_delete(const char *story_id);   /* 先持久删除意图，再分批清理 */
/** 批量删除（阻塞，每条约数百毫秒，须在后台任务调用）：逐条写删除意图，每条完成回调 progress，
 *  最后统一启动清理。*deleted 为成功条数；生成/拍照进行中返回 ESP_ERR_INVALID_STATE。 */
typedef void (*story_delete_progress_cb)(int done, int total, void *user);
esp_err_t story_delete_many(const char (*ids)[STORAGE_ID_HEX], int n,
                            story_delete_progress_cb progress, void *user, int *deleted);

#ifdef __cplusplus
}
#endif
