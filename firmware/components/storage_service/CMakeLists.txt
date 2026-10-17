# TF 存储契约实现（实施计划第 6 节）
idf_component_register(
    SRCS "src/storage_service.c"
    INCLUDE_DIRS "include"
    REQUIRES cjson mbedtls fatfs settings_service system_service
)
