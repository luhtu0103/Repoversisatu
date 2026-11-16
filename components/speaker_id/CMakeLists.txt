idf_component_register(
    SRCS "speaker_id.cpp" "speaker_id_web.cpp"
    INCLUDE_DIRS "."
    REQUIRES ai_memory esp_http_server cjson
    PRIV_REQUIRES esp_timer
)
