# UART 调试控制台（screenshot / tap / touch / goto / info / camera / nettest ...）
idf_component_register(
    SRCS "src/debug_console.c"
    INCLUDE_DIRS "include"
    REQUIRES app_ui board_adapter camera_service settings_service api_client audio_service network_service storage_service
    PRIV_REQUIRES esp_http_client esp-tls api_client esp_driver_uart esp_driver_usb_serial_jtag
                  spi_flash espressif__esp_hosted
)
