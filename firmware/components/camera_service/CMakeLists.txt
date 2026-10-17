# 相机服务：采集/编码/帧生命周期
idf_component_register(
    SRCS "src/camera_service.c"
    INCLUDE_DIRS "include"
    REQUIRES board_adapter
    PRIV_REQUIRES esp_video esp_driver_ppa esp_driver_jpeg esp_mm
)
