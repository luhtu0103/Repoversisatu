#pragma once
#ifdef __cplusplus
extern "C" {
#endif
// TTP223 digital output. Verify GPIO2 is free on the exact board/wiring.
#ifndef TOUCH_EMOTION_GPIO
#define TOUCH_EMOTION_GPIO 2
#endif
bool touch_emotion_start(void);
#ifdef __cplusplus
}
#endif
