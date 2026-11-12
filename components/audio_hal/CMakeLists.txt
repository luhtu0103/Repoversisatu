idf_component_register(
    SRCS "audio_hal.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio esp_driver_i2s esp-sr esp_timer afe_audio speaker_id
)
