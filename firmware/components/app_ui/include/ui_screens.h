#pragma once
/**
 * @file ui_screens.h
 * @brief 屏幕注册与导航（内部头）。
 */

#include "lvgl.h"
#include "ui_theme.h"
#include "storage_service.h"
#include "story_service.h"

typedef enum {
    SCREEN_BOOT = 0,
    SCREEN_CAMERA,      /* 拍摄（含拍照中遮罩） */
    SCREEN_GENERATING,
    SCREEN_PLAYER,
    SCREEN_LIBRARY,     /* 含空库状态 */
    SCREEN_DETAIL,
    SCREEN_SETTINGS,
    SCREEN_WIFI,
    SCREEN_PASSWORD,
    SCREEN_HOME,        /* v0.2 首页：四个大入口（追加在末尾，调试 goto 编号不变） */
    SCREEN_SERVER,      /* 设置 → 服务器地址（主机 + 端口） */
} screen_id_t;

typedef struct {
    lv_obj_t *(*create)(void);
    void (*on_enter)(void);
    void (*on_exit)(void);
    lv_obj_t *root;
} screen_t;

void ui_navigator_init(lv_obj_t *parent);
void ui_navigator_go(screen_id_t id);          /* 推栈式切换 */
void ui_navigator_back(void);
screen_id_t ui_navigator_current(void);

/* 屏幕根对象的父容器（由导航器设置；屏幕必须作为其子对象创建，
 *  不能用 lv_obj_create(NULL)——LVGL9 不允许 reparent 屏幕，M0 实测） */
void ui_screens_set_parent(lv_obj_t *parent);
lv_obj_t *ui_screens_parent(void);

/* 各屏幕构造函数（每个文件一组） */
lv_obj_t *screen_boot_create(void);
void screen_boot_set_status(const char *text);
lv_obj_t *screen_camera_create(void);
lv_obj_t *screen_generating_create(void);
lv_obj_t *screen_player_create(void);
lv_obj_t *screen_library_create(void);
lv_obj_t *screen_detail_create(void);
lv_obj_t *screen_settings_create(void);
lv_obj_t *screen_wifi_create(void);
lv_obj_t *screen_password_create(void);
lv_obj_t *screen_home_create(void);
void screen_home_on_enter(void);
lv_obj_t *screen_server_create(void);
void screen_server_on_enter(void);
/** https://host[:port] 拆成主机与端口（缺省 443） */
void ui_server_split(const char *url, char *host, size_t host_len, int *port);

/* 屏幕间通知 */
void screen_camera_on_story_event(const story_event_t *ev);
void screen_camera_update_availability(void);   /* 快门可用性/预览状态刷新 */
void screen_camera_show_consent(void);          /* 家长首次联网说明弹窗 */
void screen_camera_on_enter(void);              /* 进入启动预览（CAM-01 生命周期） */
void screen_camera_on_exit(void);               /* 离开停止取流释放资源 */
void screen_wifi_on_enter(void);                /* 进入触发异步扫描（FLOW-07） */
void screen_wifi_on_exit(void);
void screen_player_on_exit(void);
void screen_player_on_enter(void);               /* 离开播放页停止播放（9.3） */
void screen_generating_on_enter(void);          /* 进入重置步骤显示 */
void screen_generating_on_exit(void);
/** 生成中页面背景照片按故事 ID 加载（事件或入口调用；同一故事不重复加载） */
void screen_generating_set_story_id(const char *story_id);
void screen_generating_on_story_event(const story_event_t *ev);
void screen_player_open(const story_summary_t *s);
/** 生成完成后的自动播放：先说"故事准备好啦！"再播故事 */
void screen_player_open_with_intro(const story_summary_t *s);
void screen_detail_open(const story_summary_t *s);
void screen_library_reload(void);
void screen_library_on_enter(void);
/** 打开故事库：favorites=true 只看收藏；退出选择模式、回到第一页 */
void screen_library_open(bool favorites);
/** 播放页切换：当前视图里 story_id 的上一个（dir<0）/下一个（dir>0）可播放故事（循环）；没有返回 false */
bool screen_library_neighbor(const char *story_id, int dir, story_summary_t *out);

void screen_settings_on_enter(void);
void screen_password_on_enter(void);
void screen_password_on_exit(void);
