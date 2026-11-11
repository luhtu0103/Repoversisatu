idf_component_register(
    SRCS "led_rgb.cpp"
    INCLUDE_DIRS "include"
    REQUIRES led_strip esp_driver_gpio
)
