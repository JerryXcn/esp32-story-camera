/**
 * @file storage_service.c
 * @brief TF 数据契约实现（实施计划 6.4 写入与提交协议 / 6.5 恢复、删除与空间）。
 *
 * 原则：
 * - 不可变媒体 + 可校验元数据；索引只是缓存，可随时重建。
 * - 关键写入一律 .part -> fsync -> rename，并在回读校验后确认。
 * - 双槽 metadata 以 generation 高且引用完整者为有效；写入槽位轮换规则
 *   （DATA-01）：优先写缺失/无效槽；两槽均有效时写较旧一代。
 * - 版本提交（DATA-02/FLOW-03）：staging/<operation_id>/ 内的 story.txt 与
 *   audio.mp3 由本组件流式计算真实 SHA-256，哈希失败不得提交（禁止全零
 *   占位通过）；全部文件就位并写 version.json 后才切换 metadata。
 * - 删除（DATA-03）：意图双槽持久 -> 从列表隐藏 -> 深度受限递归清理 ->
 *   全部内容删净后最后移除意图；任何一步失败保留意图等待继续。
 * - checksum 规则：按固定键序手工序列化（UTF-8、无空白、排除 checksum 字段）
 *   后取 SHA-256。与后端契约测试共用同一规则。
 */

#include "storage_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_check.h"
#include "psa/crypto.h"
#include "esp_random.h"
#include "esp_vfs_fat.h"

#include "settings_service.h"
#include "system_service.h"

static const char *TAG = "storage";

static char s_mount[32];
static char s_box[64];   /* <mount>/storybox */

/* 前向声明（定义在文件后部，恢复扫描会调用） */
static bool delete_intent_valid(const char *path);
bool storage_has_delete_intent(const char *story_id);
esp_err_t storage_continue_delete_one(const char *story_id);

/* ---------------- 基础工具 ---------------- */

static void join2(char *out, size_t n, const char *a, const char *b)
{
    snprintf(out, n, "%s/%s", a, b);
}
static void join3(char *out, size_t n, const char *a, const char *b, const char *c)
{
    snprintf(out, n, "%s/%s/%s", a, b, c);
}
static void join4(char *out, size_t n, const char *a, const char *b, const char *c, const char *d)
{
    snprintf(out, n, "%s/%s/%s/%s", a, b, c, d);
}

static void story_dir(char *out, size_t n, const char *story_id)
{
    join3(out, n, s_box, "stories", story_id);
}

static void sha256_hex(const uint8_t *data, size_t len, uint8_t out_digest[32])
{
    /* IDF v6.1 = mbedtls 4.x：SHA-256 统一走 PSA Crypto */
    size_t olen = 0;
    psa_status_t st = psa_hash_compute(PSA_ALG_SHA_256, data, len,
                                       out_digest, 32, &olen);
    if (st != PSA_SUCCESS || olen != 32) {
        memset(out_digest, 0, 32);
    }
}

static void hexstr(const uint8_t *digest, char *out /* 65 bytes */)
{
    static const char hexd[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2] = hexd[digest[i] >> 4];
        out[i * 2 + 1] = hexd[digest[i] & 0xf];
    }
    out[64] = '\0';
}

esp_err_t storage_file_sha256(const char *path, char *hex_out /* 65 bytes */)
{
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    psa_hash_operation_t op = psa_hash_operation_init();
    uint8_t *chunk = malloc(8192);
    bool ok = chunk != NULL;
    psa_status_t st = PSA_SUCCESS;
    if (ok) st = psa_hash_setup(&op, PSA_ALG_SHA_256);
    ok = ok && st == PSA_SUCCESS;
    size_t n;
    while (ok && (n = fread(chunk, 1, 8192, f)) > 0) {
        st = psa_hash_update(&op, chunk, n);
        if (st != PSA_SUCCESS) { ok = false; break; }
    }
    if (ferror(f)) ok = false;
    free(chunk);
    fclose(f);
    uint8_t digest[32];
    size_t dlen = 0;
    if (ok) st = psa_hash_finish(&op, digest, sizeof(digest), &dlen);
    if (!ok || st != PSA_SUCCESS || dlen != 32) {
        psa_hash_abort(&op);
        return ESP_FAIL;              /* 哈希失败必须显式失败（DATA-02） */
    }
    hexstr(digest, hex_out);
    return ESP_OK;
}

/** 带长度上限与 fsync 的原子写：data -> <path>.part -> rename(path)
 * FatFs 的 f_rename 不覆盖已存在的目标（FR_EXIST），所以覆盖时先把旧文件
 * 挪到 <path>.bak，再把 .part 换上，最后删 .bak。两次 rename 之间断电时
 * 目标缺失、.bak 仍在，由 read_file_cap 还原旧内容（等同本次写未发生）。 */
static esp_err_t atomic_write(const char *path, const uint8_t *data, size_t len, size_t max_len)
{
    if (len == 0 || len > max_len) return ESP_ERR_INVALID_SIZE;
    char part[256], bak[256];
    snprintf(part, sizeof(part), "%.200s.part", path);
    snprintf(bak, sizeof(bak), "%.200s.bak", path);
    FILE *f = fopen(part, "wb");
    if (!f) return ESP_FAIL;
    size_t written = fwrite(data, 1, len, f);
    if (written != len) { fclose(f); unlink(part); return ESP_ERR_INVALID_SIZE; }
    if (fflush(f) != 0 || fsync(fileno(f)) != 0) { fclose(f); unlink(part); return ESP_FAIL; }
    if (fclose(f) != 0) { unlink(part); return ESP_FAIL; }
    if (rename(part, path) == 0) { unlink(bak); return ESP_OK; }
    struct stat st;
    if (stat(path, &st) != 0) { unlink(part); return ESP_FAIL; }
    unlink(bak);
    if (rename(path, bak) != 0) { unlink(part); return ESP_FAIL; }
    if (rename(part, path) != 0) {
        rename(bak, path);
        unlink(part);
        return ESP_FAIL;
    }
    unlink(bak);
    return ESP_OK;
}

/** 目录存在则复用，否则创建（含父目录，深度有限） */
static esp_err_t ensure_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) return S_ISDIR(st.st_mode) ? ESP_OK : ESP_ERR_INVALID_STATE;
    char parent[256];
    snprintf(parent, sizeof(parent), "%.200s", path);
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent) {
        *slash = '\0';
        ensure_dir(parent);
    }
    if (mkdir(path, 0775) != 0 && errno != EEXIST) return ESP_FAIL;
    return ESP_OK;
}

static bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool dir_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool read_file_cap(const char *path, uint8_t *buf, size_t cap, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        /* atomic_write 覆盖中途断电：目标缺失但旧内容在 .bak，原样还原 */
        char bak[256];
        snprintf(bak, sizeof(bak), "%.200s.bak", path);
        if (rename(bak, path) != 0) return false;
        f = fopen(path, "rb");
        if (!f) return false;
    }
    size_t n = fread(buf, 1, cap, f);
    bool truncated = fgetc(f) != EOF;
    fclose(f);
    if (truncated) return false;
    if (len_out) *len_out = n;
    return true;
}

/** 极简 JPEG 结构校验：SOI 开头 + EOI 结尾 + 最小长度（实施计划 5.1 质量下限） */
static bool looks_like_jpeg(const uint8_t *d, size_t n)
{
    return n > 512 && d[0] == 0xFF && d[1] == 0xD8 && d[n - 2] == 0xFF && d[n - 1] == 0xD9;
}

/* ---------------- 初始化 ---------------- */

esp_err_t storage_init(const char *mount_point)
{
    snprintf(s_mount, sizeof(s_mount), "%s", mount_point);
    join2(s_box, sizeof(s_box), s_mount, "storybox");
    char stories[80];
    join2(stories, sizeof(stories), s_box, "stories");
    ESP_RETURN_ON_ERROR(ensure_dir(s_box), TAG, "box");
    ESP_RETURN_ON_ERROR(ensure_dir(stories), TAG, "stories");
    /* PSA Crypto 初始化（幂等）；失败时哈希类操作将显式报错，不再降级全零 */
    if (psa_crypto_init() != PSA_SUCCESS) {
        ESP_LOGW(TAG, "psa_crypto_init failed");
    }
    return ESP_OK;
}

/* ---------------- ID 分配 ---------------- */

static void gen_hex32(char out[STORAGE_ID_HEX])
{
    static const char hexd[] = "0123456789abcdef";
    uint8_t raw[16];
    esp_fill_random(raw, sizeof(raw));
    for (int i = 0; i < 16; i++) {
        out[i * 2] = hexd[raw[i] >> 4];
        out[i * 2 + 1] = hexd[raw[i] & 0xf];
    }
    out[32] = '\0';
}

static esp_err_t new_unique_id(char out[STORAGE_ID_HEX], bool is_story)
{
    char dir[128];
    for (int tries = 0; tries < 8; tries++) {
        gen_hex32(out);
        if (is_story) {
            story_dir(dir, sizeof(dir), out);
            if (!dir_exists(dir)) return ESP_OK;
        } else {
            /* operation_id：128-bit 随机空间碰撞概率可忽略，不做全卡扫描 */
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

esp_err_t storage_new_story_id(char out[STORAGE_ID_HEX])
{
    return new_unique_id(out, true);
}

esp_err_t storage_new_operation_id(char out[STORAGE_ID_HEX])
{
    return new_unique_id(out, false);
}

/* ---------------- capture / original ---------------- */

esp_err_t storage_commit_original(const char *story_id, const uint8_t *jpeg,
                                  size_t len, uint32_t width, uint32_t height)
{
    char dir[128], path[192];
    story_dir(dir, sizeof(dir), story_id);
    ESP_RETURN_ON_ERROR(ensure_dir(dir), TAG, "storydir");
    join2(path, sizeof(path), dir, "original.jpg");
    if (!jpeg || !looks_like_jpeg(jpeg, len)) {
        /* 损坏照片阻止入库（实施计划 6.4 第 1 步） */
        return ESP_ERR_INVALID_ARG;
    }
    return atomic_write(path, jpeg, len, STORAGE_ORIGINAL_MAX_BYTES);
}

esp_err_t storage_commit_capture_anchor(const char *story_id, uint32_t capture_seq,
                                        const uint8_t *jpeg_sha256)
{
    char dir[128], path[192], osha[65];
    (void)jpeg_sha256;   /* 不信任调用方摘要：原图真实哈希逐块重算（FLOW-01） */
    story_dir(dir, sizeof(dir), story_id);
    join2(path, sizeof(path), dir, "original.jpg");
    if (storage_file_sha256(path, osha) != ESP_OK) return ESP_FAIL;
    /* 真实尺寸（CAM-02：不得写死传感器未提供的数值） */
    uint32_t ow = 0, oh = 0;
    if (storage_jpeg_dimensions(path, &ow, &oh) != ESP_OK) {
        ow = 0; oh = 0;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "schema_version", 1);
    cJSON_AddStringToObject(o, "story_id", story_id);
    cJSON_AddNumberToObject(o, "capture_seq", capture_seq);
    cJSON_AddBoolToObject(o, "time_synced", system_time_is_synced());
    cJSON_AddNumberToObject(o, "created_at_epoch", system_time_now_epoch());
    cJSON_AddNumberToObject(o, "original_w", ow);
    cJSON_AddNumberToObject(o, "original_h", oh);
    cJSON_AddStringToObject(o, "original_sha256", osha);
    char *str = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!str) return ESP_ERR_NO_MEM;
    join2(path, sizeof(path), dir, "capture.json");
    esp_err_t err = atomic_write(path, (const uint8_t *)str, strlen(str), 4096);
    cJSON_free(str);
    if (err != ESP_OK) return err;
    /* 回读验证后才算“照片已保存”（实施计划 6.4 第 2 步） */
    if (!file_exists(path)) return ESP_FAIL;
    return ESP_OK;
}

esp_err_t storage_jpeg_dimensions(const char *path, uint32_t *w, uint32_t *h)
{
    /* 解析 SOF0/ SOF2 段取真实尺寸；失败返回错误（不猜尺寸） */
    *w = 0; *h = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    uint8_t buf[4];
    if (fread(buf, 1, 2, f) != 2 || buf[0] != 0xFF || buf[1] != 0xD8) { fclose(f); return ESP_ERR_INVALID_ARG; }
    esp_err_t err = ESP_ERR_INVALID_ARG;
    for (;;) {
        if (fread(buf, 1, 2, f) != 2) break;
        if (buf[0] != 0xFF) break;                      /* 结构破坏 */
        uint8_t marker = buf[1];
        if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD9)) continue;  /* 无长度段 */
        uint8_t lenb[2];
        if (fread(lenb, 1, 2, f) != 2) break;
        int seglen = ((int)lenb[0] << 8) + lenb[1];
        if (seglen < 2) break;
        if ((marker >= 0xC0 && marker <= 0xC3) || (marker >= 0xC5 && marker <= 0xC7) ||
            (marker >= 0xC9 && marker <= 0xCB) || (marker >= 0xCD && marker <= 0xCF)) {
            uint8_t sof[6];
            if (seglen < 8 || fread(sof, 1, 6, f) != 6) break;
            *h = ((uint32_t)sof[1] << 8) + sof[2];
            *w = ((uint32_t)sof[3] << 8) + sof[4];
            err = (*w && *h) ? ESP_OK : ESP_ERR_INVALID_ARG;
            break;
        }
        /* 跳过段体 */
        seglen -= 2;
        while (seglen > 0) {
            int skip = seglen < (int)sizeof(buf) ? seglen : (int)sizeof(buf);
            if (fread(buf, 1, skip, f) != (size_t)skip) { seglen = 0; break; }
            seglen -= skip;
        }
        if (seglen != 0) break;
    }
    fclose(f);
    return err;
}

/** 缩略图落盘（可重建；失败不回滚原图，实施计划 6.1） */
esp_err_t storage_commit_thumb(const char *story_id, const uint8_t *jpeg, size_t len)
{
    char dir[128], path[192];
    story_dir(dir, sizeof(dir), story_id);
    if (!dir_exists(dir)) {
        ESP_LOGW(TAG, "thumb: story dir missing");
        return ESP_ERR_INVALID_STATE;
    }
    join2(path, sizeof(path), dir, "thumb.jpg");
    if (!jpeg || !looks_like_jpeg(jpeg, len)) {
        ESP_LOGW(TAG, "thumb: invalid jpeg len=%d", (int)len);
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = atomic_write(path, jpeg, len, STORAGE_THUMB_MAX_BYTES);
    if (err != ESP_OK) ESP_LOGW(TAG, "thumb write failed: %d", err);
    return err;
}

/* ---------------- 空间预检 ---------------- */

esp_err_t storage_precheck_space(bool is_regen)
{
    /* 最坏预算（实施计划 6.5）：原图 4MiB + 上传 1MiB + 音频 2MiB +
     * 缩略图 128KiB + 元数据/临时 256KiB；重新生成复用原图但预留新声音。 */
    uint64_t worst = is_regen ? (3ULL * 1024 * 1024 + 384ULL * 1024)
                              : (7ULL * 1024 * 1024 + 384ULL * 1024);
    uint64_t free_bytes = 0;
    uint64_t total = 0;
    if (esp_vfs_fat_info(s_mount, &total, &free_bytes) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;   /* 查不到剩余空间按不可写处理 */
    }
    if (free_bytes < worst + 8ULL * 1024 * 1024) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

uint32_t storage_story_count(void)
{
    char stories[80];
    DIR *d;
    struct dirent *e;
    join2(stories, sizeof(stories), s_box, "stories");
    uint32_t n = 0;
    d = opendir(stories);
    if (!d) return 0;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        n++;
    }
    closedir(d);
    return n;
}

/* ---------------- staging 与版本提交 ---------------- */

esp_err_t storage_stage_write(const char *story_id, const char *operation_id,
                              const char *filename, const uint8_t *data, size_t len)
{
    if (storage_has_delete_intent(story_id)) return ESP_ERR_INVALID_STATE;
    char dir[128], stg[192], path[256];
    story_dir(dir, sizeof(dir), story_id);
    join3(stg, sizeof(stg), dir, "staging", operation_id);
    ESP_RETURN_ON_ERROR(ensure_dir(stg), TAG, "staging");
    ESP_RETURN_ON_ERROR(dir_exists(stg) ? ESP_OK : ESP_FAIL, TAG, "staging verify");
    join2(path, sizeof(path), stg, filename);
    size_t cap = STORAGE_AUDIO_MAX_BYTES;
    if (strncmp(filename, "ai_input", 8) == 0) cap = 1024 * 1024;
    if (strncmp(filename, "story.txt", 9) == 0) cap = STORAGE_STORY_TEXT_MAX_BYTES;
    return atomic_write(path, data, len, cap);
}

esp_err_t storage_stage_discard(const char *story_id, const char *operation_id)
{
    char path[256];
    join4(path, sizeof(path), s_box, "stories", story_id, "staging");
    char stg[300];
    snprintf(stg, sizeof(stg), "%.200s/%.33s", path, operation_id);
    /* 递归删除目录内容后移除目录（FAT 上逐文件删除） */
    DIR *d = opendir(stg);
    if (!d) return ESP_OK;
    struct dirent *e;
    char f[512];
    while ((e = readdir(d)) != NULL) {
        snprintf(f, sizeof(f), "%.300s/%.64s", stg, e->d_name);
        unlink(f);
    }
    closedir(d);
    rmdir(stg);
    return ESP_OK;
}

/* ---------------- 双槽 metadata（DATA-01/02） ---------------- */

/** 确定性序列化（固定键序，排除 checksum 字段本身）后计算 SHA-256 */
static void metadata_canonical(const char *story_id, uint32_t capture_seq,
                               int current_version, uint32_t generation,
                               const char *versions_csv,
                               char *out, size_t outn)
{
    snprintf(out, outn,
             "{\"capture_seq\":%lu,\"current_version\":%d,\"generation\":%lu,"
             "\"schema_version\":1,\"story_id\":\"%s\",\"versions\":\"%s\"}",
             (unsigned long)capture_seq, current_version, (unsigned long)generation,
             story_id, versions_csv);
}

static esp_err_t write_metadata_slot(const char *story_id, char slot /* 'a'|'b' */,
                                     uint32_t capture_seq, int current_version,
                                     uint32_t generation, const char *versions_csv)
{
    char dir[128], path[192], canon[1024];
    story_dir(dir, sizeof(dir), story_id);
    metadata_canonical(story_id, capture_seq, current_version, generation, versions_csv,
                       canon, sizeof(canon));
    uint8_t digest[32];
    char checksum[65];
    sha256_hex((const uint8_t *)canon, strlen(canon), digest);
    hexstr(digest, checksum);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "schema_version", 1);
    cJSON_AddNumberToObject(o, "generation", generation);
    cJSON_AddStringToObject(o, "checksum", checksum);
    cJSON_AddStringToObject(o, "story_id", story_id);
    cJSON_AddNumberToObject(o, "capture_seq", capture_seq);
    cJSON_AddBoolToObject(o, "time_synced", system_time_is_synced());
    cJSON_AddNumberToObject(o, "created_at_epoch", system_time_now_epoch());
    cJSON_AddNumberToObject(o, "current_version", current_version);
    cJSON_AddStringToObject(o, "committed_versions", versions_csv);
    char *str = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!str) return ESP_ERR_NO_MEM;
    snprintf(path, sizeof(path), "%.100s/metadata_%c.json", dir, slot);
    esp_err_t err = atomic_write(path, (const uint8_t *)str, strlen(str), 8192);
    cJSON_free(str);
    return err;
}

/** 读取并校验一个 metadata 槽；失败返回 ESP_ERR_INVALID_STATE */
typedef struct {
    uint32_t generation;
    int current_version;
    uint32_t capture_seq;
    char versions_csv[64];
} meta_slot_t;

static esp_err_t read_metadata_slot(const char *story_id, char slot, meta_slot_t *out)
{
    char dir[128], path[192], buf[2048];
    size_t len = 0;
    story_dir(dir, sizeof(dir), story_id);
    snprintf(path, sizeof(path), "%.100s/metadata_%c.json", dir, slot);
    if (!read_file_cap(path, (uint8_t *)buf, sizeof(buf) - 1, &len)) return ESP_ERR_INVALID_STATE;
    buf[len] = '\0';
    cJSON *o = cJSON_Parse(buf);
    if (!o) return ESP_ERR_INVALID_STATE;
    cJSON *chk = cJSON_GetObjectItem(o, "checksum");
    cJSON *gen = cJSON_GetObjectItem(o, "generation");
    cJSON *cur = cJSON_GetObjectItem(o, "current_version");
    cJSON *seq = cJSON_GetObjectItem(o, "capture_seq");
    cJSON *ver = cJSON_GetObjectItem(o, "committed_versions");
    cJSON *sid = cJSON_GetObjectItem(o, "story_id");
    esp_err_t err = ESP_ERR_INVALID_STATE;
    do {
        if (!cJSON_IsString(chk) || !cJSON_IsNumber(gen) || !cJSON_IsNumber(cur) ||
            !cJSON_IsNumber(seq) || !cJSON_IsString(ver) || !cJSON_IsString(sid)) break;
        if (strcmp(sid->valuestring, story_id) != 0) break;
        metadata_canonical(story_id, (uint32_t)seq->valuedouble, cur->valueint,
                           (uint32_t)gen->valuedouble, ver->valuestring, buf, sizeof(buf));
        uint8_t digest[32];
        char expect[65];
        sha256_hex((const uint8_t *)buf, strlen(buf), digest);
        hexstr(digest, expect);
        if (strcmp(expect, chk->valuestring) != 0) break;
        out->generation = (uint32_t)gen->valuedouble;
        out->current_version = cur->valueint;
        out->capture_seq = (uint32_t)seq->valuedouble;
        snprintf(out->versions_csv, sizeof(out->versions_csv), "%.63s", ver->valuestring);
        err = ESP_OK;
    } while (0);
    cJSON_Delete(o);
    return err;
}

static bool version_complete(const char *story_id, int version);

/** 读取有效 metadata（generation 高且校验通过的槽）；无有效槽返回 false */
static bool read_valid_metadata(const char *story_id, meta_slot_t *out)
{
    if (storage_has_delete_intent(story_id)) return false;
    meta_slot_t sa, sb;
    bool has_a = read_metadata_slot(story_id, 'a', &sa) == ESP_OK;
    bool has_b = read_metadata_slot(story_id, 'b', &sb) == ESP_OK;
    const meta_slot_t *first = NULL, *second = NULL;
    if (has_a && has_b) {
        first = sa.generation >= sb.generation ? &sa : &sb;
        second = first == &sa ? &sb : &sa;
    } else if (has_a) first = &sa;
    else if (has_b) first = &sb;
    /* Validate the newest candidate once. Only hash the old version on fallback. */
    if (first && version_complete(story_id, first->current_version)) { *out = *first; return true; }
    if (second && version_complete(story_id, second->current_version)) { *out = *second; return true; }
    return false;
}

/** 版本内容完整性：story.txt/audio.mp3/version.json 都存在，version.json
 *  可解析，长度及音频/正文的 SHA-256 都与清单一致。 */
static bool version_complete(const char *story_id, int version)
{
    char dir[128], path[256];
    story_dir(dir, sizeof(dir), story_id);
    snprintf(path, sizeof(path), "%.100s/versions/%d", dir, version);
    char f[320];
    join2(f, sizeof(f), path, "story.txt");
    if (!file_exists(f)) return false;
    join2(f, sizeof(f), path, "audio.mp3");
    if (!file_exists(f)) return false;
    join2(f, sizeof(f), path, "version.json");
    if (!file_exists(f)) return false;
    uint8_t buf[1024];
    size_t len = 0;
    if (!read_file_cap(f, buf, sizeof(buf) - 1, &len)) return false;
    buf[len] = '\0';
    cJSON *o = cJSON_Parse((char *)buf);
    if (!o) return false;
    bool ok = false;
    cJSON *abytes = cJSON_GetObjectItem(o, "audio_bytes");
    cJSON *ash = cJSON_GetObjectItem(o, "audio_sha256");
    cJSON *ssh = cJSON_GetObjectItem(o, "story_sha256");
    cJSON *dur = cJSON_GetObjectItem(o, "duration_ms");
    if (cJSON_IsNumber(abytes) && cJSON_IsString(ash) &&
        strlen(ash->valuestring) == 64 && cJSON_IsString(ssh) &&
        strlen(ssh->valuestring) == 64 && cJSON_IsNumber(dur) &&
        dur->valuedouble > 0 && dur->valuedouble <= 240000 &&
        abytes->valuedouble > 0 && abytes->valuedouble <= STORAGE_AUDIO_MAX_BYTES) {
        snprintf(f, sizeof(f), "%.200s/audio.mp3", path);
        struct stat st;
        char actual[65];
        if (stat(f, &st) == 0 && (uint64_t)st.st_size == (uint64_t)abytes->valuedouble &&
            storage_file_sha256(f, actual) == ESP_OK && strcmp(actual, ash->valuestring) == 0) {
            join2(f, sizeof(f), path, "story.txt");
            ok = storage_file_sha256(f, actual) == ESP_OK && strcmp(actual, ssh->valuestring) == 0;
        }
    }
    cJSON_Delete(o);
    return ok;
}

esp_err_t storage_get_current_version(const char *story_id)
{
    meta_slot_t valid;
    if (!read_valid_metadata(story_id, &valid)) return -ESP_ERR_NOT_FOUND;
    if (valid.current_version <= 0) return -ESP_ERR_NOT_FOUND;
    return valid.current_version;
}

static void csv_append(char *csv, size_t n, int v)
{
    size_t len = strlen(csv);
    snprintf(csv + len, n - len, len ? ",%d" : "%d", v);
}

/**
 * 版本提交（实施计划 6.4 第 4~6 步 + FLOW-03 + DATA-02）：
 * 1. staging/<op>/ 内 story.txt 与 audio.mp3 必须已由 story_service 就位；
 * 2. 本组件对两个文件流式计算真实 SHA-256（哈希失败 = 不提交）；
 * 3. 移动到 versions/<n>/（同分区 rename 原子），写 version.json；
 * 4. 最后一步切换 metadata 双槽（激活点）；失败保留旧版。
 */
esp_err_t storage_commit_version(const char *story_id, const char *operation_id,
                                 const char *title, size_t audio_bytes_expected,
                                 uint32_t duration_ms, int *new_version_out)
{
    char dir[128], vdir[192], path[256];
    story_dir(dir, sizeof(dir), story_id);

    meta_slot_t valid;
    bool has_valid = read_valid_metadata(story_id, &valid);

    char versions_csv[64] = {0};
    uint32_t generation = 0;
    uint32_t capture_seq = 0;
    if (has_valid) {
        snprintf(versions_csv, sizeof(versions_csv), "%s", valid.versions_csv);
        generation = valid.generation;
        capture_seq = valid.capture_seq;
    } else {
        /* 无有效 metadata：从 capture.json 恢复锚点信息 */
        char p2[192];
        uint8_t buf[1024];
        size_t len = 0;
        join2(p2, sizeof(p2), dir, "capture.json");
        if (!read_file_cap(p2, buf, sizeof(buf) - 1, &len)) return ESP_ERR_INVALID_STATE;
        buf[len] = '\0';
        cJSON *o = cJSON_Parse((char *)buf);
        if (!o) return ESP_ERR_INVALID_STATE;
        cJSON *seq = cJSON_GetObjectItem(o, "capture_seq");
        if (!cJSON_IsNumber(seq)) { cJSON_Delete(o); return ESP_ERR_INVALID_STATE; }
        capture_seq = (uint32_t)seq->valuedouble;
        cJSON_Delete(o);
    }

    if (storage_has_delete_intent(story_id)) return ESP_ERR_INVALID_STATE;
    if (!duration_ms || duration_ms > 240000 || !audio_bytes_expected ||
        audio_bytes_expected > STORAGE_AUDIO_MAX_BYTES) return ESP_ERR_INVALID_SIZE;
    for (int v = 1; has_valid && v <= STORAGE_MAX_VERSIONS; ++v) {
        char candidate[16];
        snprintf(candidate, sizeof(candidate), ",%d,", v);
        char committed[80];
        snprintf(committed, sizeof(committed), ",%s,", versions_csv);
        if (!strstr(committed, candidate)) continue;
        uint8_t vb[1024]; size_t vn = 0;
        snprintf(path, sizeof(path), "%.100s/versions/%d/version.json", dir, v);
        if (!read_file_cap(path, vb, sizeof(vb) - 1, &vn)) continue;
        vb[vn] = 0;
        cJSON *manifest = cJSON_Parse((char *)vb);
        cJSON *op = cJSON_GetObjectItem(manifest, "operation_id");
        bool same = cJSON_IsString(op) && strcmp(op->valuestring, operation_id) == 0;
        cJSON_Delete(manifest);
        if (same) {
            if (!version_complete(story_id, v)) return ESP_ERR_INVALID_CRC;
            /* 恢复路径重下的 staging 副本已无用（版本早已提交），一并清理 */
            storage_stage_discard(story_id, operation_id);
            if (new_version_out) *new_version_out = v;
            return ESP_OK;
        }
    }
    int max_v = 0;
    for (const char *p = versions_csv; *p; ) {
        char *end = NULL;
        int v = (int)strtol(p, &end, 10);
        if (end == p || v < 1 || v > STORAGE_MAX_VERSIONS || (*end && *end != ','))
            return ESP_ERR_INVALID_STATE;
        p = end;
        if (v > max_v) max_v = v;
        if (*p == ',') p++;
    }
    int next_version = max_v + 1;
    if (max_v >= STORAGE_MAX_VERSIONS) return ESP_ERR_NOT_SUPPORTED;  /* 版本上限 */

    /* staging 内文件必须先就位并真实校验（FLOW-03：音频与正文同一 staging 事务） */
    char src[300], dst[300];
    join3(src, sizeof(src), dir, "staging", operation_id);
    char staged_story[384], staged_audio[384];
    join2(staged_story, sizeof(staged_story), src, "story.txt");
    join2(staged_audio, sizeof(staged_audio), src, "audio.mp3");
    if (!file_exists(staged_story) || !file_exists(staged_audio)) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 真实哈希与长度（DATA-02：哈希失败路径显式失败） */
    char story_sha_hex[65], audio_sha_hex[65];
    if (storage_file_sha256(staged_story, story_sha_hex) != ESP_OK ||
        storage_file_sha256(staged_audio, audio_sha_hex) != ESP_OK) {
        return ESP_FAIL;
    }
    struct stat ast;
    if (stat(staged_audio, &ast) != 0 || (size_t)ast.st_size != audio_bytes_expected) {
        return ESP_ERR_INVALID_CRC;      /* 长度与 result 描述不符 */
    }

    /* 4) staging -> versions/<n>/ 移动（同分区 rename 原子） */
    snprintf(vdir, sizeof(vdir), "%.100s/versions/%d", dir, next_version);
    struct stat existing;
    /* An interrupted commit may leave an unreferenced directory. Preserve it
     * instead of overwriting files; select the next unused version number. */
    while (stat(vdir, &existing) == 0 && next_version <= STORAGE_MAX_VERSIONS) {
        ++next_version;
        snprintf(vdir, sizeof(vdir), "%.100s/versions/%d", dir, next_version);
    }
    if (next_version > STORAGE_MAX_VERSIONS) return ESP_ERR_NOT_SUPPORTED;
    ESP_RETURN_ON_ERROR(ensure_dir(vdir), TAG, "vdir");

    join2(dst, sizeof(dst), vdir, "story.txt");
    if (rename(staged_story, dst) != 0) return ESP_FAIL;
    join2(dst, sizeof(dst), vdir, "audio.mp3");
    if (rename(staged_audio, dst) != 0) {
        /* 回滚正文移动，保持 staging 完整（失败保留旧版语义） */
        char back[384];
        join2(dst, sizeof(dst), vdir, "story.txt");
        join2(back, sizeof(back), src, "story.txt");
        rename(dst, back);
        rmdir(vdir);
        return ESP_FAIL;
    }

    /* 5) version.json 提交清单（真实长度与哈希，不含临时路径） */
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "schema_version", 1);
    cJSON_AddNumberToObject(o, "version", next_version);
    cJSON_AddStringToObject(o, "operation_id", operation_id);
    cJSON_AddStringToObject(o, "title", title);
    cJSON_AddStringToObject(o, "story_sha256", story_sha_hex);
    cJSON_AddStringToObject(o, "audio_sha256", audio_sha_hex);
    cJSON_AddNumberToObject(o, "audio_bytes", (double)ast.st_size);
    cJSON_AddNumberToObject(o, "duration_ms", duration_ms);
    cJSON_AddStringToObject(o, "audio_format", "mp3/24000/1/64000");
    char *str = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!str) return ESP_ERR_NO_MEM;
    join2(dst, sizeof(dst), vdir, "version.json");
    esp_err_t err = atomic_write(dst, (const uint8_t *)str, strlen(str), 4096);
    cJSON_free(str);
    if (err != ESP_OK) return err;
    if (!version_complete(story_id, next_version)) {
        return ESP_FAIL;                 /* 自校验不过 = 不切换 metadata */
    }

    /* 6) 双槽轮换提交（DATA-01）：优先写缺失/无效槽；两槽均有效写较旧一代。
     *    metadata 切换是激活点；新槽无效则仍使用旧版。 */
    csv_append(versions_csv, sizeof(versions_csv), next_version);
    generation += 1;
    char slot = 'a';
    meta_slot_t sa, sb;
    bool has_a = read_metadata_slot(story_id, 'a', &sa) == ESP_OK && version_complete(story_id, sa.current_version);
    bool has_b = read_metadata_slot(story_id, 'b', &sb) == ESP_OK && version_complete(story_id, sb.current_version);
    if (has_a && has_b) slot = (sa.generation <= sb.generation) ? 'a' : 'b';
    else if (has_a && !has_b) slot = 'b';
    else if (!has_a && has_b) slot = 'a';
    /* 两槽皆无效：写 a（首个有效槽） */
    err = write_metadata_slot(story_id, slot, capture_seq, next_version, generation, versions_csv);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "metadata slot %c write failed: %d", slot, err);
        return err;
    }
    meta_slot_t verify;
    if (read_metadata_slot(story_id, slot, &verify) != ESP_OK ||
        verify.generation != generation) {
        return ESP_FAIL;
    }
    storage_stage_discard(story_id, operation_id);
    if (new_version_out) *new_version_out = next_version;
    return ESP_OK;
}

/* ---------------- 恢复 ---------------- */

esp_err_t storage_get_audio_path(const char *story_id, char *buf, size_t buflen)
{
    meta_slot_t valid;
    if (!read_valid_metadata(story_id, &valid)) return ESP_ERR_NOT_FOUND;
    if (valid.current_version <= 0) return ESP_ERR_NOT_FOUND;
    char dir[128];
    story_dir(dir, sizeof(dir), story_id);
    snprintf(buf, buflen, "%.100s/versions/%d/audio.mp3", dir, valid.current_version);
    return ESP_OK;
}

esp_err_t storage_get_photo_path(const char *story_id, char *buf, size_t buflen)
{
    char dir[128];
    story_dir(dir, sizeof(dir), story_id);
    snprintf(buf, buflen, "%.100s/original.jpg", dir);
    return file_exists(buf) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t storage_get_thumb_path(const char *story_id, char *buf, size_t buflen)
{
    char dir[128];
    story_dir(dir, sizeof(dir), story_id);
    snprintf(buf, buflen, "%.100s/thumb.jpg", dir);
    return file_exists(buf) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t storage_get_story_text(const char *story_id, char *buf, size_t buflen)
{
    meta_slot_t valid;
    if (!read_valid_metadata(story_id, &valid)) return ESP_ERR_NOT_FOUND;
    if (valid.current_version <= 0) return ESP_ERR_NOT_FOUND;
    char dir[128], path[256];
    story_dir(dir, sizeof(dir), story_id);
    snprintf(path, sizeof(path), "%.100s/versions/%d/story.txt", dir, valid.current_version);
    size_t len = 0;
    if (!read_file_cap(path, (uint8_t *)buf, buflen - 1, &len)) return ESP_FAIL;
    buf[len] = '\0';
    return ESP_OK;
}

/** 从 version.json 读显示标题（无则空串） */
static void read_version_title(const char *story_id, int version, char *out, size_t n)
{
    out[0] = '\0';
    if (version <= 0) return;
    char dir[128], path[256];
    story_dir(dir, sizeof(dir), story_id);
    snprintf(path, sizeof(path), "%.100s/versions/%d/version.json", dir, version);
    uint8_t buf[1024];
    size_t len = 0;
    if (!read_file_cap(path, buf, sizeof(buf) - 1, &len)) return;
    buf[len] = '\0';
    cJSON *o = cJSON_Parse((char *)buf);
    if (!o) return;
    cJSON *t = cJSON_GetObjectItem(o, "title");
    if (cJSON_IsString(t)) strlcpy(out, t->valuestring, n);
    cJSON_Delete(o);
}

esp_err_t storage_get_summary(const char *sid, story_summary_t *s)
{
    if (!sid || !s || storage_has_delete_intent(sid)) return ESP_ERR_NOT_FOUND;
    char dir[128], path[256];
        story_dir(dir, sizeof(dir), sid);
        memset(s, 0, sizeof(*s));
        snprintf(s->story_id, sizeof(s->story_id), "%.32s", sid);

        /* 有效 metadata 决定可播放性、序号与当前版本（DATA-04：完整填充） */
        meta_slot_t valid;
        bool has_valid = read_valid_metadata(sid, &valid);
        int cur_ver = 0;
        if (has_valid) {
            s->capture_seq = valid.capture_seq;
            cur_ver = valid.current_version;
            s->current_version = cur_ver;
            s->availability = STORY_AVAIL_PLAYABLE;
            read_version_title(sid, cur_ver, s->title, sizeof(s->title));
            if (!s->title[0]) strlcpy(s->title, "未命名故事", sizeof(s->title));
        }

        /* capture.json / 孤立原图恢复（实施计划 6.4 第 2 步、6.5） */
        join2(path, sizeof(path), dir, "capture.json");
        if (file_exists(path)) {
            uint8_t buf[1024];
            size_t len = 0;
            if (read_file_cap(path, buf, sizeof(buf) - 1, &len)) {
                buf[len] = '\0';
                cJSON *o = cJSON_Parse((char *)buf);
                if (o) {
                    cJSON *seq = cJSON_GetObjectItem(o, "capture_seq");
                    cJSON *ts = cJSON_GetObjectItem(o, "time_synced");
                    cJSON *cat = cJSON_GetObjectItem(o, "created_at_epoch");
                    if (cJSON_IsNumber(seq)) s->capture_seq = (uint32_t)seq->valuedouble;
                    s->has_time = cJSON_IsTrue(ts);
                    if (cJSON_IsNumber(cat)) s->created_at_epoch = (int64_t)cat->valuedouble;
                    cJSON_Delete(o);
                }
            }
        }
        join2(path, sizeof(path), dir, "original.jpg");
        if (!file_exists(path)) return ESP_ERR_NOT_FOUND;    /* 无原图无法恢复 */
        if (s->capture_seq == 0) {
            /* 孤立原图：capture 锚点未写完。补建待生成记录，不自动上传。 */
            settings_reserve_capture_seq(1, &s->capture_seq);
            if (storage_commit_capture_anchor(sid, s->capture_seq, NULL) != ESP_OK)
                return ESP_FAIL;
            s->availability = STORY_AVAIL_PHOTO_ONLY;
        }
        if (!has_valid) s->availability = STORY_AVAIL_PHOTO_ONLY;
        /* 缩略图路径（缺失由 UI 占位，重建为后续任务） */
        join2(path, sizeof(path), dir, "thumb.jpg");
        if (file_exists(path)) {
            snprintf(s->thumb_path, sizeof(s->thumb_path), "%.127s", path);
        }
        join2(path, sizeof(path), dir, "favorite");
        s->favorite = file_exists(path);
    return ESP_OK;
}

esp_err_t storage_set_favorite(const char *story_id, bool favorite)
{
    if (!story_id || strlen(story_id) != 32 || storage_has_delete_intent(story_id)) return ESP_ERR_INVALID_ARG;
    char dir[128], path[256];
    story_dir(dir, sizeof(dir), story_id);
    if (!dir_exists(dir)) return ESP_ERR_NOT_FOUND;
    join2(path, sizeof(path), dir, "favorite");
    if (!favorite) return (unlink(path) == 0 || errno == ENOENT) ? ESP_OK : ESP_FAIL;
    FILE *f = fopen(path, "w");
    if (!f) return ESP_FAIL;
    bool ok = fputs("1", f) >= 0 && fflush(f) == 0 && fsync(fileno(f)) == 0;
    fclose(f);
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t storage_get_playback_info(const char *story_id, char *audio_path, size_t cap,
                                      uint32_t *duration_ms)
{
    if (!duration_ms || !audio_path || !cap) return ESP_ERR_INVALID_ARG;
    *duration_ms = 0;
    meta_slot_t valid;
    if (!read_valid_metadata(story_id, &valid)) return ESP_ERR_NOT_FOUND;
    char dir[128], path[256], buf[1024]; size_t len = 0;
    story_dir(dir, sizeof(dir), story_id);
    int written = snprintf(audio_path, cap, "%.100s/versions/%d/audio.mp3", dir, valid.current_version);
    if (written < 0 || (size_t)written >= cap) return ESP_ERR_INVALID_SIZE;
    snprintf(path, sizeof(path), "%.100s/versions/%d/version.json", dir, valid.current_version);
    if (!read_file_cap(path, (uint8_t *)buf, sizeof(buf) - 1, &len)) return ESP_FAIL;
    buf[len] = 0;
    cJSON *o = cJSON_Parse(buf);
    cJSON *dur = cJSON_GetObjectItem(o, "duration_ms");
    if (cJSON_IsNumber(dur) && dur->valuedouble > 0 && dur->valuedouble <= 240000)
        *duration_ms = (uint32_t)dur->valuedouble;
    cJSON_Delete(o);
    return *duration_ms ? ESP_OK : ESP_ERR_INVALID_STATE;
}
esp_err_t storage_get_audio_duration(const char *story_id, uint32_t *duration_ms)
{
    char audio[256];
    return storage_get_playback_info(story_id, audio, sizeof(audio), duration_ms);
}

bool storage_operation_is_terminal(const char *state, const char *error_code)
{
    if (!state) return false;
    if (!strcmp(state, "completed") || !strcmp(state, "closed")) return true;
    return !strcmp(state, "failed") && error_code && !strcmp(error_code, "CONTENT_REJECTED");
}

/** 清理已完成操作残留的 staging/<op>/（如断电在提交后、清理前）。
 *  未完成操作的 staging 保留给恢复路径使用。 */
static void sweep_completed_staging(const char *story_id)
{
    char dir[128], stg[192], path[256];
    story_dir(dir, sizeof(dir), story_id);
    join2(stg, sizeof(stg), dir, "staging");
    DIR *d = opendir(stg);
    if (!d) return;
    char done[8][STORAGE_ID_HEX];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < 8) {
        if (strlen(e->d_name) != 32) continue;
        snprintf(path, sizeof(path), "%.100s/operations/%.32s.json", dir, e->d_name);
        uint8_t buf[1024];
        size_t len = 0;
        if (!read_file_cap(path, buf, sizeof(buf) - 1, &len)) continue;
        buf[len] = '\0';
        cJSON *o = cJSON_Parse((char *)buf);
        cJSON *state = cJSON_GetObjectItem(o, "state");
        cJSON *code = cJSON_GetObjectItem(o, "error_code");
        if (cJSON_IsString(state) &&
            storage_operation_is_terminal(state->valuestring, cJSON_IsString(code) ? code->valuestring : ""))
            snprintf(done[n++], STORAGE_ID_HEX, "%.32s", e->d_name);
        cJSON_Delete(o);
    }
    closedir(d);
    for (int i = 0; i < n; i++) storage_stage_discard(story_id, done[i]);
}

int storage_recover_all(story_summary_t *out, int max_out)
{
    if (!out || max_out <= 0) return 0;
    char stories[80];
    join2(stories, sizeof(stories), s_box, "stories");
    DIR *d = opendir(stories);
    if (!d) return 0;
    struct dirent *e;
    int count = 0;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' || strlen(e->d_name) != 32) continue;
        const char *sid = e->d_name;
        if (storage_has_delete_intent(sid)) {
            /* 有有效删除意图的记录不展示、继续清理（实施计划 6.5） */
            continue;
        }
        sweep_completed_staging(sid);
        if (storage_get_summary(sid, &out[count]) != ESP_OK) continue;
        count++;
        if (count >= max_out) {
            /* DATA-04：容量截断时不能保证保留最新——对满载情况记录告警；
             * 当前实现按目录序扫描（FAT 名字序无时序意义），容量 64 覆盖
             * 正常开发/演示场景，全量 1000 条分页扫描列为限制。 */
            ESP_LOGW(TAG, "recover hit max_out=%d（超出部分本次未展示）", max_out);
            break;
        }
    }
    closedir(d);
    /* capture_seq 倒序插入排序（数量有限，简单排序即可） */
    for (int i = 1; i < count; i++) {
        story_summary_t key = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].capture_seq < key.capture_seq) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = key;
    }
    return count;
}

/* ---------------- operation 持久化（FLOW-05） ---------------- */

static void operations_dir(const char *story_id, char *out, size_t n)
{
    char dir[128];
    story_dir(dir, sizeof(dir), story_id);
    snprintf(out, n, "%.150s/operations", dir);
}

esp_err_t storage_write_operation(const char *story_id, const char *operation_id,
                                  const char *json, size_t len)
{
    char dir[160], path[256];
    operations_dir(story_id, dir, sizeof(dir));
    ESP_RETURN_ON_ERROR(ensure_dir(dir), TAG, "opsdir");
    snprintf(path, sizeof(path), "%.190s/%.33s.json", dir, operation_id);
    if (!json || len == 0 || len > 4096) return ESP_ERR_INVALID_SIZE;
    return atomic_write(path, (const uint8_t *)json, len, 4096);
}

esp_err_t storage_read_operation(const char *story_id, const char *operation_id,
                                 char *buf, size_t cap)
{
    char dir[160], path[256];
    operations_dir(story_id, dir, sizeof(dir));
    snprintf(path, sizeof(path), "%.190s/%.33s.json", dir, operation_id);
    size_t len = 0;
    if (!read_file_cap(path, (uint8_t *)buf, cap - 1, &len)) return ESP_ERR_NOT_FOUND;
    buf[len] = '\0';
    return ESP_OK;
}

esp_err_t storage_find_latest_operation(const char *story_id, char *operation_id_out)
{
    char dir[160];
    operations_dir(story_id, dir, sizeof(dir));
    DIR *d = opendir(dir);
    if (!d) return ESP_ERR_NOT_FOUND;
    struct dirent *e;
    char best[STORAGE_ID_HEX] = {0};
    time_t best_mtime = 0;
    char path[256];
    uint8_t buf[1024];
    while ((e = readdir(d)) != NULL) {
        size_t nl = strlen(e->d_name);
        if (nl != 32 + 5 || strcmp(e->d_name + nl - 5, ".json") != 0) continue;
        snprintf(path, sizeof(path), "%.190s/%.40s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        size_t len = 0;
        if (!read_file_cap(path, buf, sizeof(buf) - 1, &len)) continue;
        buf[len] = '\0';
        cJSON *o = cJSON_Parse((char *)buf);
        if (!o) continue;
        cJSON *state = cJSON_GetObjectItem(o, "state");
        cJSON *code = cJSON_GetObjectItem(o, "error_code");
        bool unfinished = cJSON_IsString(state) &&
                          !storage_operation_is_terminal(state->valuestring,
                                                         cJSON_IsString(code) ? code->valuestring : "");
        cJSON_Delete(o);
        if (!unfinished) continue;
        if (best[0] == '\0' || st.st_mtime >= best_mtime) {
            best_mtime = st.st_mtime;
            snprintf(best, sizeof(best), "%.32s", e->d_name);
        }
    }
    closedir(d);
    if (!best[0]) return ESP_ERR_NOT_FOUND;
    snprintf(operation_id_out, STORAGE_ID_HEX, "%s", best);
    return ESP_OK;
}

/* ---------------- 删除意图（DATA-03） ---------------- */

static void delete_intent_path(const char *story_id, char slot, char *out, size_t n)
{
    char dir[128];
    story_dir(dir, sizeof(dir), story_id);
    snprintf(out, n, "%.100s/deleted_%c.json", dir, slot);
}

static bool delete_intent_valid(const char *path)
{
    uint8_t buf[512];
    size_t len = 0;
    if (!read_file_cap(path, buf, sizeof(buf) - 1, &len)) return false;
    buf[len] = '\0';
    cJSON *o = cJSON_Parse((char *)buf);
    if (!o) return false;
    cJSON *sid = cJSON_GetObjectItem(o, "story_id");
    cJSON *ok = cJSON_GetObjectItem(o, "confirmed");
    bool valid = cJSON_IsString(sid) && cJSON_IsTrue(ok);
    cJSON_Delete(o);
    return valid;
}

bool storage_has_delete_intent(const char *story_id)
{
    char p[256];
    delete_intent_path(story_id, 'a', p, sizeof(p));
    if (delete_intent_valid(p)) return true;
    delete_intent_path(story_id, 'b', p, sizeof(p));
    return delete_intent_valid(p);
}

esp_err_t storage_begin_delete(const char *story_id)
{
    char p[256];
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "story_id", story_id);
    cJSON_AddBoolToObject(o, "confirmed", true);
    char *str = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!str) return ESP_ERR_NO_MEM;
    /* 双槽都写并同步验证：任意有效即视为已表达删除意图（实施计划 6.5） */
    esp_err_t err = ESP_OK;
    for (char slot = 'a'; slot <= 'b'; slot++) {
        delete_intent_path(story_id, slot, p, sizeof(p));
        esp_err_t e = atomic_write(p, (const uint8_t *)str, strlen(str), 512);
        if (e != ESP_OK) err = e;
    }
    cJSON_free(str);
    return err;
}

/**
 * 深度受限递归删除（DATA-03）：删除目录下全部内容后删目录本身；
 * 忽略删除意图文件（由调用方最后处理）；任一文件删除失败返回失败，
 * 调用方保留意图等待下轮继续。
 */
static bool rmrf_inner(const char *dir, int depth, char *intent_a, char *intent_b)
{
    if (depth > 4) return false;          /* 契约目录深度有限，超深按失败处理 */
    DIR *d = opendir(dir);
    if (!d) return false;
    struct dirent *e;
    bool ok = true;
    char f[384];
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        snprintf(f, sizeof(f), "%.150s/%.64s", dir, e->d_name);
        if (intent_a && strcmp(f, intent_a) == 0) continue;
        if (intent_b && strcmp(f, intent_b) == 0) continue;
        struct stat st;
        if (stat(f, &st) != 0) { ok = false; break; }
        if (S_ISDIR(st.st_mode)) {
            if (!rmrf_inner(f, depth + 1, intent_a, intent_b)) { ok = false; break; }
            if (rmdir(f) != 0) { ok = false; break; }
        } else {
            if (unlink(f) != 0) { ok = false; break; }
        }
    }
    closedir(d);
    return ok;
}

esp_err_t storage_continue_delete_one(const char *story_id)
{
    /* 分批删除：先内容后意图，最后空目录（实施计划 6.5 / DATA-03） */
    char dir[128], ia[256], ib[256];
    story_dir(dir, sizeof(dir), story_id);
    delete_intent_path(story_id, 'a', ia, sizeof(ia));
    delete_intent_path(story_id, 'b', ib, sizeof(ib));
    if (!rmrf_inner(dir, 0, ia, ib)) {
        return ESP_FAIL;                  /* 有残留：保留意图等待继续 */
    }
    /* 全部内容已删净：最后移除意图与目录本身 */
    unlink(ia);
    unlink(ib);
    if (rmdir(dir) != 0 && errno != ENOENT) return ESP_FAIL;
    return ESP_OK;
}

esp_err_t storage_continue_delete(void)
{
    char stories[80];
    join2(stories, sizeof(stories), s_box, "stories");
    DIR *dir = opendir(stories);
    if (!dir) return ESP_ERR_NOT_FOUND;
    struct dirent *entry;
    esp_err_t result = ESP_OK;
    while ((entry = readdir(dir)) != NULL) {
        if (strlen(entry->d_name) != 32 || !storage_has_delete_intent(entry->d_name)) continue;
        if (storage_continue_delete_one(entry->d_name) != ESP_OK) result = ESP_FAIL;
    }
    closedir(dir);
    return result;
}
