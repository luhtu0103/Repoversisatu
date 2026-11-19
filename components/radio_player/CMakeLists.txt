idf_component_register(
    SRCS "radio_player.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES audio_hal display servo_head esp_http_client esp-tls mbedtls cjson esp_timer
)
