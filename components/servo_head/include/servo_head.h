#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
bool servo_head_init(void);
void servo_head_set_speaking(bool speaking);
// Current shared gaze direction from any manual or automatic servo movement.
bool servo_head_get_gaze(int *x, int *y, uint32_t *transition_ms,
                          uint32_t *revision, uint32_t *start_tick);
bool servo_head_command(const char *action);
#ifdef __cplusplus
}
#endif
