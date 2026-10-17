# 板卡适配层：唯一允许触碰 BSP/引脚/方向 的组件
idf_component_register(
    SRCS "src/board_adapter.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_lcd esp_video esp_wifi_remote lvgl esp_codec_dev sdmmc fatfs
             espressif__esp_lcd_st7796 espressif__esp_lcd_touch_ft5x06
             esp_driver_i2c esp_driver_i2s esp_driver_ledc esp_driver_spi
    PRIV_REQUIRES esp_timer esp_hw_support esp_driver_sdmmc esp_driver_sdspi esp_driver_spi
    PRIV_INCLUDE_DIRS "../../managed_components/espressif__esp_video/private_include"
)
