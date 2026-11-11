idf_component_register(
    SRCS "wakeword.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp-sr esp_psram audio_hal
)
