#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    bool active;
    int hour;
    int minute;
    int64_t target_epoch;
    char label[96];
} alarm_info_t;
bool alarm_init(void);
void alarm_set_time_ready(bool ready);
bool alarm_set(int hour, int minute, const char *label, char *result, size_t result_size);
bool alarm_cancel(char *result, size_t result_size);
bool alarm_get(alarm_info_t *info);
bool alarm_take_due(alarm_info_t *info);
bool alarm_poll_due_internal(void);
#ifdef __cplusplus
}
#endif
