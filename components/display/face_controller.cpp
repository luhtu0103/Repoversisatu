#include "display.h"
#include "display_internal.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "FACE_CTRL";
static TaskHandle_t anim_task_handle = NULL;
static SemaphoreHandle_t anim_start_mutex = NULL;

// Temporarily override one of the three supported face states.
// Animation sequences remain in face_animation.cpp.
static face_state_t face_override_previous = FACE_SLEEP;
static int64_t face_override_until_us = 0;
static portMUX_TYPE face_override_mux = portMUX_INITIALIZER_UNLOCKED;

void face_show_for_ms(face_state_t state, uint32_t duration_ms)
{
    const face_state_t previous = face_get_state();
    const int64_t until_us = duration_ms == 0
        ? 0
        : esp_timer_get_time() + ((int64_t)duration_ms * 1000LL);

    portENTER_CRITICAL(&face_override_mux);
    face_override_previous = previous;
    face_override_until_us = until_us;
    portEXIT_CRITICAL(&face_override_mux);

    face_set_state(state);

    ESP_LOGI(TAG,
             "FACE TOOL: state=%d duration=%lu ms previous=%d",
             (int)state,
             (unsigned long)duration_ms,
             (int)previous);
}

static void face_controller_task(void *arg)
{
    (void)arg;
    oled_init();

    while (1) {
        face_state_t restore_state = FACE_SLEEP;
        bool should_restore = false;
        const int64_t now_us = esp_timer_get_time();

        portENTER_CRITICAL(&face_override_mux);
        if (face_override_until_us > 0 && now_us >= face_override_until_us) {
            face_override_until_us = 0;
            restore_state = face_override_previous;
            should_restore = true;
        }
        portEXIT_CRITICAL(&face_override_mux);

        if (should_restore) {
            face_set_state(restore_state);
            ESP_LOGI(TAG, "FACE TOOL selesai; restore state=%d", (int)restore_state);
        }

        face_animation_run_state(face_get_state());
    }
}

void face_animation_start(void)
{
    if (!anim_start_mutex) {
        anim_start_mutex = xSemaphoreCreateMutex();
        if (!anim_start_mutex) {
            ESP_LOGE(TAG, "Gagal membuat mutex animasi OLED");
            return;
        }
    }

    xSemaphoreTake(anim_start_mutex, portMAX_DELAY);
    if (!anim_task_handle) {
        BaseType_t ok = xTaskCreate(
            face_controller_task,
            "face_ctrl",
            6144,
            NULL,
            2,
            &anim_task_handle
        );

        if (ok != pdPASS) {
            ESP_LOGE(TAG, "Gagal membuat task controller OLED");
            anim_task_handle = NULL;
        } else {
            ESP_LOGI(TAG, "Face controller + animation aktif");
        }
    }
    xSemaphoreGive(anim_start_mutex);
}

void face_animation_stop(void)
{
    if (!anim_start_mutex) return;

    xSemaphoreTake(anim_start_mutex, portMAX_DELAY);
    if (anim_task_handle) {
        vTaskDelete(anim_task_handle);
        anim_task_handle = NULL;
    }
    xSemaphoreGive(anim_start_mutex);
}
