idf_component_register(
    SRCS "robot_stats.cpp"
    INCLUDE_DIRS "include"
    REQUIRES uart_control esp_wifi esp_event esp_timer app_update esp_netif display
)
