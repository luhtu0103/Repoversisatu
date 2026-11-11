#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize ESP-SR AFE for 16 kHz, mono microphone audio.
// Enables noise suppression, automatic gain control and voice activity detection.
// AEC is intentionally disabled until a synchronized speaker-reference channel is provided.
bool afe_audio_init(void);
bool afe_audio_is_ready(void);
size_t afe_audio_get_feed_samples(void);

// Process exactly one AFE feed frame. Returns output samples, or 0 on failure.
size_t afe_audio_process_frame(const int16_t *input,
                               size_t input_samples,
                               int16_t *output,
                               size_t output_capacity_samples);

#ifdef __cplusplus
}
#endif
