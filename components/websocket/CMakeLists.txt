idf_component_register(
    SRCS
        "websocket_mgr.cpp"
        "websocket_events.cpp"
        "websocket_rx.cpp"
        "websocket_json.cpp"
        "websocket_audio.cpp"
        "websocket_tx.cpp"
        "websocket_tool.cpp"
        "websocket_search.cpp"
        "websocket_radio.cpp"
        "websocket_stack_wrap.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        esp_websocket_client
        esp-tls
        esp_http_client
        esp_event
        esp_wifi
        cjson
        mbedtls
        display
        audio_hal
        wifi_manager
        uart_control
        web_config
        ai_memory
        servo_head
        alarm
        radio_player
        ota_update
        habit
        robot_stats
)

target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=esp_websocket_client_init"
)
