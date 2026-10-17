# UI 组件：主题、资源、屏幕（LVGL）
idf_component_register(
    SRCS "src/app_ui.c" "src/ui_theme.c" "src/ui_assets.c" "generated/assets.c"
         "src/ui_screens_boot_camera.c" "src/ui_screens_generate_player.c"
         "src/ui_screens_library_detail.c" "src/ui_screens_settings_net.c"
         "src/ui_dialogs.c" "src/ui_thumbs.c"
         "src/ui_widgets.c" "src/ui_sound.c" "src/ui_photo.c" "src/ui_screen_home.c" "src/ui_screen_server.c" "src/ui_camera_direct.c" "src/ui_story_ops.c"
         "fonts/ui_font_16.c" "fonts/ui_font_22.c"
         "fonts/ui_kid_24.c" "fonts/ui_kid_32.c" "fonts/ui_kid_44.c"
    INCLUDE_DIRS "include" "generated"
    REQUIRES lvgl board_adapter story_service settings_service network_service storage_service
    PRIV_REQUIRES api_client esp-tls audio_service system_service esp_driver_jpeg esp_driver_ppa esp_app_format
)
