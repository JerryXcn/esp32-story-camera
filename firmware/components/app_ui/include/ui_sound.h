#pragma once
/**
 * @file ui_sound.h
 * @brief 界面声音：点击音、快门声、语音提示（音频内置固件，经 audio_service 播放）。
 *
 * 点击音/快门声只在没有故事播放时发声，不打断故事；语音提示会打断其他提示音。
 * 静音调试构建（CONFIG_STORY_DEBUG_SILENT）下流程照常但不出声。
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_PROMPT_LOOKING,   /* "我来看看这一页～"（开始生成） */
    UI_PROMPT_READY,     /* "故事准备好啦！"（生成完成，随后自动播放） */
    UI_PROMPT_DONE,      /* "讲完啦，还想再听一遍吗？" */
    UI_PROMPT_REJECTED,  /* "这一页暂时讲不了，换一页试试吧" */
} ui_prompt_t;

void ui_sound_click(void);
void ui_sound_shutter(void);
void ui_sound_prompt(ui_prompt_t prompt);
/** 最近一次语音提示是否仍在播放（用于"提示语说完再自动播放故事"） */
bool ui_sound_prompt_busy(void);

#ifdef __cplusplus
}
#endif
