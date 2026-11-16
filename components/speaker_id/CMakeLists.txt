idf_component_register(
    SRCS "speaker_id.cpp" "speaker_id_web.cpp"
    INCLUDE_DIRS "."
    REQUIRES ai_memory esp_http_server json
    PRIV_REQUIRES esp_timer
)
