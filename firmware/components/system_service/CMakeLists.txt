# 系统服务：时钟质量、堆统计、复位原因
idf_component_register(
    SRCS "src/system_service.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer
)
