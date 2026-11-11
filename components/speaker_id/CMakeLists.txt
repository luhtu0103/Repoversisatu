idf_component_register(
    SRCS "speaker_id.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_common log freertos nvs_flash
)
