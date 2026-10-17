# 网络服务：C6 Wi-Fi、扫描、连接、校时
idf_component_register(
    SRCS "src/network_service.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi_remote esp_wifi esp_netif esp_event nvs_flash settings_service system_service lwip
)
