#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the independent second SSD1306 on I2C1 (GPIO41/42).
// Failure is non-fatal: OLED #1 and the AI pipeline remain available.
void oled2_face_init(void);

// OLED2 menampilkan RSSI Wi-Fi dan latensi TCP ke endpoint Gemini di baris atas.\n// Render the latest user and Gemini transcription on OLED #2.
void oled2_face_set_user_text(const char *text);
void oled2_face_set_gemini_text(const char *text);
void oled2_face_set_status(const char *status);
void oled2_face_append_user_text(const char *text);
void oled2_face_append_gemini_text(const char *text);

// Live PCM amplitude inputs for the OLED2 equalizer (0-255).
void oled2_face_set_mic_level(uint8_t level);
void oled2_face_set_speaker_level(uint8_t level);

#ifdef __cplusplus
}
#endif
