/**
 * @file api_client.c
 * @brief V1 契约设备端实现（esp_http_client + PSA SHA-256）。
 *
 * 修复对应优化建议 FLOW-01/02：
 * - multipart 字段空间精确计算，任何 snprintf/写失败提前返回；
 *   esp_http_client_write 正数短写循环补齐。
 * - 统一收包路径：open -> fetch_headers -> 循环 read；响应缓冲按
 *   Content-Length 有界堆分配（≤32KiB），不再占用任务栈大数组。
 * - 非成功响应一律解析错误信封；无 job_id 的错误 JSON 不再被当作
 *   成功任务；所有返回结构 memset 初始化。
 *
 * 错误映射原则（实施计划 7.1/10）：
 * - HTTP 401/403 -> AUTH_FAILED（ESP_ERR_INVALID_STATE，不重试）
 * - 409 + IDEMPOTENCY_CONFLICT / DEVICE_BUSY / RESULT_NOT_READY -> 解析信封
 * - 429 -> QUOTA_EXCEEDED 信封；410 -> RESULT_EXPIRED（ESP_ERR_INVALID_STATE）
 * - 404 -> ESP_ERR_NOT_FOUND；4xx 其他 -> 解析信封 + ESP_ERR_INVALID_RESPONSE
 * - 5xx/传输失败 -> ESP_FAIL，由 story_service 有界重试
 */

#include "api_client.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "psa/crypto.h"

#include "settings_service.h"

static const char *TAG = "api";

static char s_base_url[128];
static char s_token[33];

/** TLS 校验配置：证书校验永远开启（产品文档 10）。
 * 统一走证书包：ESP-IDF 公共根证书 + 自定义证书包里的开发 CA
 * （CONFIG_MBEDTLS_CUSTOM_CERTIFICATE_BUNDLE_PATH = server_ca.pem），
 * 因此设置页里填域名（公共证书）或局域网 IP（开发证书）都能校验。 */
static void apply_tls(esp_http_client_config_t *cfg)
{
    cfg->crt_bundle_attach = esp_crt_bundle_attach;
}

/* 7.5 初始超时 */
#define TIMEOUT_CONNECT_MS 10000
#define TIMEOUT_UPLOAD_MS 30000
#define TIMEOUT_POLL_MS 10000
#define TIMEOUT_DOWNLOAD_MS 30000

/* 响应缓冲上限：契约 JSON ≤32KiB，音频走流式不经此路径 */
#define RESP_MAX (32 * 1024)

esp_err_t api_client_init(const char *base_url)
{
    strlcpy(s_base_url, base_url, sizeof(s_base_url));
    ESP_RETURN_ON_ERROR(settings_get_device_token(s_token, sizeof(s_token)), TAG, "token");
    /* PSA Crypto 初始化（幂等），供音频下载流式 SHA-256 使用 */
    if (psa_crypto_init() != PSA_SUCCESS) {
        ESP_LOGW(TAG, "psa_crypto_init failed");
    }
    return ESP_OK;
}

static void set_auth(esp_http_client_handle_t cli, const char *idempotency_key)
{
    char auth[64];
    snprintf(auth, sizeof(auth), "Bearer %.32s", s_token);
    esp_http_client_set_header(cli, "Authorization", auth);
    if (idempotency_key) esp_http_client_set_header(cli, "Idempotency-Key", idempotency_key);
}

/** 全量写：esp_http_client_write 可能正数短写，必须循环补齐（FLOW-01） */
static esp_err_t write_all(esp_http_client_handle_t cli, const char *data, int len)
{
    int off = 0;
    while (off < len) {
        int w = esp_http_client_write(cli, data + off, len - off);
        if (w <= 0) return ESP_FAIL;
        off += w;
    }
    return ESP_OK;
}

/**
 * 统一收包：调用前提是 esp_http_client_open + fetch_headers 已完成。
 * 返回 malloc 的 NUL 结尾正文（调用方 free），*out_http 为状态码。
 * 长度上限 RESP_MAX；缺失 Content-Length 时按流式读到上限。
 */
static esp_err_t fetch_body(esp_http_client_handle_t cli, char **body, int *out_http)
{
    *body = NULL;
    int64_t declared = esp_http_client_fetch_headers(cli);
    int code = esp_http_client_get_status_code(cli);
    if (out_http) *out_http = code;
    if (code <= 0 || (declared < 0 && code == 0)) return ESP_FAIL;
    int64_t content_len = esp_http_client_get_content_length(cli);
    bool chunked = esp_http_client_is_chunked_response(cli);
    if (content_len > RESP_MAX) return ESP_ERR_INVALID_SIZE;
    int capacity = content_len > 0 && !chunked ? (int)content_len : RESP_MAX;
    char *buf = malloc((size_t)capacity + 1);
    if (!buf) return ESP_ERR_NO_MEM;
    int total = 0, n = 0;
    while (total < capacity) {
        n = esp_http_client_read(cli, buf + total, capacity - total);
        if (n <= 0) break;
        total += n;
    }
    bool complete = esp_http_client_is_complete_data_received(cli);
    if (total == capacity && !complete) {
        char extra;
        n = esp_http_client_read(cli, &extra, 1);
        complete = esp_http_client_is_complete_data_received(cli);
        if (n > 0) { free(buf); return ESP_ERR_INVALID_SIZE; }
    }
    if (n < 0 || !complete || (content_len >= 0 && !chunked && total != content_len)) {
        free(buf); return ESP_ERR_INVALID_RESPONSE;
    }
    buf[total] = 0;
    *body = buf;
    return ESP_OK;
}

/** 解析状态/错误信封（202/200/4xx 通用）。out 必然被初始化。 */
static esp_err_t parse_status_body(const char *body, api_job_status_t *out)
{
    memset(out, 0, sizeof(*out));
    cJSON *o = cJSON_Parse(body);
    if (!o) return ESP_ERR_INVALID_RESPONSE;
    cJSON *jid = cJSON_GetObjectItem(o, "job_id");
    cJSON *opid = cJSON_GetObjectItem(o, "operation_id");
    cJSON *st = cJSON_GetObjectItem(o, "status");
    cJSON *sg = cJSON_GetObjectItem(o, "stage");
    cJSON *att = cJSON_GetObjectItem(o, "attempt");
    cJSON *rev = cJSON_GetObjectItem(o, "revision");
    cJSON *pa = cJSON_GetObjectItem(o, "poll_after_ms");
    if (cJSON_IsString(jid)) strlcpy(out->job_id, jid->valuestring, sizeof(out->job_id));
    if (cJSON_IsString(opid)) strlcpy(out->operation_id, opid->valuestring, sizeof(out->operation_id));
    if (cJSON_IsString(st)) strlcpy(out->status, st->valuestring, sizeof(out->status));
    if (cJSON_IsString(sg)) strlcpy(out->stage, sg->valuestring, sizeof(out->stage));
    if (cJSON_IsNumber(att)) out->attempt = att->valueint;
    if (cJSON_IsNumber(rev)) out->revision = rev->valueint;
    if (cJSON_IsNumber(pa)) out->poll_after_ms = pa->valueint;
    cJSON *error = cJSON_GetObjectItem(o, "error");
    if (cJSON_IsObject(error)) {
        out->has_error = true;
        cJSON *code = cJSON_GetObjectItem(error, "code");
        cJSON *retryable = cJSON_GetObjectItem(error, "retryable");
        if (cJSON_IsString(code)) strlcpy(out->error_code, code->valuestring, sizeof(out->error_code));
        out->error_retryable = cJSON_IsTrue(retryable);
    }
    cJSON_Delete(o);
    return ESP_OK;
}

/** 按 HTTP 状态码归类 + 解析正文。成功返回 ESP_OK 并填 out。 */
static esp_err_t classify_response(int code, const char *body, api_job_status_t *out,
                                   esp_err_t parse_err)
{
    if (parse_err != ESP_OK) {
        /* 正文缺失/超限/坏 JSON：按状态码给保守错误，绝不当成功 */
        memset(out, 0, sizeof(*out));
        if (code == 202 || code == 200) {
            strlcpy(out->error_code, "PROVIDER_RESULT_UNKNOWN", sizeof(out->error_code));
            out->has_error = true;
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    if (code == 202 || code == 200) {
        if (parse_err != ESP_OK) return parse_err;
        if (!out->job_id[0] || !out->operation_id[0] || !out->status[0] ||
            out->attempt < 1 || out->revision < 1) return ESP_ERR_INVALID_RESPONSE;
        return ESP_OK;
    }
    /* Preserve a parsed business error envelope. */
    switch (code) {
    case 401:
    case 403:
        strlcpy(out->error_code, "AUTH_FAILED", sizeof(out->error_code));
        out->has_error = true;
        return ESP_ERR_INVALID_STATE;
    case 404:
        return ESP_ERR_NOT_FOUND;
    case 410:
        strlcpy(out->error_code, "RESULT_EXPIRED", sizeof(out->error_code));
        out->has_error = true;
        return ESP_ERR_INVALID_STATE;
    case 409:
    case 413:
    case 422:
    case 429:
        /* 业务错误信封：正文必须成功解析并带回调用方 */
        if (parse_err == ESP_OK) return ESP_ERR_INVALID_STATE;
        strlcpy(out->error_code, code == 429 ? "QUOTA_EXCEEDED" : "INVALID_REQUEST",
                sizeof(out->error_code));
        out->has_error = true;
        return ESP_ERR_INVALID_STATE;
    default:
        if (code >= 500) return ESP_FAIL;            /* 服务端错误：可重试 */
        strlcpy(out->error_code, "INVALID_REQUEST", sizeof(out->error_code));
        out->has_error = true;
        return ESP_ERR_INVALID_RESPONSE;
    }
}

/* ---------- multipart 上传（边读边发，不整包入内存） ---------- */

esp_err_t api_post_job(const char *operation_id, const char *story_id,
                       const char *mode, int base_version,
                       const char *age_group, const char *story_length,
                       const char *voice_id, const char *language,
                       const char *image_path, const char *image_sha256_hex,
                       api_job_status_t *out, int *out_http)
{
    memset(out, 0, sizeof(*out));
    if (out_http) *out_http = 0;
    if (!image_sha256_hex || strlen(image_sha256_hex) != 64) {
        return ESP_ERR_INVALID_ARG;      /* 契约：真实 SHA-256（64 hex） */
    }

    char url[192];
    snprintf(url, sizeof(url), "%.96s/v1/jobs", s_base_url);
    const char *boundary = "storycam-7e21a-boundary";

    /* multipart 字段（契约总量上限 1MiB+16KiB，可整体缓冲在 PSRAM） */
    char fields[1024];
    int flen = snprintf(fields, sizeof(fields),
             "--%s\r\nContent-Disposition: form-data; name=\"operation_id\"\r\n\r\n%s\r\n"
             "--%s\r\nContent-Disposition: form-data; name=\"story_id\"\r\n\r\n%s\r\n"
             "--%s\r\nContent-Disposition: form-data; name=\"mode\"\r\n\r\n%s\r\n"
             "--%s\r\nContent-Disposition: form-data; name=\"base_version\"\r\n\r\n%d\r\n"
             "--%s\r\nContent-Disposition: form-data; name=\"image_sha256\"\r\n\r\n%s\r\n"
             "--%s\r\nContent-Disposition: form-data; name=\"age_group\"\r\n\r\n%s\r\n"
             "--%s\r\nContent-Disposition: form-data; name=\"story_length\"\r\n\r\n%s\r\n"
             "--%s\r\nContent-Disposition: form-data; name=\"voice_id\"\r\n\r\n%s\r\n"
             "--%s\r\nContent-Disposition: form-data; name=\"language\"\r\n\r\n%s\r\n",
             boundary, operation_id, boundary, story_id, boundary, mode,
             boundary, base_version, boundary, image_sha256_hex,
             boundary, age_group, boundary, story_length, boundary, voice_id,
             boundary, language);
    if (flen < 0 || flen >= (int)sizeof(fields)) return ESP_ERR_INVALID_SIZE;

    FILE *img = fopen(image_path, "rb");
    if (!img) {
        ESP_LOGE("api", "post: image open failed %s", image_path);
        return ESP_ERR_NOT_FOUND;
    }
    fseek(img, 0, SEEK_END);
    long img_size = ftell(img);
    fseek(img, 0, SEEK_SET);
    if (img_size <= 0 || img_size > 1024 * 1024) {
        fclose(img);
        return ESP_ERR_INVALID_SIZE;     /* 契约：≤1MiB */
    }

    char part_header[160];
    int hlen = snprintf(part_header, sizeof(part_header),
                        "--%s\r\nContent-Disposition: form-data; name=\"image\"; "
                        "filename=\"ai_input.jpg\"\r\nContent-Type: image/jpeg\r\n\r\n",
                        boundary);
    char closing[80];
    int clen = snprintf(closing, sizeof(closing), "\r\n--%s--\r\n", boundary);
    size_t total = (size_t)flen + hlen + img_size + clen;

    uint8_t *body = malloc(total);
    if (!body) { fclose(img); return ESP_ERR_NO_MEM; }
    memcpy(body, fields, flen);
    memcpy(body + flen, part_header, hlen);
    esp_err_t err = ESP_OK;
    if (fread(body + flen + hlen, 1, (size_t)img_size, img) != (size_t)img_size) {
        err = ESP_FAIL;
    }
    fclose(img);
    if (err == ESP_OK) memcpy(body + flen + hlen + img_size, closing, clen);
    if (err != ESP_OK) { free(body); return err; }

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = TIMEOUT_UPLOAD_MS,
        .buffer_size = 32 * 1024,
    };
    apply_tls(&cfg);
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) { free(body); return ESP_FAIL; }
    set_auth(cli, operation_id);                      /* Idempotency-Key = operation_id */
    char ctype[96];
    snprintf(ctype, sizeof(ctype), "multipart/form-data; boundary=%.32s", boundary);
    esp_http_client_set_header(cli, "Content-Type", ctype);
    int code = 0;
    char *resp_body = NULL;
    err = esp_http_client_open(cli, (int)total);
    if (err == ESP_OK) err = write_all(cli, (const char *)body, (int)total);
    free(body);
    if (err == ESP_OK) {
        err = fetch_body(cli, &resp_body, &code);
        if (out_http) *out_http = code;
        if (err == ESP_OK) {
            esp_err_t parse = parse_status_body(resp_body, out);
            err = classify_response(code, resp_body, out, parse);
        }
    }
    free(resp_body);
    esp_http_client_cleanup(cli);
    ESP_LOGI(TAG, "post done: err=%s http=%d", esp_err_to_name(err), code);
    return err;
}

/* ---------- GET 状态 ---------- */

static esp_err_t api_get_status(const char *url, api_job_status_t *out)
{
    memset(out, 0, sizeof(*out));
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = TIMEOUT_POLL_MS,
    };
    apply_tls(&cfg);
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return ESP_FAIL;
    set_auth(cli, NULL);
    esp_err_t err = esp_http_client_open(cli, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(cli);
        return ESP_FAIL;
    }
    int code = 0;
    char *body = NULL;
    err = fetch_body(cli, &body, &code);
    if (err == ESP_OK) {
        esp_err_t parse = parse_status_body(body, out);
        err = classify_response(code, body, out, parse);
    }
    free(body);
    esp_http_client_cleanup(cli);
    return err;
}

esp_err_t api_get_by_operation(const char *operation_id, api_job_status_t *out)
{
    char url[192];
    snprintf(url, sizeof(url), "%.96s/v1/jobs/by-operation/%.32s", s_base_url, operation_id);
    return api_get_status(url, out);
}

esp_err_t api_get_job(const char *job_id, api_job_status_t *out)
{
    char url[192];
    snprintf(url, sizeof(url), "%.96s/v1/jobs/%.47s", s_base_url, job_id);
    return api_get_status(url, out);
}

/* ---------- result ---------- */

esp_err_t api_get_result(const char *job_id, api_result_t *out)
{
    char url[192];
    snprintf(url, sizeof(url), "%.96s/v1/jobs/%.47s/result", s_base_url, job_id);
    memset(out, 0, sizeof(*out));
    esp_http_client_config_t cfg = {
        .url = url, .method = HTTP_METHOD_GET, .timeout_ms = TIMEOUT_POLL_MS,
    };
    apply_tls(&cfg);
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return ESP_FAIL;
    set_auth(cli, NULL);
    esp_err_t err = esp_http_client_open(cli, 0);
    if (err == ESP_OK) {
        int code = 0;
        char *body = NULL;
        err = fetch_body(cli, &body, &code);
        if (err == ESP_OK) {
            if (code == 200) {
                cJSON *o = cJSON_Parse(body);
                if (!o) {
                    err = ESP_ERR_INVALID_RESPONSE;
                } else {
                    cJSON *title = cJSON_GetObjectItem(o, "title");
                    cJSON *story = cJSON_GetObjectItem(o, "story");
                    cJSON *audio = cJSON_GetObjectItem(o, "audio");
                    if (cJSON_IsString(title))
                        strlcpy(out->title, title->valuestring, sizeof(out->title));
                    if (cJSON_IsString(story) && story->valuestring) {
                        out->story_len = strlen(story->valuestring);
                        if (out->story_len > 12 * 1024) {
                            err = ESP_ERR_INVALID_SIZE;
                        } else {
                            out->story = malloc(out->story_len + 1);
                            if (out->story) {
                                memcpy(out->story, story->valuestring, out->story_len + 1);
                            } else err = ESP_ERR_NO_MEM;
                        }
                    }
                    if (err == ESP_OK && cJSON_IsObject(audio)) {
                        out->has_audio = true;
                        cJSON *path = cJSON_GetObjectItem(audio, "path");
                        cJSON *dur = cJSON_GetObjectItem(audio, "duration_ms");
                        cJSON *bytes = cJSON_GetObjectItem(audio, "bytes");
                        cJSON *sha = cJSON_GetObjectItem(audio, "sha256");
                        /* 只接受本服务约定的 audio 路径，禁止任意 URL 注入 */
                        if (cJSON_IsString(path) && path->valuestring[0] == '/')
                            strlcpy(out->audio_path, path->valuestring, sizeof(out->audio_path));
                        if (cJSON_IsNumber(dur)) out->duration_ms = (uint32_t)dur->valuedouble;
                        if (cJSON_IsNumber(bytes)) out->audio_bytes = (size_t)bytes->valuedouble;
                        if (cJSON_IsString(sha))
                            strlcpy(out->audio_sha256, sha->valuestring, sizeof(out->audio_sha256));
                        /* 契约完整性：音频描述字段必须齐全且 hash 格式正确 */
                        if (!out->audio_path[0] || !out->audio_sha256[0] ||
                            strlen(out->audio_sha256) != 64 || out->audio_bytes == 0 ||
                            out->audio_bytes > 2 * 1024 * 1024 || !out->duration_ms || out->duration_ms > 240000) {
                            err = ESP_ERR_INVALID_RESPONSE;
                        }
                    }
                    if (!out->story || !out->story_len || !out->title[0]) err = ESP_ERR_INVALID_RESPONSE;
                    cJSON_Delete(o);
                }
            } else if (code == 409) {
                err = ESP_ERR_NOT_FOUND;         /* RESULT_NOT_READY */
            } else if (code == 410) {
                err = ESP_ERR_INVALID_STATE;     /* RESULT_EXPIRED */
            } else if (code == 401 || code == 403) {
                err = ESP_ERR_INVALID_STATE;     /* AUTH_FAILED */
            } else {
                err = (code >= 500) ? ESP_FAIL : ESP_ERR_INVALID_RESPONSE;
            }
        }
        free(body);
    }
    esp_http_client_cleanup(cli);
    return err;
}

/* ---------- retry ---------- */

esp_err_t api_retry(const char *job_id, int expected_attempt,
                    const char *command_id, api_job_status_t *out)
{
    memset(out, 0, sizeof(*out));
    char url[192];
    snprintf(url, sizeof(url), "%.96s/v1/jobs/%.47s/retry", s_base_url, job_id);
    char body[64];
    snprintf(body, sizeof(body), "{\"expected_attempt\":%d}", expected_attempt);

    esp_http_client_config_t cfg = {
        .url = url, .method = HTTP_METHOD_POST, .timeout_ms = TIMEOUT_POLL_MS,
    };
    apply_tls(&cfg);
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return ESP_FAIL;
    set_auth(cli, command_id);                 /* 新的独立恢复命令 ID */
    esp_http_client_set_header(cli, "Content-Type", "application/json");
    esp_err_t err = ESP_OK;
    if (esp_http_client_open(cli, (int)strlen(body)) != ESP_OK) {
        err = ESP_FAIL;
    } else if (write_all(cli, body, (int)strlen(body)) != ESP_OK) {
        err = ESP_FAIL;
    } else {
        int code = 0;
        char *resp = NULL;
        err = fetch_body(cli, &resp, &code);
        if (err == ESP_OK) {
            esp_err_t parse = parse_status_body(resp, out);
            err = classify_response(code, resp, out, parse);
        }
        free(resp);
    }
    esp_http_client_cleanup(cli);
    return err;
}

/* ---------- audio 下载（流式 + SHA-256 + 长度上限） ---------- */

esp_err_t api_download_audio(const char *job_id, const char *expected_sha256,
                             size_t expected_bytes, const char *dest_path)
{
    char url[192];
    snprintf(url, sizeof(url), "%.96s/v1/jobs/%.47s/audio", s_base_url, job_id);
    if (!expected_sha256 || strlen(expected_sha256) != 64 || expected_bytes == 0 ||
        expected_bytes > 2 * 1024 * 1024) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_http_client_config_t cfg = {
        .url = url, .method = HTTP_METHOD_GET, .timeout_ms = TIMEOUT_DOWNLOAD_MS,
        .buffer_size = 32 * 1024,
    };
    apply_tls(&cfg);
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return ESP_FAIL;
    set_auth(cli, NULL);
    esp_err_t err = esp_http_client_open(cli, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(cli);
        return err;
    }
    esp_http_client_fetch_headers(cli);
    int code = esp_http_client_get_status_code(cli);
    if (code != 200) {
        ESP_LOGE(TAG, "audio download HTTP %d", code);
        esp_http_client_cleanup(cli);
        return code == 410 ? ESP_ERR_INVALID_STATE : ESP_ERR_NOT_FOUND;
    }

    psa_hash_operation_t op = psa_hash_operation_init();
    psa_status_t psa = psa_hash_setup(&op, PSA_ALG_SHA_256);
    FILE *f = fopen(dest_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "audio dest open failed: %s errno=%d", dest_path, errno);
        psa_hash_abort(&op);
        esp_http_client_cleanup(cli);
        return ESP_FAIL;
    }
    uint8_t *chunk = malloc(8192);
    if (!chunk) {
        fclose(f);
        unlink(dest_path);
        psa_hash_abort(&op);
        esp_http_client_cleanup(cli);
        return ESP_ERR_NO_MEM;
    }
    size_t total = 0;
    int read_len;
    err = ESP_OK;
    while ((read_len = esp_http_client_read(cli, (char *)chunk, 8192)) > 0) {
        /* 流式读取 + 逐步校验 */
        total += (size_t)read_len;
        if (total > expected_bytes || total > 2 * 1024 * 1024) {   /* 契约上限 */
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        if (psa == PSA_SUCCESS &&
            psa_hash_update(&op, chunk, (size_t)read_len) != PSA_SUCCESS) {
            psa = PSA_ERROR_SERVICE_FAILURE;
        }
        if (fwrite(chunk, 1, (size_t)read_len, f) != (size_t)read_len) { err = ESP_FAIL; break; }
    }
    free(chunk);
    if (read_len < 0 || !esp_http_client_is_complete_data_received(cli)) err = ESP_FAIL;
    if (fflush(f) != 0 || fsync(fileno(f)) != 0) err = ESP_FAIL;
    if (fclose(f) != 0) err = ESP_FAIL;
    esp_http_client_cleanup(cli);

    uint8_t digest[32];
    char hex[65];
    size_t dlen = 0;
    static const char hexd[] = "0123456789abcdef";
    memset(hex, 0, sizeof(hex));
    if (psa == PSA_SUCCESS &&
        psa_hash_finish(&op, digest, sizeof(digest), &dlen) == PSA_SUCCESS && dlen == 32) {
        for (int i = 0; i < 32; i++) {
            hex[i * 2] = hexd[digest[i] >> 4];
            hex[i * 2 + 1] = hexd[digest[i] & 0xf];
        }
    } else {
        psa_hash_abort(&op);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio read/write failed: %s", esp_err_to_name(err));
        unlink(dest_path);
        return err;
    }
    /* 字节数与 SHA-256 双校验；失败保留旧版本不提交（实施计划 7.3/10） */
    if (total != expected_bytes || strcmp(hex, expected_sha256) != 0) {
        ESP_LOGE(TAG, "audio verify failed: got %u bytes sha=%.8s... expect %u %.8s...",
                 (unsigned)total, hex, (unsigned)expected_bytes, expected_sha256);
        unlink(dest_path);
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}

void api_result_free(api_result_t *r)
{
    if (r && r->story) {
        free(r->story);
        r->story = NULL;
    }
}

esp_err_t api_client_probe(const char *base_url)
{
    if (!base_url || strncmp(base_url, "https://", 8) != 0) return ESP_ERR_INVALID_ARG;
    char url[160];
    snprintf(url, sizeof(url), "%.120s/v1/health", base_url);
    esp_http_client_config_t cfg = {
        .url = url, .method = HTTP_METHOD_GET, .timeout_ms = TIMEOUT_CONNECT_MS,
    };
    apply_tls(&cfg);
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return ESP_ERR_NO_MEM;
    esp_err_t err = esp_http_client_open(cli, 0);
    char body[96] = "";
    int status = 0;
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(cli);
        status = esp_http_client_get_status_code(cli);
        int n = esp_http_client_read(cli, body, sizeof(body) - 1);
        if (n > 0) body[n] = '\0';
    } else {
        /* 连接失败时细分原因（DNS / 超时 / 证书），供设置页给出可读提示 */
        int tls_code = 0, tls_flags = 0;
        esp_err_t tls_err = esp_http_client_get_and_clear_last_tls_error(cli, &tls_code, &tls_flags);
        if (tls_err != ESP_OK) err = tls_err;
    }
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    if (err != ESP_OK) return err;
    return (status == 200 && strstr(body, "\"ok\"")) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}
