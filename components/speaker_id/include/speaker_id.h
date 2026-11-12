#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Lightweight MFCC + DTW speaker-comparison prototype.
 * Input: mono, signed PCM16, 16 kHz.
 * No neural-network model is used. The named enrolled template is stored persistently in NVS.
 * The returned DTW distance is an uncalibrated score, not an identity proof.
 */
esp_err_t speaker_id_init(void);

/* Enroll one clean reference recording (recommend 1-3 seconds). */
esp_err_t speaker_id_enroll_pcm16(const int16_t *pcm, size_t samples);

/* Compare a new recording to the enrolled reference. Lower often means closer.
 * Calibrate a decision threshold using genuine and impostor recordings.
 */
esp_err_t speaker_id_compare_pcm16(
    const int16_t *pcm,
    size_t samples,
    float *out_distance);

int speaker_id_has_template(void);
esp_err_t speaker_id_enroll_named_pcm16(const int16_t *pcm, size_t samples, const char *name);
esp_err_t speaker_id_request_enrollment(const char *name);
int speaker_id_take_enrollment_request(char *name_out, size_t name_out_size);
void speaker_id_complete_enrollment(esp_err_t result);

/* Explicit diagnostic verification; returns DTW distance, not an identity decision. */
esp_err_t speaker_id_request_verification(float *out_distance);
int speaker_id_take_verification_request(void);
void speaker_id_complete_verification(esp_err_t result, float distance);

#ifdef __cplusplus
}
#endif
