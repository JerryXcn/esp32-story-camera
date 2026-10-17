/**
 * @file ui_sound.c
 * @brief 界面声音（见 ui_sound.h）。
 */

#include "ui_sound.h"
#include "audio_service.h"

/* 故事在播放/暂停、或语音提示正在说时不发点击音/快门声：新片段会打断（取消）当前播放 */
static bool story_busy(void)
{
    audio_state_t st = audio_get_state();
    if (audio_prompt_active() >= 0) return true;
    if (audio_clip_active()) return false;
    return st == AUDIO_PLAYING || st == AUDIO_PAUSED || st == AUDIO_LOADING;
}

void ui_sound_click(void)
{
    if (!story_busy()) audio_play_clip(AUDIO_CLIP_CLICK);
}

void ui_sound_shutter(void)
{
    if (!story_busy()) audio_play_clip(AUDIO_CLIP_SHUTTER);
}

void ui_sound_prompt(ui_prompt_t prompt)
{
    static const audio_clip_t map[] = {
        [UI_PROMPT_LOOKING] = AUDIO_CLIP_PROMPT_LOOKING,
        [UI_PROMPT_READY] = AUDIO_CLIP_PROMPT_READY,
        [UI_PROMPT_DONE] = AUDIO_CLIP_PROMPT_DONE,
        [UI_PROMPT_REJECTED] = AUDIO_CLIP_PROMPT_REJECTED,
    };
    audio_play_clip(map[prompt]);
}

bool ui_sound_prompt_busy(void)
{
    return audio_clip_active();
}
