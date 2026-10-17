# API 客户端：云端任务 V1 契约
idf_component_register(
    SRCS "src/api_client.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client mbedtls cjson settings_service
)
# TLS 一律走证书包（ESP-IDF 公共根证书）。局域网自签名联调：server/scripts/gen_dev_certs.sh
# 会把开发 CA 写到本目录 server_ca.pem（不入库），并在 menuconfig 开启
# CONFIG_MBEDTLS_CUSTOM_CERTIFICATE_BUNDLE（路径 components/api_client/server_ca.pem）。
