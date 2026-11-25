
idf_component_register(
    SRCS "habit.cpp"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_timer app_update
)
