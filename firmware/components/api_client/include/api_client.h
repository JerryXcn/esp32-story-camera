#pragma once
/**
 * @file api_client.h
 * @brief 云端任务 API 客户端（实施计划第 7 节唯一 V1 契约，设备侧）。
 *
 * - Bearer 设备令牌 + Idempotency-Key（= operation_id）
 * - 传输超时按 7.5：建连 10s、上传 30s、轮询起步 2s、下载 30s
 * - 证书校验永远开启；失败不允许通过关闭校验恢复
 * - 响应只接受本服务约定的 audio 路径，禁止响应注入任意 URL
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define API_OP_ID_LEN 33
#define API_JOB_ID_LEN 48
#define API_ERR_CODE_LEN 32

typedef struct {
    char job_id[API_JOB_ID_LEN];
    char operation_id[API_OP_ID_LEN];
    char status[16];        /* queued/running/succeeded/failed/rejected/expired */
    char stage[24];         /* queued/understanding/story_generating/tts_generating/ready */
    int attempt;
    int revision;
    int poll_after_ms;
    bool has_error;
    char error_code[API_ERR_CODE_LEN];
    bool error_retryable;
} api_job_status_t;

typedef struct {
    char title[64];
    char *story;            /* 调用方释放；UTF-8 ≤12KiB */
    size_t story_len;
    bool has_audio;
    char audio_path[API_JOB_ID_LEN + 32];  /* 仅接受 /v1/jobs/<job_id>/audio */
    uint32_t duration_ms;
    size_t audio_bytes;
    char audio_sha256[65];
} api_result_t;

esp_err_t api_client_init(const char *base_url);

/** 连通性测试：GET <base_url>/v1/health（与正式请求同一套 TLS 校验），阻塞至多约 10 秒。
 *  ESP_OK = 能连上且服务正常；连接/TLS 失败返回对应错误；响应不对返回 ESP_ERR_INVALID_RESPONSE。 */
esp_err_t api_client_probe(const char *base_url);

/** POST /v1/jobs（multipart）。image_path 为已持久化的 ai_input.jpg。
 *  out 填充 202 响应；HTTP 409/422 等按 out_http 返回并解析 error 信封。 */
esp_err_t api_post_job(const char *operation_id, const char *story_id,
                       const char *mode, int base_version,
                       const char *age_group, const char *story_length,
                       const char *voice_id, const char *language,
                       const char *image_path, const char *image_sha256_hex,
                       api_job_status_t *out, int *out_http);

/** GET /v1/jobs/by-operation/{op}（POST 响应丢失/断电恢复） */
esp_err_t api_get_by_operation(const char *operation_id, api_job_status_t *out);

/** GET /v1/jobs/{job_id} */
esp_err_t api_get_job(const char *job_id, api_job_status_t *out);

/** GET /v1/jobs/{job_id}/result。story 缓冲由调用方 free。 */
esp_err_t api_get_result(const char *job_id, api_result_t *out);

/** POST /v1/jobs/{job_id}/retry，Idempotency-Key = recovery_command_id */
esp_err_t api_retry(const char *job_id, int expected_attempt,
                    const char *command_id, api_job_status_t *out);

/** GET /v1/jobs/{job_id}/audio：流式下载并计算 SHA-256/长度。
 *  校验失败返回 ESP_ERR_INVALID_CRC，保留旧版本不提交。 */
esp_err_t api_download_audio(const char *job_id, const char *expected_sha256,
                             size_t expected_bytes, const char *dest_path);

void api_result_free(api_result_t *r);

#ifdef __cplusplus
}
#endif
