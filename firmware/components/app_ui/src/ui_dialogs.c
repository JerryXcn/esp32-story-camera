/**
 * @file ui_dialogs.c
 * @brief 弹窗（画面12，v0.2）：错误、内容被拒、删除确认、重新生成确认。
 *
 * 只是告知时只有一个"知道了"；能恢复时是"稍后"+"重试"（重试先查原任务，不重复生成）；
 * 内容被拒时显示是哪张照片，给"删掉这张"和"重拍"。文案按实施计划第 10 节错误映射。
 */

#include "ui_screens.h"
#include "ui_story_ops.h"
#include "ui_assets.h"
#include "ui_widgets.h"
#include "ui_sound.h"
#include "app_ui.h"
#include "story_service.h"
#include "storage_service.h"

#include "lvgl.h"
#include <string.h>

void ui_detail_regen_confirmed(void);

/* 业务码 -> 文案（UI 使用本地映射，不直出 ESP 错误） */
typedef struct {
    const char *code;
    const char *title;
    const char *body;
    bool retryable;
} err_text_t;

static const err_text_t k_err_texts[] = {
    { "WIFI_PASSWORD_INVALID", "密码长度不正确", "请输入 8 至 64 位密码。", false },
    { "SD_IO_ERROR",          "保存失败",       "请检查存储卡后再试。", true },
    { "NETWORK_TIMEOUT",      "网络开小差了",   "照片已经保存好了。可以现在重试，也可以稍后在故事库里继续。", true },
    { "SD_NOT_FOUND",         "存储卡不可用",   "请检查存储卡是否插好。", false },
    { "STORAGE_FULL",         "空间不足",       "删除一些旧故事后再试。", false },
    { "STORY_LIMIT",          "数量已达上限",   "删除一些旧故事后再试。", false },
    { "VERSION_LIMIT",        "版本已达上限",   "这条故事的新版本太多了。", false },
    { "CAMERA_FAILED",        "相机不可用",     "请重新启动后再试。", false },
    { "IMAGE_INVALID",        "照片不能使用",   "请重新拍一张试试。", false },
    { "QUOTA_EXCEEDED",       "生成次数已用完", "今天的故事额度用完啦，明天再来吧。", false },
    { "PROVIDER_RESULT_UNKNOWN", "结果暂时无法确认", "可以重试，可能会重新处理。", true },
    { "TTS_FAILED",           "声音没准备好",   "故事内容已保存，可以重试声音。", true },
    { "AI_FAILED",            "故事没准备好",   "稍后再试试吧。", true },
    { "DOWNLOAD_INVALID",     "声音文件不完整", "可以重新下载。", true },
    { "AUDIO_DECODE_FAILED",  "声音播放失败",   "这个文件暂时不能播放。", false },
    { "AUTH_FAILED",          "设备服务不可用", "已有的故事仍然可以听，请稍后再试。", false },
    { "AUDIO_TOO_LONG",       "故事太长了",     "把故事长度调短一点再试试。", false },
    { "DEVICE_BUSY",          "还有故事在生成", "等上一个故事好了再试。", true },
    { "RESULT_EXPIRED",       "结果已过期",     "请重新生成这个故事。", false },
    { "OFFLINE",              "还没联网",       "连上 Wi-Fi 后再来生成故事吧。照片已经保存好了。", false },
    { "TIMEOUT",              "网络开小差了",   "照片已经保存好了。可以现在重试，也可以稍后在故事库里继续。", true },
};

/* 弹窗上下文：绑定真实动作 */
static char s_retry_story[STORAGE_ID_HEX];

static void on_dialog_later(lv_event_t *e)
{
    lv_obj_delete_async(lv_event_get_user_data(e));
    if (ui_navigator_current() == SCREEN_GENERATING) ui_navigator_go(SCREEN_HOME);
}

static void on_dialog_info_ok(lv_event_t *e)
{
    lv_obj_delete_async(lv_event_get_user_data(e));
    if (ui_navigator_current() == SCREEN_GENERATING) ui_navigator_go(SCREEN_HOME);
}

/** 重试 = 对当前错误故事继续原任务（FLOW-05：先查原任务不重复生成） */
static void on_dialog_retry(lv_event_t *e)
{
    lv_obj_delete_async(lv_event_get_user_data(e));
    if (s_retry_story[0] && story_resume_generation(s_retry_story) == ESP_OK) {
        screen_generating_set_story_id(s_retry_story);
        ui_navigator_go(SCREEN_GENERATING);
        screen_generating_on_enter();  /* 已在生成中页时重置步骤 */
    } else if (ui_navigator_current() == SCREEN_GENERATING) {
        ui_navigator_go(SCREEN_HOME);
    }
}

void ui_dialog_show_error_cb(const char *code, bool retryable, const char *story_id)
{
    const err_text_t *found = NULL;
    for (size_t i = 0; i < sizeof(k_err_texts) / sizeof(k_err_texts[0]); i++) {
        if (strcmp(k_err_texts[i].code, code) == 0) { found = &k_err_texts[i]; break; }
    }
    static char title[64], body[160];
    if (found) {
        strlcpy(title, found->title, sizeof(title));
        strlcpy(body, found->body, sizeof(body));
        retryable = retryable && found->retryable;
    } else {
        strlcpy(title, "出了点问题", sizeof(title));
        strlcpy(body, "请稍后再试。", sizeof(body));
    }
    strlcpy(s_retry_story, story_id ? story_id : "", sizeof(s_retry_story));
    ui_dialog_spec_t spec = { .title = title, .body = body };
    if (retryable && s_retry_story[0]) {
        spec.left = "稍后";
        spec.left_cb = on_dialog_later;
        spec.right = "重试";
        spec.right_cb = on_dialog_retry;
        spec.right_face = UI_COL_SKY;
        spec.right_edge = UI_COL_SKY_DK;
    } else {
        spec.right = "知道了";
        spec.right_cb = on_dialog_info_ok;
    }
    ui_dialog(&spec);
}

void ui_dialog_show_error(const char *code, bool retryable)
{
    ui_dialog_show_error_cb(code, retryable, app_ui_current_story()->story_id);
}

/* ---------------- 内容被拒 ---------------- */

static char s_reject_story[STORAGE_ID_HEX];

static void on_reject_delete(lv_event_t *e)
{
    lv_obj_delete_async(lv_event_get_user_data(e));
    if (s_reject_story[0]) ui_ops_delete((const char (*)[STORAGE_ID_HEX])&s_reject_story, 1, NULL);
    ui_navigator_go(SCREEN_CAMERA);
}

static void on_reject_retake(lv_event_t *e)
{
    lv_obj_delete_async(lv_event_get_user_data(e));
    ui_navigator_go(SCREEN_CAMERA);
}

void ui_dialog_show_rejected(const char *story_id)
{
    strlcpy(s_reject_story, story_id ? story_id : "", sizeof(s_reject_story));
    char thumb[192];
    const lv_image_dsc_t *photo = NULL;
    if (s_reject_story[0] && storage_get_thumb_path(s_reject_story, thumb, sizeof(thumb)) == ESP_OK)
        photo = ui_thumb_get(thumb);
    ui_dialog_spec_t spec = {
        .title = "这一页讲不了",
        .body = "换一页绘本试试吧。",
        .photo = photo,
        .left = "删掉这张", .left_cb = on_reject_delete, .left_danger = true,
        .right = "重拍", .right_cb = on_reject_retake,
    };
    ui_dialog(&spec);
    ui_sound_prompt(UI_PROMPT_REJECTED);
}

/* ---------------- 删除确认 ---------------- */

static char s_delete_target[STORAGE_ID_HEX];

static void on_single_deleted(int deleted)
{
    (void)deleted;
    if (ui_navigator_current() == SCREEN_DETAIL) ui_navigator_go(SCREEN_LIBRARY);
}

static void on_delete_yes(lv_event_t *e)
{
    lv_obj_delete_async(lv_event_get_user_data(e));
    /* 后台删除：加载遮罩 → 从列表移除 → 回到故事库并提示"已删除" */
    ui_ops_delete((const char (*)[STORAGE_ID_HEX])&s_delete_target, 1, on_single_deleted);
}

void ui_dialog_confirm_delete(const char *story_id)
{
    strlcpy(s_delete_target, story_id, sizeof(s_delete_target));
    ui_dialog_spec_t spec = {
        .title = "删除这条故事？",
        .body = "照片和故事都会删掉，在设备里就找不到了。",
        .left = "取消",
        .right = "删除", .right_cb = on_delete_yes,
        .right_face = UI_COL_DANGER, .right_edge = 0xC2453E,
    };
    ui_dialog(&spec);
}

/* ---------------- 重新生成确认 ---------------- */

static void on_regen_yes(lv_event_t *e)
{
    lv_obj_delete_async(lv_event_get_user_data(e));
    ui_detail_regen_confirmed();
}

void ui_dialog_confirm_regen(const char *story_id)
{
    (void)story_id;
    ui_dialog_spec_t spec = {
        .title = "再编一个新故事？",
        .body = "会根据这张照片重新创作。现在的故事也会保留。",
        .left = "取消",
        .right = "重新编", .right_cb = on_regen_yes,
    };
    ui_dialog(&spec);
}
