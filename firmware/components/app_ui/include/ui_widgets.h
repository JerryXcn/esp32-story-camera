#pragma once
/**
 * @file ui_widgets.h
 * @brief v0.2 公共组件（图标、按钮、弹窗、提示等）。
 *
 * 所有组件从 lv_obj_remove_style_all 起步，不依赖 LVGL 默认主题；
 * 按钮类组件统一挂点击音（ui_sound_click），按下时有下沉/缩小反馈。
 */

#include "lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_IC_HOME, UI_IC_CAMERA, UI_IC_BOOKS, UI_IC_BACK, UI_IC_NEXT, UI_IC_PLAY, UI_IC_PAUSE,
    UI_IC_REPLAY, UI_IC_MORE, UI_IC_VOLUME, UI_IC_WIFI, UI_IC_WIFI_OFF, UI_IC_SPARK,
    UI_IC_CHECK, UI_IC_TRASH, UI_IC_REFRESH, UI_IC_CLOSE, UI_IC_HEART, UI_IC_SKIP_PREV, UI_IC_SKIP_NEXT, UI_IC_HEART_F,
    UI_IC_COUNT,
    UI_IC_NONE = -1,
} ui_icon_t;

typedef enum {
    UI_ROUND_GLASS,     /* 白色半透明（叠在照片上） */
    UI_ROUND_OUTLINE,   /* 白底 + 浅描边（奶油色页面上） */
    UI_ROUND_CREAM,     /* 奶油底 + 浅描边（白色控制条里） */
} ui_round_style_t;

/** A8 图标（尺寸取 20/24/28/40 中不小于 size 的最小一档），color 着色 */
lv_obj_t *ui_icon_create(lv_obj_t *parent, ui_icon_t ic, int size, uint32_t color);
void ui_icon_set(lv_obj_t *icon, ui_icon_t ic, int size);

/** 圆形图标按钮 */
lv_obj_t *ui_round_btn(lv_obj_t *parent, ui_icon_t ic, int diameter, ui_round_style_t style,
                       lv_event_cb_t cb, void *user);
/** 更换圆形按钮的图标与颜色 */
void ui_round_btn_set_icon(lv_obj_t *btn, ui_icon_t ic, uint32_t color);
/** 收藏按钮状态：实心粉心 / 空心深色心 */
void ui_heart_btn_set(lv_obj_t *btn, bool favorite);

/** 带"厚度"的胶囊按钮：face 面色、edge 底边深色；ic 可为 UI_IC_NONE。返回按钮，文字为最后一个子对象 */
lv_obj_t *ui_chunky_btn(lv_obj_t *parent, ui_icon_t ic, const char *text, uint32_t face, uint32_t edge,
                        uint32_t text_color, int w, int h, lv_event_cb_t cb, void *user);
void ui_chunky_set(lv_obj_t *btn, ui_icon_t ic, const char *text, uint32_t face, uint32_t edge);

/** 首页入口块（216x114）：贴纸插画 + 快乐体大字 + 小字说明；返回块，说明标签可用 ui_tile_set_sub 更新 */
lv_obj_t *ui_tile(lv_obj_t *parent, const lv_image_dsc_t *art, const char *title, const char *sub,
                  uint32_t face, uint32_t edge, lv_event_cb_t cb, void *user);
void ui_tile_set_sub(lv_obj_t *tile, const char *sub);

/** 页面标题栏：左上圆形返回键（back_ic）+ 快乐体标题；返回标题标签 */
lv_obj_t *ui_page_header(lv_obj_t *parent, ui_icon_t back_ic, const char *title, lv_event_cb_t back_cb);

/** 状态胶囊：Wi-Fi 图标 + 时间（未联网显示"未联网"），自带 2 秒刷新定时器 */
lv_obj_t *ui_status_pill(lv_obj_t *parent, bool on_photo);

/** 分段按钮：labels 共 n 项，active 为选中项；changed 回调的 user_data 为选中下标 (intptr_t) */
lv_obj_t *ui_seg(lv_obj_t *parent, const char *const *labels, int n, int active, lv_event_cb_t changed);
void ui_seg_select(lv_obj_t *seg, int index);

/** 统一弹窗：狐狸 + 标题 + 正文 + 1~2 个按钮；photo 非空时在标题旁显示照片小图。
 *  按钮回调的 user_data 为遮罩对象（回调里自行 lv_obj_delete）。返回遮罩。 */
typedef struct {
    const char *title;
    const char *body;
    const lv_image_dsc_t *photo;
    const char *left;           /* NULL = 只有一个按钮 */
    lv_event_cb_t left_cb;
    const char *right;
    lv_event_cb_t right_cb;
    uint32_t right_face, right_edge;
    bool left_danger;           /* 左按钮红字（如"删掉这张"） */
} ui_dialog_spec_t;
lv_obj_t *ui_dialog(const ui_dialog_spec_t *spec);
void ui_dialog_close_cb(lv_event_t *e);   /* 通用"关闭"回调（user_data 为遮罩） */

/** 图片"铺满裁切"显示在 box_w x box_h（居中缩放；裁切依赖同尺寸的父容器） */
void ui_image_cover(lv_obj_t *img, int32_t box_w, int32_t box_h);

/** 单行标签按文字实际宽度定宽（不超过 max_w，超出显示省略号）。
 *  LVGL 的 LONG_DOT 需要固定宽高，放在自适应宽度的容器里会算成 0 宽 */
void ui_label_fit(lv_obj_t *label, const char *text, int32_t max_w);

/** 加载遮罩：半透明底 + 白卡片（转圈 + 文字），吞掉背后点击；返回遮罩，文字可用 ui_loading_set_text 更新 */
lv_obj_t *ui_loading_show(const char *text);
void ui_loading_set_text(lv_obj_t *loading, const char *text);

/** 底部轻提示（绿色对勾 + 文字），约 1.8 秒后淡出并自动删除 */
void ui_toast(const char *text);

/** 给任意可点击对象挂点击音 */
void ui_add_click_sound(lv_obj_t *obj);

#ifdef __cplusplus
}
#endif
