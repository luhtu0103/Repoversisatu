#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initializes WakeNet 9 with the Hi ESP model in the "model" partition.
bool wakeword_init(void);
bool wakeword_is_ready(void);
int wakeword_get_chunk_samples(void);

// Feed exactly one chunk of signed 16-bit mono PCM at 16 kHz.
// Returns >0 when the wake word is detected, otherwise 0.
int wakeword_detect(int16_t *pcm);

#ifdef __cplusplus
}
#endif
