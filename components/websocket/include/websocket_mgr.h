#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ============================================================
// GEMINI LIVE API WEBSOCKET
// ============================================================
// API key dibaca saat runtime dari NVS melalui web_config.
// Tidak ada API key hardcoded di source repository.
// ============================================================

const char *websocket_get_server_url(void);
#define WEBSOCKET_SERVER_URL websocket_get_server_url()

void websocket_app_start(void);
bool websocket_send_audio_data(const uint8_t *data, size_t len);
bool websocket_is_connected(void);
bool websocket_send_touch_prompt(bool angry);
bool websocket_send_alarm_prompt(int hour, int minute, const char *label);

// Beri tahu Gemini siapa yang bicara (hasil pengenal suara) agar ia menyapa dengan nama.
// Dipanggil hanya untuk suara yang DIKENAL. Suara tidak dikenal: jangan panggil (tidak disapa).
bool websocket_send_speaker_context(const char *name);
