/**
 * @file story_service.c
 * @brief 单任务编排实现（实施计划 6.4 写入与提交协议 / 7.5 超时与退避）。
 *
 * worker 生命周期（FLOW-01/03/05 修复）：
 *   持久化 operation（operations/<op>.json） -> staging/<op>/ai_input.jpg
 *   （复制原图 + 真实 SHA-256）-> POST（响应丢失查 by-operation，
 *   仅 404 才按原 ID/原字节重发）-> 轮询（poll_after + 抖动）
 *   -> result -> story.txt/audio.mp3 落 staging -> storage_commit_version
 *   （真实哈希提交）-> 仍持有自动播放令牌才播放。
 * 前台总等待 120s（单调时钟）；截止后转 WAITING_USER，不自动播放。
 *
 * 恢复语义（FLOW-05）：
 * - 显式继续 = 读取持久化 operation，job 已知则从结果/下载阶段继续；
 *   POST 从未成功则按原 ID/原字节重发（服务端幂等去重）。
 * - TTS 失败：文本已生成时先取回并落 staging 草稿（"故事已生成，
 *   声音待完成"），重试只补声音。
 * - 用户明确"重新生成"才创建新 operation（base_version 取当前已提交版本）。
 * 大缓冲一律堆分配；参数快照在操作创建时固化。
 */

#include "story_service.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"

#include "settings_service.h"
#include "network_service.h"
#include "camera_service.h"
#include "audio_service.h"
#include "api_client.h"
#include "system_service.h"

static const char *TAG = "story";

#define FOREGROUND_LIMIT_MS (120 * 1000)
#define AUTO_RETRY_MAX 2
/* TLS 握手 + HTTP 客户端调用深度需要 ≥16KB 任务栈（实机 2026-09-13：
 * 8.6KB 栈上 POST 必然连接失败/随机故障；大缓冲仍堆分配，不占栈） */
#define WORKER_STACK (20 * 1024)

static story_event_cb s_cb;
static void *s_cb_user;
static volatile bool s_generation_active;
static volatile bool s_capture_active;
static volatile uint32_t s_autoplay_token;   /* 0 = 已失效 */
static portMUX_TYPE s_token_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_token_counter, s_event_sequence;
static char s_active_story[STORAGE_ID_HEX];
static char s_active_operation[STORAGE_ID_HEX];
static char s_remote_stage[24];

static const char *AGE_STR[] = { "3-4", "5-6", "7-10" };
static const char *LEN_STR[] = { "short", "standard", "long" };

static uint32_t next_autoplay_token(void)
{
    portENTER_CRITICAL(&s_token_lock);
    if (++s_token_counter == 0) ++s_token_counter;
    s_autoplay_token = s_token_counter;
    uint32_t result = s_autoplay_token;
    portEXIT_CRITICAL(&s_token_lock);
    return result;
}
bool story_claim_autoplay(uint32_t token)
{
    portENTER_CRITICAL(&s_token_lock);
    bool allowed = token && token == s_autoplay_token;
    if (allowed) s_autoplay_token = 0;
    portEXIT_CRITICAL(&s_token_lock);
    return allowed;
}

static void publish(operation_state_t st, int attempt, int revision,
                    const char *err_code, bool retryable)
{
    story_event_t ev;
    memset(&ev, 0, sizeof(ev));
    strlcpy(ev.story_id, s_active_story, sizeof(ev.story_id));
    strlcpy(ev.operation_id, s_active_operation, sizeof(ev.operation_id));
    portENTER_CRITICAL(&s_token_lock);
    ev.sequence = ++s_event_sequence;
    ev.autoplay_token = s_autoplay_token;
    portEXIT_CRITICAL(&s_token_lock);
    if (st == OP_COMPLETED || st == OP_CAPTURE_DONE)
        ev.has_summary = storage_get_summary(ev.story_id, &ev.summary) == ESP_OK;
    strlcpy(ev.stage, s_remote_stage, sizeof(ev.stage));
    ev.state = st;
    ev.attempt = attempt;
    ev.revision = revision;
    ev.error_retryable = retryable;
    if (err_code) strlcpy(ev.error_code, err_code, sizeof(ev.error_code));
    if (s_cb) s_cb(&ev, s_cb_user);
}

/* ---------- 持久化 operation 记录（FLOW-05） ---------- */

typedef struct {
    char story_id[STORAGE_ID_HEX];
    char operation_id[STORAGE_ID_HEX];
    char job_id[API_JOB_ID_LEN];
    char mode[16];
    int base_version;
    char age_group[8];
    char story_length[12];
    int attempt;
    char state[24];                  /* 上次到达的本地阶段 */
    char error_code[API_ERR_CODE_LEN];
    bool has_draft;                  /* staging 已有 story.txt 草稿 */
} op_record_t;

static void op_record_fill(op_record_t *op, const char *story_id,
                           const char *operation_id, const char *mode,
                           int base_version, const product_settings_t *set)
{
    memset(op, 0, sizeof(*op));
    strlcpy(op->story_id, story_id, sizeof(op->story_id));
    strlcpy(op->operation_id, operation_id, sizeof(op->operation_id));
    strlcpy(op->mode, mode, sizeof(op->mode));
    op->base_version = base_version;
    strlcpy(op->age_group, AGE_STR[set->age_group], sizeof(op->age_group));
    strlcpy(op->story_length, LEN_STR[set->story_length], sizeof(op->story_length));
    op->attempt = 1;
    strlcpy(op->state, "created", sizeof(op->state));
}

static esp_err_t op_save(const op_record_t *op)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "operation_id", op->operation_id);
    cJSON_AddStringToObject(o, "story_id", op->story_id);
    if (op->job_id[0]) cJSON_AddStringToObject(o, "job_id", op->job_id);
    cJSON_AddStringToObject(o, "mode", op->mode);
    cJSON_AddNumberToObject(o, "base_version", op->base_version);
    cJSON_AddStringToObject(o, "age_group", op->age_group);
    cJSON_AddStringToObject(o, "story_length", op->story_length);
    cJSON_AddNumberToObject(o, "attempt", op->attempt);
    cJSON_AddStringToObject(o, "state", op->state);
    if (op->error_code[0]) cJSON_AddStringToObject(o, "error_code", op->error_code);
    cJSON_AddBoolToObject(o, "has_draft", op->has_draft);
    char *str = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!str) return ESP_ERR_NO_MEM;
    esp_err_t err = storage_write_operation(op->story_id, op->operation_id,
                                            str, strlen(str));
    cJSON_free(str);
    return err;
}

static bool op_load(const char *story_id, const char *operation_id, op_record_t *op)
{
    char buf[2048];
    if (storage_read_operation(story_id, operation_id, buf, sizeof(buf)) != ESP_OK) {
        return false;
    }
    cJSON *o = cJSON_Parse(buf);
    if (!o) return false;
    memset(op, 0, sizeof(*op));
    bool ok = false;
    cJSON *jid = cJSON_GetObjectItem(o, "job_id");
    cJSON *opid = cJSON_GetObjectItem(o, "operation_id");
    cJSON *sid = cJSON_GetObjectItem(o, "story_id");
    if (cJSON_IsString(opid) && cJSON_IsString(sid) &&
        strlen(opid->valuestring) == 32 && strlen(sid->valuestring) == 32) {
        strlcpy(op->operation_id, opid->valuestring, sizeof(op->operation_id));
        strlcpy(op->story_id, sid->valuestring, sizeof(op->story_id));
        if (cJSON_IsString(jid)) strlcpy(op->job_id, jid->valuestring, sizeof(op->job_id));
#define OP_STR(field, name) do { \
        cJSON *it = cJSON_GetObjectItem(o, name); \
        if (cJSON_IsString(it)) strlcpy(field, it->valuestring, sizeof(field)); \
    } while (0)
        OP_STR(op->mode, "mode");
        OP_STR(op->age_group, "age_group");
        OP_STR(op->story_length, "story_length");
        OP_STR(op->state, "state");
        OP_STR(op->error_code, "error_code");
#undef OP_STR
        cJSON *bv = cJSON_GetObjectItem(o, "base_version");
        if (cJSON_IsNumber(bv)) op->base_version = bv->valueint;
        cJSON *at = cJSON_GetObjectItem(o, "attempt");
        if (cJSON_IsNumber(at)) op->attempt = at->valueint;
        cJSON *dr = cJSON_GetObjectItem(o, "has_draft");
        op->has_draft = cJSON_IsTrue(dr);
        ok = true;
    }
    cJSON_Delete(o);
    return ok;
}

/* ---------- 拍照（工作线程） ---------- */

static esp_err_t capture_and_save_impl(const char *story_id, story_summary_t *out_summary)
{
    esp_err_t err;
    camera_capture_t cap;
    err = camera_capture_jpeg(&cap);
    if (err != ESP_OK) return err;                    /* 相机故障：禁止拍照 */

    /* 原图先落盘；损坏照片在此被拒绝，不产生半记录 */
    err = storage_commit_original(story_id, cap.jpeg, cap.jpeg_len, cap.jpeg_w, cap.jpeg_h);
    camera_release_jpeg();
    if (err != ESP_OK) return err;

    /* 缩略图落盘（失败不回滚原图，可重建） */
    storage_commit_thumb(story_id, cap.thumb_jpeg, cap.thumb_len);

    uint32_t seq = 0;
    err = settings_reserve_capture_seq(1, &seq);
    if (err != ESP_OK) return err;
    /* 真实 SHA-256 由存储层逐块重算（FLOW-01：零占位不可通过恢复校验） */
    uint8_t digest[32] = {0};
    err = storage_commit_capture_anchor(story_id, seq, digest);
    if (err != ESP_OK) {
        /* 原图已写但锚点失败：恢复器可校验孤立原图并补建，不自动上传 */
        ESP_LOGE(TAG, "anchor commit failed: %d", err);
        return err;
    }
    if (out_summary) {
        memset(out_summary, 0, sizeof(*out_summary));
        strlcpy(out_summary->story_id, story_id, sizeof(out_summary->story_id));
        out_summary->capture_seq = seq;
        out_summary->availability = STORY_AVAIL_PHOTO_ONLY;
        out_summary->has_time = system_time_is_synced();
        storage_get_thumb_path(story_id, out_summary->thumb_path,
                               sizeof(out_summary->thumb_path));
    }
    return ESP_OK;
}

static void capture_worker(void *arg)
{
    char *story_id = (char *)arg;      /* 堆分配的 33 字节 ID */

    strlcpy(s_active_story, story_id, sizeof(s_active_story));
    s_active_operation[0] = 0;
    publish(OP_CAPTURE_SAVING, 1, 1, NULL, false);
    story_summary_t summary;
    esp_err_t err = capture_and_save_impl(story_id, &summary);
    product_settings_t set;
    settings_load_product(&set);
    bool want_auto = (set.consent_version >= CONFIG_STORY_CONSENT_VERSION) &&
                     (network_state() >= NET_STATE_GOT_IP);
    bool auto_started = false;
    if (err == ESP_OK && want_auto) {
        auto_started = story_start_generation(story_id, false) == ESP_OK;
    }
    s_capture_active = false;
    if (err != ESP_OK) {
        const char *code = (err == ESP_ERR_INVALID_STATE) ? "CAMERA_FAILED"
                          : (err == ESP_ERR_NO_MEM) ? "STORAGE_FULL" : "SD_IO_ERROR";
        publish(OP_FAILED, 1, 1, code, false);
    } else if (!auto_started) {
        publish(OP_CAPTURE_DONE, 1, 1, NULL, false);
    } else {
        /* 生成事件由生成 worker 继续发布（UI 已切生成页） */
    }
    free(story_id);
    vTaskDelete(NULL);
}

esp_err_t story_capture_async(void)
{
    if (s_capture_active || s_generation_active) return ESP_ERR_INVALID_STATE;
    if (storage_precheck_space(false) != ESP_OK) return ESP_ERR_NO_MEM;

    char *story_id = malloc(STORAGE_ID_HEX);
    if (!story_id) return ESP_ERR_NO_MEM;
    if (storage_story_count() >= STORAGE_MAX_STORIES) { free(story_id); return ESP_ERR_NOT_SUPPORTED; }
    if (storage_new_story_id(story_id) != ESP_OK) { free(story_id); return ESP_FAIL; }
    s_capture_active = true;
    if (xTaskCreate(capture_worker, "capture", 8192, story_id, 6, NULL) != pdPASS) {
        free(story_id);
        s_capture_active = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t story_capture_and_save(story_summary_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (s_capture_active || s_generation_active) return ESP_ERR_INVALID_STATE;
    if (storage_story_count() >= STORAGE_MAX_STORIES) return ESP_ERR_NOT_SUPPORTED;
    if (storage_precheck_space(false) != ESP_OK) return ESP_ERR_NO_MEM;
    char id[STORAGE_ID_HEX];
    ESP_RETURN_ON_ERROR(storage_new_story_id(id), TAG, "story id");
    s_capture_active = true;
    esp_err_t err = capture_and_save_impl(id, out);
    s_capture_active = false;
    return err;
}

/* ---------- 生成 worker ---------- */

typedef struct {
    char story_id[STORAGE_ID_HEX];
    char operation_id[STORAGE_ID_HEX];
    bool regenerate;
    bool manual_resume;
    int base_version;
    char mode[16];
    uint32_t autoplay_token;
    op_record_t rec;                 /* 参数快照（FLOW-05：操作创建时固化） */
} gen_job_t;

static bool deadline_exceeded(int64_t deadline_us)
{
    return esp_timer_get_time() > deadline_us;
}

/** staging/<op>/ai_input.jpg：从原图复制（原图长边 ≤1280 满足契约 ≤1MiB），
 *  真实 SHA-256 逐块重算。统一走 storage_stage_write（ensure_dir 建目录，
 *  .part→rename 原子写）；恢复重试时重新复制相同字节，幂等键内容不变。 */
static esp_err_t prepare_upload_image(const gen_job_t *job, char *upload_path,
                                      size_t path_cap, char *sha_hex /* 65 */)
{
    char photo[256], dir[192];
    if (storage_get_photo_path(job->story_id, photo, sizeof(photo)) != ESP_OK) {
        ESP_LOGE(TAG, "ai_input: original.jpg missing");
        return ESP_ERR_NOT_FOUND;
    }
    char *slash = strrchr(photo, '/');
    if (!slash) return ESP_ERR_INVALID_STATE;
    /* dir = story 目录（photo 保持完整，指向 original.jpg 供读取） */
    snprintf(dir, sizeof(dir), "%.*s", (int)(slash - photo), photo);
    snprintf(upload_path, path_cap, "%.190s/staging/%.33s/ai_input.jpg",
             dir, job->operation_id);

    /* 原图 → 上传图：读入堆缓冲后经存储层原子写入（≤1MiB 契约上限） */
    FILE *src = fopen(photo, "rb");
    if (!src) {
        ESP_LOGE(TAG, "ai_input: fopen %s failed errno=%d", photo, errno);
        return ESP_ERR_NOT_FOUND;
    }
    fseek(src, 0, SEEK_END);
    long sz = ftell(src);
    fseek(src, 0, SEEK_SET);
    if (sz <= 0 || sz > 1024 * 1024) {
        fclose(src);
        ESP_LOGE(TAG, "ai_input: original size %ld out of contract", sz);
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *buf = malloc((size_t)sz);
    if (!buf) { fclose(src); return ESP_ERR_NO_MEM; }
    esp_err_t err = ESP_OK;
    if (fread(buf, 1, (size_t)sz, src) != (size_t)sz) {
        ESP_LOGE(TAG, "ai_input: short read of original");
        err = ESP_FAIL;
    }
    fclose(src);
    if (err == ESP_OK) {
        err = storage_stage_write(job->story_id, job->operation_id,
                                  "ai_input.jpg", buf, (size_t)sz);
        if (err != ESP_OK) ESP_LOGE(TAG, "ai_input stage write failed: %d", err);
    }
    free(buf);
    if (err != ESP_OK) return err;

    esp_err_t sha_err = storage_file_sha256(upload_path, sha_hex);
    if (sha_err != ESP_OK) {
        ESP_LOGE(TAG, "ai_input sha256 failed: %d", sha_err);
    }
    return sha_err;
}

static void generation_worker(void *arg)
{
    gen_job_t job;
    memcpy(&job, arg, sizeof(job));
    free(arg);

    op_record_t *rec = &job.rec;
    int attempt = rec->attempt ? rec->attempt : 1;
    int revision = 1;
    int64_t deadline = esp_timer_get_time() + FOREGROUND_LIMIT_MS * 1000LL;

    s_remote_stage[0] = 0;
    strlcpy(s_active_story, job.story_id, sizeof(s_active_story));
    strlcpy(s_active_operation, job.operation_id, sizeof(s_active_operation));
    bool resumed_with_job = rec->job_id[0] != '\0';   /* 恢复路径：跳过 POST */

    /* 上传图 + 真实 SHA-256（持久化后字节不再改变） */
    char upload_path[256];
    char sha_hex[65];
    esp_err_t err = prepare_upload_image(&job, upload_path, sizeof(upload_path), sha_hex);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "prepare upload failed: %s", esp_err_to_name(err));
        s_generation_active = false;
        publish(OP_FAILED, attempt, revision, "IMAGE_INVALID", false);
        vTaskDelete(NULL);
        return;
    }

    api_job_status_t st;
    memset(&st, 0, sizeof(st));
    int http = 0;
    if (resumed_with_job) {
        /* 恢复：直接查原 job 当前状态（不打扰云端任务，不重复计费） */
        publish(OP_REMOTE_PENDING, attempt, revision, NULL, false);
        err = api_get_job(rec->job_id, &st);
        if (err == ESP_ERR_NOT_FOUND && strcmp(rec->mode, "regenerate") == 0) {
            /* 极端：job 丢失。按用户显式"继续"语义重建 operation */
            resumed_with_job = false;
        }
    }
    if (!resumed_with_job) {
        /* 持久化 operation 快照后才发请求（实施计划 6.4 第 3 步） */
        publish(OP_UPLOADING, attempt, revision, NULL, false);
        if (op_save(rec) != ESP_OK) {
            publish(OP_FAILED, attempt, revision, "SD_IO_ERROR", true);
            s_generation_active = false; vTaskDelete(NULL); return;
        }
        ESP_LOGI(TAG, "POST begin op=%.8s img=%s", job.operation_id, upload_path);
        err = api_post_job(job.operation_id, job.story_id, job.mode,
                           job.base_version, rec->age_group, rec->story_length,
                           "warm_female_01", "zh-CN",
                           upload_path, sha_hex, &st, &http);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            /* 响应丢失：先查原任务，仅 404 才允许按原 ID 重发（实施计划 7.2） */
            publish(OP_WAITING_NETWORK, attempt, revision, "NETWORK_TIMEOUT", true);
            api_job_status_t by_op;
            esp_err_t q = api_get_by_operation(job.operation_id, &by_op);
            if (q == ESP_ERR_NOT_FOUND) {
                err = api_post_job(job.operation_id, job.story_id, job.mode,
                                   job.base_version, rec->age_group, rec->story_length,
                                   "warm_female_01", "zh-CN",
                                   upload_path, sha_hex, &st, &http);
            } else if (q == ESP_OK) {
                st = by_op;
                err = ESP_OK;
            }
        }
    }
    if (err != ESP_OK) {
        strlcpy(rec->state, "failed", sizeof(rec->state));
        strlcpy(rec->error_code, st.error_code[0] ? st.error_code : "NETWORK_TIMEOUT",
                sizeof(rec->error_code));
        if (st.job_id[0]) strlcpy(rec->job_id, st.job_id, sizeof(rec->job_id));
        op_save(rec);
        s_generation_active = false;
        /* 无业务错误信封 = 传输层失败（连不上/超时），总是可重试 */
        publish(OP_FAILED, attempt, revision,
                st.error_code[0] ? st.error_code : "NETWORK_TIMEOUT",
                st.error_code[0] ? st.error_retryable : true);
        vTaskDelete(NULL);
        return;
    }
    strlcpy(rec->job_id, st.job_id, sizeof(rec->job_id));
    strlcpy(rec->state, "remote_pending", sizeof(rec->state));
    op_save(rec);

    /* 轮询：遵守 poll_after + 抖动；同 attempt 阶段只前进（UI 按 revision 核对） */
    strlcpy(s_active_operation, st.operation_id[0] ? st.operation_id : job.operation_id,
            sizeof(s_active_operation));
    strlcpy(s_remote_stage, st.stage, sizeof(s_remote_stage));
    publish(OP_REMOTE_PENDING, st.attempt, st.revision, NULL, false);
    for (;;) {
        if (s_autoplay_token != job.autoplay_token) {
            /* 用户已离开等待：停止本机轮询（云端继续执行是允许情况） */
            strlcpy(rec->state, "waiting_user", sizeof(rec->state));
            op_save(rec);
            s_generation_active = false;
            publish(OP_WAITING_USER, st.attempt, st.revision, NULL, false);
            vTaskDelete(NULL);
            return;
        }
        if (deadline_exceeded(deadline)) {
            strlcpy(rec->state, "waiting_user", sizeof(rec->state));
            strlcpy(rec->error_code, "TIMEOUT", sizeof(rec->error_code));
            op_save(rec);
            s_generation_active = false;
            publish(OP_WAITING_USER, st.attempt, st.revision, "TIMEOUT", true);
            vTaskDelete(NULL);
            return;
        }
        if (strcmp(st.status, "succeeded") == 0) break;
        if (strcmp(st.status, "failed") == 0 || strcmp(st.status, "rejected") == 0) {
            /* 失败分派（实施计划 7.4）：可重试错误在剩余预算内有限重试 */
            bool can_auto = strcmp(st.status, "failed") == 0 &&
                ((st.error_retryable && st.attempt <= AUTO_RETRY_MAX) ||
                 (job.manual_resume && (st.error_retryable || strcmp(st.error_code, "PROVIDER_RESULT_UNKNOWN") == 0)));
            if (can_auto && !deadline_exceeded(deadline)) {
                char cmd[API_OP_ID_LEN];
                snprintf(cmd, sizeof(cmd), "%.24s-r%d", job.operation_id, st.attempt);
                api_job_status_t rst;
                if (api_retry(st.job_id, st.attempt, cmd, &rst) == ESP_OK) {
                    st = rst;
                    revision = rst.revision;
                    attempt = rst.attempt;
                    rec->attempt = rst.attempt;
                    job.manual_resume = false;
                    op_save(rec);
                    continue;
                }
            }
            if (strcmp(st.error_code, "TTS_FAILED") == 0) {
                api_result_t draft = {0};
                if (api_get_result(st.job_id, &draft) == ESP_OK && draft.story && draft.story_len)
                    rec->has_draft = storage_stage_write(job.story_id, job.operation_id, "story.txt",
                        (const uint8_t *)draft.story, draft.story_len) == ESP_OK;
                api_result_free(&draft);
            }
            /* 不可重试（如 CONTENT_REJECTED）记为终态 closed：不再提供"继续生成"，
             * 只能显式重新生成；可重试失败保持 failed 供恢复。 */
            strlcpy(rec->state, st.error_retryable ? "failed" : "closed", sizeof(rec->state));
            strlcpy(rec->error_code, st.error_code, sizeof(rec->error_code));
            op_save(rec);
            s_generation_active = false;
            if (strcmp(st.error_code, "CONTENT_REJECTED") == 0)
                publish(OP_CONTENT_REJECTED, st.attempt, st.revision, st.error_code, false);
            else
                publish(OP_FAILED, st.attempt, st.revision, st.error_code, st.error_retryable);
            vTaskDelete(NULL);
            return;
        }
        if (strcmp(st.status, "expired") == 0) {
            publish(OP_FAILED, st.attempt, st.revision, "RESULT_EXPIRED", false);
            s_generation_active = false; vTaskDelete(NULL); return;
        }
        vTaskDelay(pdMS_TO_TICKS((st.poll_after_ms > 0 ? st.poll_after_ms : 2000) +
                                 (esp_random() % 200)));
        api_job_status_t cur;
        if (api_get_job(st.job_id, &cur) != ESP_OK) continue;   /* 查询失败不等于任务不存在 */
        if (cur.revision >= revision) {
            revision = cur.revision;
            rec->attempt = cur.attempt;
            st = cur;
            strlcpy(s_remote_stage, st.stage, sizeof(s_remote_stage));
            publish(OP_REMOTE_PENDING, st.attempt, st.revision, NULL, false);
        }

    }

    /* 结果下载与提交（FLOW-03：全部进 staging/<op>/，提交统一走存储层） */
    publish(OP_DOWNLOADING, st.attempt, st.revision, NULL, false);
    api_result_t result;
    err = api_get_result(st.job_id, &result);
    if (err == ESP_OK && !result.has_audio) {
        /* TTS 失败但文本已生成：先取回草稿（"故事已生成，声音待完成"），
         * 重试只补声音，不重做识图（FLOW-05） */
        if (result.story && result.story_len > 0) {
            storage_stage_write(job.story_id, job.operation_id, "story.txt",
                                (const uint8_t *)result.story, result.story_len);
            rec->has_draft = true;
        }
        strlcpy(rec->state, "failed", sizeof(rec->state));
        strlcpy(rec->error_code, "TTS_FAILED", sizeof(rec->error_code));
        op_save(rec);
        api_result_free(&result);
        s_generation_active = false;
        publish(OP_FAILED, st.attempt, st.revision, "TTS_FAILED", true);
        vTaskDelete(NULL);
        return;
    }
    if (err != ESP_OK) {
        api_result_free(&result);
        s_generation_active = false;
        publish(OP_FAILED, st.attempt, st.revision, "AI_FAILED", true);
        vTaskDelete(NULL);
        return;
    }

    /* staging 目录 = ai_input.jpg 所在目录（upload_path 是文件路径） */
    char stg_dir[256];
    snprintf(stg_dir, sizeof(stg_dir), "%.250s", upload_path);
    char *dslash = strrchr(stg_dir, '/');
    if (!dslash) {
        api_result_free(&result);
        s_generation_active = false;
        publish(OP_FAILED, st.attempt, st.revision, "SD_IO_ERROR", true);
        vTaskDelete(NULL);
        return;
    }
    *dslash = '\0';
    char dest[300];
    snprintf(dest, sizeof(dest), "%.240s/audio.mp3.part", stg_dir);
    /* 下载失败/校验失败从头重下（实施计划 7.5），最多 1 次重试 */
    for (int dl = 0; dl < 2; dl++) {
        err = api_download_audio(st.job_id, result.audio_sha256,
                                 result.audio_bytes, dest);
        if (err == ESP_OK) break;
        ESP_LOGW(TAG, "audio download attempt %d failed: %s", dl + 1,
                 esp_err_to_name(err));
        if (dl == 0) vTaskDelay(pdMS_TO_TICKS(1500));
    }
    if (err != ESP_OK) {
        /* 下载失败仅重试下载，不重新生成：保留旧可播放版本 */
        ESP_LOGE(TAG, "audio download failed: %s (job=%s expect=%u bytes sha=%.8s...)",
                 esp_err_to_name(err), st.job_id,
                 (unsigned)result.audio_bytes, result.audio_sha256);
        api_result_free(&result);
        s_generation_active = false;
        publish(OP_FAILED, st.attempt, st.revision, "DOWNLOAD_INVALID", true);
        vTaskDelete(NULL);
        return;
    }
    char final_audio[300];
    snprintf(final_audio, sizeof(final_audio), "%.240s/audio.mp3", stg_dir);
    /* 恢复路径可能残留上次已校验的 audio.mp3；FatFs rename 不覆盖目标 */
    unlink(final_audio);
    if (rename(dest, final_audio) != 0) {
        unlink(dest);
        api_result_free(&result);
        s_generation_active = false;
        publish(OP_FAILED, st.attempt, st.revision, "SD_IO_ERROR", true);
        vTaskDelete(NULL);
        return;
    }
    /* 正文写入同一 staging（提交事务由 storage_commit_version 保证） */
    err = storage_stage_write(job.story_id, job.operation_id, "story.txt",
                              (const uint8_t *)result.story, result.story_len);
    if (err != ESP_OK || s_autoplay_token != job.autoplay_token) {
        api_result_free(&result);
        publish(err != ESP_OK ? OP_FAILED : OP_WAITING_USER, st.attempt, st.revision,
                err != ESP_OK ? "SD_IO_ERROR" : NULL, err != ESP_OK);
        s_generation_active = false;
        vTaskDelete(NULL);
        return;
    }

    publish(OP_COMMITTING, st.attempt, st.revision, NULL, false);
    int new_version = 0;
    err = storage_commit_version(job.story_id, job.operation_id, result.title,
                                 result.audio_bytes, result.duration_ms, &new_version);
    api_result_free(&result);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        s_generation_active = false;
        publish(OP_FAILED, st.attempt, st.revision, "VERSION_LIMIT", false);
        vTaskDelete(NULL);
        return;
    }
    if (err != ESP_OK) {
        s_generation_active = false;
        publish(OP_FAILED, st.attempt, st.revision, "SD_IO_ERROR", true);
        vTaskDelete(NULL);
        return;
    }

    strlcpy(rec->state, "completed", sizeof(rec->state));
    rec->error_code[0] = '\0';
    op_save(rec);
    s_generation_active = false;
    publish(OP_COMPLETED, st.attempt, st.revision, NULL, false);
    ESP_LOGI(TAG, "story %s committed v%d", job.story_id, new_version);
    /* UI claims this operation token once before starting playback. */
    vTaskDelete(NULL);
}

esp_err_t story_start_generation(const char *story_id, bool regenerate)
{
    if (!story_id || strlen(story_id) != 32) return ESP_ERR_INVALID_ARG;
    if (s_generation_active) return ESP_ERR_INVALID_STATE;    /* 单任务并发 1 */
    strlcpy(s_active_story, story_id, sizeof(s_active_story));
    int current_version = storage_get_current_version(story_id);
    if (current_version >= STORAGE_MAX_VERSIONS) return ESP_ERR_NOT_SUPPORTED;
    product_settings_t set;
    settings_load_product(&set);
    if (set.consent_version < CONFIG_STORY_CONSENT_VERSION) {
        publish(OP_NEEDS_CONFIRMATION, 1, 1, "NEEDS_CONSENT", false);
        return ESP_ERR_INVALID_STATE;
    }
    if (network_state() < NET_STATE_GOT_IP) {
        /* 离线拍照保持 PHOTO_ONLY；用户明确点击生成时才提示联网 */
        return ESP_ERR_INVALID_STATE;
    }
    if (storage_precheck_space(regenerate) != ESP_OK) return ESP_ERR_NO_MEM;
    if (storage_has_delete_intent(story_id)) return ESP_ERR_INVALID_STATE;

    gen_job_t *job = calloc(1, sizeof(gen_job_t));
    if (!job) return ESP_ERR_NO_MEM;
    strlcpy(job->story_id, story_id, sizeof(job->story_id));
    storage_new_operation_id(job->operation_id);
    strlcpy(job->mode, regenerate ? "regenerate" : "generate", sizeof(job->mode));
    /* 重新生成读取当前已提交版本号（FLOW-05：不再固定 0） */
    job->base_version = 0;
    if (regenerate) {
        if (current_version > 0) job->base_version = current_version;
    }
    job->regenerate = regenerate;
    /* 参数快照在操作创建时固化：进行中的任务不被后续设置修改（FLOW-07） */
    op_record_fill(&job->rec, story_id, job->operation_id, job->mode,
                   job->base_version, &set);

    job->autoplay_token = next_autoplay_token();
    strlcpy(s_active_story, story_id, sizeof(s_active_story));
    s_generation_active = true;
    if (xTaskCreate(generation_worker, "gen_worker", WORKER_STACK, job, 5, NULL) != pdPASS) {
        free(job);
        s_generation_active = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void story_cancel_wait(void)
{
    portENTER_CRITICAL(&s_token_lock);
    s_autoplay_token = 0;
    portEXIT_CRITICAL(&s_token_lock);
}

esp_err_t story_resume_generation(const char *story_id)
{
    /* 显式继续：先查原任务，避免重复生成（FLOW-05）。
     * 存在未完成 operation 时从持久记录恢复；否则按用户显式"继续"
     * 语义走 regenerate 新 operation。 */
    if (s_generation_active) return ESP_ERR_INVALID_STATE;
    char operation_id[STORAGE_ID_HEX];
    if (storage_find_latest_operation(story_id, operation_id) == ESP_OK) {
        op_record_t rec;
        if (op_load(story_id, operation_id, &rec) &&
            !storage_operation_is_terminal(rec.state, rec.error_code)) {
            gen_job_t *job = calloc(1, sizeof(gen_job_t));
            if (!job) return ESP_ERR_NO_MEM;
            job->rec = rec;
            strlcpy(job->story_id, rec.story_id, sizeof(job->story_id));
            strlcpy(job->operation_id, rec.operation_id, sizeof(job->operation_id));
            strlcpy(job->mode, rec.mode, sizeof(job->mode));
            job->base_version = rec.base_version;
            job->regenerate = strcmp(rec.mode, "regenerate") == 0;
            job->manual_resume = true;
            product_settings_t set;
            settings_load_product(&set);
            if (set.consent_version < CONFIG_STORY_CONSENT_VERSION ||
                network_state() < NET_STATE_GOT_IP) {
                free(job);
                return ESP_ERR_INVALID_STATE;
            }
            job->autoplay_token = next_autoplay_token();
            s_generation_active = true;
            if (xTaskCreate(generation_worker, "gen_resume", WORKER_STACK, job, 5, NULL) != pdPASS) {
                free(job);
                s_generation_active = false;
                return ESP_ERR_NO_MEM;
            }
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t story_confirm_consent(void)
{
    product_settings_t set;
    settings_load_product(&set);
    set.consent_version = CONFIG_STORY_CONSENT_VERSION;
    return settings_save_product(&set);
}

esp_err_t story_play(const char *story_id)
{
    if (s_generation_active) return ESP_ERR_INVALID_STATE;
    char audio[256];
    uint32_t duration = 0;
    ESP_RETURN_ON_ERROR(storage_get_playback_info(story_id, audio, sizeof(audio), &duration), TAG, "audio");
    return audio_play_file_with_duration(audio, duration);
}

void story_stop_playback(void)
{
    audio_stop();
}

static void delete_cleanup_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < 500 && !audio_is_quiescent(); ++i) vTaskDelay(pdMS_TO_TICKS(10));
    if (audio_is_quiescent()) storage_continue_delete();
    /* On timeout the durable intent remains hidden and is retried at boot. */
    vTaskDelete(NULL);
}

esp_err_t story_delete_many(const char (*ids)[STORAGE_ID_HEX], int n,
                            story_delete_progress_cb progress, void *user, int *deleted)
{
    if (deleted) *deleted = 0;
    if (!ids || n <= 0) return ESP_ERR_INVALID_ARG;
    if (s_generation_active || s_capture_active) return ESP_ERR_INVALID_STATE;
    story_cancel_wait();
    story_stop_playback();
    esp_err_t err = ESP_OK;
    int ok = 0;
    for (int i = 0; i < n; i++) {
        /* 每条写双槽删除意图并同步（约数百毫秒）；写完即对扫描不可见，内容由清理任务分批删 */
        esp_err_t e = storage_begin_delete(ids[i]);
        if (e == ESP_OK) ok++;
        else err = e;
        if (progress) progress(i + 1, n, user);
    }
    if (ok) xTaskCreate(delete_cleanup_worker, "delete", 6144, NULL, 4, NULL);
    if (deleted) *deleted = ok;
    return err;
}

esp_err_t story_delete(const char *story_id)
{
    char one[1][STORAGE_ID_HEX];
    strlcpy(one[0], story_id, sizeof(one[0]));
    return story_delete_many((const char (*)[STORAGE_ID_HEX])one, 1, NULL, NULL, NULL);
}

esp_err_t story_service_init(story_event_cb cb, void *user)
{
    s_cb = cb;
    s_cb_user = user;
    char server_url[96];
    settings_get_server_url(server_url, sizeof(server_url));
    ESP_LOGI(TAG, "backend %s", server_url);
    api_client_init(server_url);
    s_generation_active = false;
    s_capture_active = false;
    s_autoplay_token = 0;
    /* 重启后绝不自动播放或重新提交生成（实施计划 9.1） */
    return ESP_OK;
}

bool story_generation_active(void) { return s_generation_active; }
