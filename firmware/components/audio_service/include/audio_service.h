#pragma once
/**
 * @file audio_service.h
 * @brief 本地播放（实施计划 9.3）。
 * 单个工作任务拥有文件、解码器和 codec；UI 通过命令切换会话。
 * 进度由 codec 成功接收的 PCM 样本驱动；暂停不计时。
 * P0 能力：播放、暂停、恢复、从头、音量；进度只读，不支持 Seek。
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AUDIO_STOPPED = 0,
    AUDIO_PLAYING,
    AUDIO_PAUSED,
    AUDIO_ERROR,
    AUDIO_LOADING,
} audio_state_t;

typedef void (*audio_state_cb)(audio_state_t state, void *user);

esp_err_t audio_init(audio_state_cb cb, void *user);

/** 播放本地 MP3（24kHz/mono 契约格式；播放前先验证可解码由 story_service 负责）。
 *  非阻塞：启动成功即返回，结束后回调 AUDIO_STOPPED。 */
esp_err_t audio_play_file(const char *path);
esp_err_t audio_play_file_with_duration(const char *path, uint32_t duration_ms);
/** UART diagnostic: 3-second audible reference, executed by the same audio worker. */
esp_err_t audio_play_test_tone(void);
/** UART diagnostic: generated 6-second MP3 through the normal decoder/output. */
esp_err_t audio_play_test_mp3(void);

/** 内置音频片段（clips 目录下的 mp3）：界面音效与语音提示，与故事同一条解码通路，会打断当前播放 */
typedef enum {
    AUDIO_CLIP_CLICK = 0,
    AUDIO_CLIP_SHUTTER,
    AUDIO_CLIP_PROMPT_LOOKING,
    AUDIO_CLIP_PROMPT_READY,
    AUDIO_CLIP_PROMPT_DONE,
    AUDIO_CLIP_PROMPT_REJECTED,
    AUDIO_CLIP_COUNT,
} audio_clip_t;
esp_err_t audio_play_clip(audio_clip_t clip);
/** 当前命令是片段且仍在加载/播放 */
bool audio_clip_active(void);
/** 正在播放（或排队中）的语音提示片段 ID（PROMPT_*）；没有则返回 -1。点击/快门音不应打断它 */
int audio_prompt_active(void);

/** 当前播放状态（UI 判定是否已在播放，避免重复开会话） */
audio_state_t audio_get_state(void);

void audio_pause(void);
void audio_resume(void);
bool audio_is_quiescent(void);
void audio_stop(void);            /* 清空队列并按硬件顺序静音，防旧 PCM 混入新故事 */

/** 只读进度（0..1000）与总时长；离开播放页由 UI 停止 */
int audio_progress_permille(void);
uint32_t audio_duration_ms(void);
void audio_set_duration_ms(uint32_t ms);   /* 提交路径从 version.json 读取后注入 */

void audio_set_volume(uint8_t vol /* 0..100 */);

#ifdef __cplusplus
}
#endif
