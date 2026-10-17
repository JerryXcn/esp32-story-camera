# NVS 设置服务
idf_component_register(
    SRCS "src/settings_service.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash
)
