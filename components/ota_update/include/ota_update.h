#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Checks the latest GitHub Release and installs a newer firmware image if available.
// Returns ESP_OK when no update was needed; successful installation restarts the device.
esp_err_t ota_update_check_and_apply(void);

#ifdef __cplusplus
}
#endif
