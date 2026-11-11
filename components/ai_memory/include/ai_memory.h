#pragma once
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool ai_memory_init(void);
bool ai_memory_save(const char *key, const char *value);
int ai_memory_delete_target(const char *target, char *detail, size_t detail_size);
size_t ai_memory_get_context(char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
