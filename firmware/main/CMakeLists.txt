# 应用主组件
idf_component_register(
    SRCS "src/main.c" "src/app_context.c"
    INCLUDE_DIRS "include"
    REQUIRES board_adapter app_ui story_service storage_service settings_service
             network_service system_service audio_service camera_service debug_console
             nvs_flash
)

# 强制保留 esp_hosted 的 idempotent wrap 实现：旧版 esp_video（2.0.x，为兼容
# rev v1.3/eco2 而锁定）不引用它们，静态库会丢弃，导致 __wrap_esp_netif_init
# 与 __wrap_esp_event_loop_create_default 未定义（链接失败）。
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--undefined=__wrap_esp_netif_init"
    "-Wl,--undefined=__wrap_esp_event_loop_create_default"
)
