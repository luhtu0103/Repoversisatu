idf_component_register(
    SRCS "ota_update.cpp"
    INCLUDE_DIRS "include"
    REQUIRES app_update esp_http_client esp_https_ota mbedtls
)
