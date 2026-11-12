idf_component_register(
    SRCS
        "display.cpp"
        "oled2_face.cpp"
        "face_base.cpp"
        "face_animation.cpp"
        "face_controller.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio esp_driver_i2c esp_lcd esp_timer esp_wifi
    PRIV_REQUIRES servo_head
)
