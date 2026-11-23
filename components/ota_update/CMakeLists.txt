idf_component_register(
    SRCS "ota_update.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        app_update
        esp_app_format
        esp_partition
        esp_https_ota
        esp_http_client
        esp-tls
        display
)
