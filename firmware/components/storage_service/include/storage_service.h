#pragma once
/**
 * @file storage_service.h
 * @brief TF 卡数据契约（实施计划 6.1~6.5）。
 *
 * 卡内布局：
 *   /storybox/index.json                    可重建列表缓存（不是业务真相）
 *   /storybox/stories/<story_id>/
 *       capture.json        原图恢复锚点，首次保存后不修改
 *       original.jpg        不可变原图
 *       thumb.jpg           240x160（v0.2 起 3:2；旧记录 240x180），可重建
 *       metadata_a.json / metadata_b.json   双槽交替提交，带 generation+checksum
 *       operations/<operation_id>.json      请求参数快照与状态
 *       staging/<operation_id>/             临时文件（ai_input.jpg / *.part）
 *       versions/<version>/story.txt|audio.mp3|version.json
 *       deleted_a.json / deleted_b.json     删除意图双槽
 *
 * 所有磁盘路径由本组件构造并限定在 story 目录内；模型标题、SSID 等
 * 任意字符串绝不参与路径拼接。story_id/operation_id 为 128-bit 随机
 * ID 的 32 位十六进制。
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STORAGE_ID_HEX 33
#define STORAGE_MAX_STORIES 1000
#define STORAGE_MAX_VERSIONS 10
#define STORAGE_ORIGINAL_MAX_BYTES (4 * 1024 * 1024)
#define STORAGE_THUMB_MAX_BYTES (128 * 1024)
#define STORAGE_AUDIO_MAX_BYTES (2 * 1024 * 1024)
#define STORAGE_STORY_TEXT_MAX_BYTES (12 * 1024)

/** 故事可用性（实施计划 6.3，独立于操作状态） */
typedef enum {
    STORY_AVAIL_PHOTO_ONLY = 0,
    STORY_AVAIL_PLAYABLE = 1,
    STORY_AVAIL_CORRUPT = 2,
    STORY_AVAIL_DELETING = 3,
} story_availability_t;

typedef struct {
    char story_id[STORAGE_ID_HEX];
    uint32_t capture_seq;
    bool has_time;                    /* created_at 是否可信 */
    int64_t created_at_epoch;         /* time_source=synced 才有效 */
    story_availability_t availability;
    int current_version;              /* 0 = 尚无已提交版本 */
    char title[64];                   /* 显示标题；缺字回退由 UI 处理 */
    char thumb_path[128];             /* 绝对路径，可空 */
    bool favorite;                    /* 已收藏（故事目录下有 favorite 标记文件） */
} story_summary_t;

/** 初始化挂载点下的 storybox 目录；返回挂载点。 */
esp_err_t storage_init(const char *mount_point);

/* ---------- 写入协议（实施计划 6.4） ---------- */

/** 生成不碰撞的随机 ID（先检查卡上不存在再返回）。 */
esp_err_t storage_new_story_id(char out[STORAGE_ID_HEX]);
esp_err_t storage_new_operation_id(char out[STORAGE_ID_HEX]);

/** 1) 原图写 .part -> 校验 JPEG -> flush/sync -> rename original.jpg */
esp_err_t storage_commit_original(const char *story_id, const uint8_t *jpeg,
                                  size_t len, uint32_t width, uint32_t height);

/** 2) capture.json 锚点；写入并同步、回读验证后才可报“照片已保存”。
 *  原图尺寸与真实 SHA-256 由本组件对落盘文件重算（不信任调用方占位）。 */
esp_err_t storage_commit_capture_anchor(const char *story_id, uint32_t capture_seq,
                                        const uint8_t *jpeg_sha256);

/** 缩略图落盘（可重建；失败不回滚原图）。jpeg 为 240x160 缩略图 JPEG（旧记录为 240x180）。 */
esp_err_t storage_commit_thumb(const char *story_id, const uint8_t *jpeg, size_t len);

/** 逐块计算文件 SHA-256（64 hex，含 NUL 需 65 字节）；失败显式报错。 */
esp_err_t storage_file_sha256(const char *path, char *hex_out);

/** 解析 JPEG SOF 段取真实像素尺寸；失败报错（不猜尺寸）。 */
esp_err_t storage_jpeg_dimensions(const char *path, uint32_t *w, uint32_t *h);

/** 空间预检：剩余 >= 本次最坏新增 + 8MiB 安全余量（实施计划 6.5） */
esp_err_t storage_precheck_space(bool is_regen);

/** staging 与版本提交 */
esp_err_t storage_stage_write(const char *story_id, const char *operation_id,
                              const char *filename, const uint8_t *data, size_t len);
esp_err_t storage_stage_discard(const char *story_id, const char *operation_id);

/** 4)~6) 完整校验后提交新版本：staging/<op>/ 的 story.txt+audio.mp3 必须就位；
 *  本组件流式计算两者真实 SHA-256（哈希失败不得提交），移动到 versions/<n>/
 *  并写 version.json，最后切换双槽 metadata。audio_bytes_expected 为
 *  result 描述的音频字节数，与实际不符报 ESP_ERR_INVALID_CRC。 */
esp_err_t storage_commit_version(const char *story_id, const char *operation_id,
                                 const char *title, size_t audio_bytes_expected,
                                 uint32_t duration_ms, int *new_version_out);

/** 当前已提交且引用完整的版本号；无版本 -ESP_ERR_NOT_FOUND，损坏 -INVALID_STATE */
esp_err_t storage_get_current_version(const char *story_id);

/* ---------- operation 持久化（FLOW-05：重启/断电可恢复原任务） ---------- */

/** 原子写 operations/<op_id>.json（内容为调用方序列化的 JSON）。 */
esp_err_t storage_write_operation(const char *story_id, const char *operation_id,
                                  const char *json, size_t len);

/** 读取 operation JSON（NUL 结尾）；不存在返回 ESP_ERR_NOT_FOUND。 */
esp_err_t storage_read_operation(const char *story_id, const char *operation_id,
                                 char *buf, size_t cap);

/** operation 是否已到终态：completed、closed（不可重试失败），以及旧固件写下的
 *  failed + CONTENT_REJECTED。终态不再提供"继续生成"，staging 可清理。 */
bool storage_operation_is_terminal(const char *state, const char *error_code);

/** 找该故事最近一个未到终态的 operation。
 *  比较 operations/ 下各 JSON 的修改时间；无候选返回 ESP_ERR_NOT_FOUND。 */
esp_err_t storage_find_latest_operation(const char *story_id, char *operation_id_out);

/* ---------- 读取 / 恢复（实施计划 6.5） ---------- */

/** 启动恢复：扫描 stories/，双槽 metadata 校验，孤立原图重建待生成记录。
 *  回填 summary 列表（按 capture_seq 倒序），max_out 为容量，返回条数。 */
int storage_recover_all(story_summary_t *out, int max_out);
esp_err_t storage_get_summary(const char *story_id, story_summary_t *out);
esp_err_t storage_get_playback_info(const char *story_id, char *audio_path, size_t cap, uint32_t *duration_ms);
esp_err_t storage_get_audio_duration(const char *story_id, uint32_t *duration_ms);

/** 读取当前版本音频路径（current_version 不可播放时返回 ESP_ERR_NOT_FOUND） */
esp_err_t storage_get_audio_path(const char *story_id, char *buf, size_t buflen);
esp_err_t storage_get_photo_path(const char *story_id, char *buf, size_t buflen);
esp_err_t storage_get_thumb_path(const char *story_id, char *buf, size_t buflen);
esp_err_t storage_get_story_text(const char *story_id, char *buf, size_t buflen);

/* ---------- 收藏（目录下 favorite 标记文件；删除故事时随目录清理） ---------- */
esp_err_t storage_set_favorite(const char *story_id, bool favorite);

/* ---------- 删除（实施计划 6.5：先意图后清理，分批） ---------- */
esp_err_t storage_begin_delete(const char *story_id);    /* 写删除意图并同步 */
esp_err_t storage_continue_delete(void);                 /* 重启后续清理 */
bool storage_has_delete_intent(const char *story_id);

/* ---------- 状态快照 ---------- */
uint32_t storage_story_count(void);

#ifdef __cplusplus
}
#endif
