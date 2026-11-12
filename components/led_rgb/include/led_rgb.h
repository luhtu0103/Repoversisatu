#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LED_RGB_OFF = 0,
    LED_RGB_BOOT_BLINK,
    LED_RGB_CONNECTING,
    LED_RGB_LISTENING,
    LED_RGB_VOICE_DETECTED,
    LED_RGB_SPEAKING,
    LED_RGB_WIFI_CONFIG
} led_rgb_state_t;

// One WS2812/NeoPixel on GPIO48. Call once during app startup.
bool led_rgb_init(void);
void led_rgb_set_state(led_rgb_state_t state);

#ifdef __cplusplus
}
#endif
