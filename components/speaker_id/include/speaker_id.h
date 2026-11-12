#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define SPEAKER_ID_NAME_MAX 24
void speaker_id_init(void);
/* Feed mono 16 kHz PCM16 after AFE. Non-blocking feature tap; safe to call per audio frame. */
void speaker_id_process_pcm(const int16_t *pcm, size_t samples);
/* Save the most recent voice features under an explicit profile name. */
bool speaker_id_enroll_last(const char *name, char *result, size_t result_len);
bool speaker_id_identify(char *name, size_t name_len, float *distance_out);
size_t speaker_id_profile_count(void);
#ifdef __cplusplus
}
#endif