#pragma once

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// ================================
// INMP441 - MICROPHONE
// ================================
#define MIC_I2S_SCK   GPIO_NUM_5
#define MIC_I2S_WS    GPIO_NUM_4
#define MIC_I2S_SD    GPIO_NUM_6

// ================================
// MAX98357A - SPEAKER
// ================================
#define SPK_I2S_BCLK  GPIO_NUM_15
#define SPK_I2S_LRCK  GPIO_NUM_16
#define SPK_I2S_DOUT  GPIO_NUM_7

// Gemini Live API input audio
#define MIC_SAMPLE_RATE 16000

// Gemini Live API native audio output
#define SPK_SAMPLE_RATE 24000

#define AUDIO_BITS 16

void audio_hal_init(void);

// Read PCM frames captured by the dedicated I2S producer task.

// Automatically disable microphone capture while speaker playback is active.
void audio_hal_set_mic_enabled(bool enabled);
bool audio_hal_is_mic_enabled(void);

// Initialize the ESP-SR Audio Front-End for microphone-to-Gemini audio.
void audio_hal_afe_init(void);

void audio_i2s_test_tone(void);

// Raw PCM16 from I2S for WakeNet; bypasses NSNet2 and conversation gain.
size_t audio_read_mic_raw(uint8_t *dest, size_t max_len);

size_t audio_read_mic(uint8_t *dest, size_t max_len);

// Returns the number of original PCM16 bytes successfully accepted by I2S.
size_t audio_write_speaker(const uint8_t *src, size_t len);
