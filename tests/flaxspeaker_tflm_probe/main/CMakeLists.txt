idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "." "../compat"
    REQUIRES esp-tflite-micro esp_timer heap log
    EMBED_FILES "model_int8.tflite"
)
