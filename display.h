#pragma once

#include "driver/gpio.h"
#include <stdint.h>
#include <stdbool.h>

// OLED1 face display pinout (GPIOs swapped in code; physical wiring unchanged).
#define OLED_SDA_PIN  GPIO_NUM_10
#define OLED_SCL_PIN  GPIO_NUM_11
#define OLED_I2C_ADDR 0x3C
#define OLED_WIDTH    128
#define OLED_HEIGHT   64

typedef enum {
    FACE_THINKING = 0,
    FACE_SPEAKING,
    FACE_SLEEP,
    FACE_HAPPY,
    FACE_ANGRY
} face_state_t;

#ifdef __cplusplus
extern "C" {
#endif

void oled_init(void);
void display_status(const char *status);
void display_set_user_text(const char *text);
void display_set_gemini_text(const char *text);
void display_append_user_text(const char *text);
void display_append_gemini_text(const char *text);
void face_render(void);
void display_render_buffer(const uint8_t *buffer);

// Mochi pixel renderer used by the three-state face animation.
// expr: 0 neutral/thinking, 2 speaking. step: 0 open, 1 blink, 3 sleep, 5 thinking.
// sX/sY: shift the complete pair of eyes.
// arahLirik: 0 center, 1 left, 2 right, 3 up.
void display_render_mochi(int expr, int step, int sX, int sY, int arahLirik);

// Extended renderer used by the animation engine. gaze_x/gaze_y move only
// the pupils, allowing smooth eye movement without moving the eye shapes.
// gaze_x: -7..7, gaze_y: -5..5.
void display_render_mochi_gaze(
    int expr,
    int step,
    int sX,
    int sY,
    int gaze_x,
    int gaze_y, 
    int eye_shift_x,
    int eye_shift_y
);

void face_animation_start(void);
void face_animation_stop(void);
void face_set_state(face_state_t state);
// Audio-reactive mouth intensity: 0 = silence, 255 = full-scale PCM.
void face_set_speech_level(uint8_t level);
uint8_t face_get_speech_level(void);
face_state_t face_get_state(void);
void face_show_for_ms(face_state_t state, uint32_t duration_ms);

#ifdef __cplusplus
}
#endif
