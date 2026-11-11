#include "touch_emotion.h"
#include "display.h"
#include "websocket_mgr.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "TOUCH_EMOTION";
static constexpr int64_t DEBOUNCE_US = 50000LL;
static constexpr int64_t LONG_TOUCH_US = 1200000LL;
static constexpr int64_t MULTI_TOUCH_WINDOW_US = 2000000LL;
static constexpr unsigned MULTI_TOUCH_COUNT = 4;
static constexpr uint32_t EMOTION_MS = 1800;

static void touch_task(void *arg)
{
    (void)arg;
    bool raw = gpio_get_level((gpio_num_t)TOUCH_EMOTION_GPIO) != 0;
    bool stable = raw;
    bool long_sent = false;
    int64_t raw_changed_us = esp_timer_get_time();
    int64_t pressed_since_us = raw ? raw_changed_us : 0;
    int64_t touch_window_start_us = 0;
    int64_t emotion_until_us = 0;
    face_state_t emotion_previous = FACE_SLEEP;
    unsigned touch_count = 0;

    ESP_LOGI(TAG, "Task sensor aktif di GPIO%d; long=%lldms, %u tap/%lldms",
             TOUCH_EMOTION_GPIO, (long long)(LONG_TOUCH_US / 1000),
             MULTI_TOUCH_COUNT, (long long)(MULTI_TOUCH_WINDOW_US / 1000));

    for (;;) {
        const int64_t now = esp_timer_get_time();
        const bool sampled = gpio_get_level((gpio_num_t)TOUCH_EMOTION_GPIO) != 0;
        if (sampled != raw) {
            raw = sampled;
            raw_changed_us = now;
        }

        if (sampled != stable && now - raw_changed_us >= DEBOUNCE_US) {
            stable = sampled;
            if (stable) {
                pressed_since_us = now;
                long_sent = false;
                if (touch_window_start_us == 0 || now - touch_window_start_us > MULTI_TOUCH_WINDOW_US) {
                    touch_window_start_us = now;
                    touch_count = 0;
                }
                ++touch_count;
                if (touch_count >= MULTI_TOUCH_COUNT) {
                    touch_count = 0;
                    touch_window_start_us = 0;
                    long_sent = true;
                    if (face_get_state() != FACE_SPEAKING) {
                        emotion_previous = face_get_state();
                        face_set_state(FACE_ANGRY);
                        if (websocket_is_connected()) websocket_send_touch_prompt(true);
                        emotion_until_us = now + (int64_t)EMOTION_MS * 1000LL;
                        ESP_LOGI(TAG, "Sentuhan berulang: MARAH");
                    } else {
                        ESP_LOGI(TAG, "Tap berulang diabaikan: robot sedang berbicara");
                    }
                }
            } else {
                pressed_since_us = 0;
            }
        }

        if (stable && !long_sent && pressed_since_us > 0 && now - pressed_since_us >= LONG_TOUCH_US) {
            long_sent = true;
            touch_count = 0;
            touch_window_start_us = 0;
            if (face_get_state() != FACE_SPEAKING) {
                emotion_previous = face_get_state();
                face_set_state(FACE_HAPPY);
                if (websocket_is_connected()) websocket_send_touch_prompt(false);
                emotion_until_us = now + (int64_t)EMOTION_MS * 1000LL;
                ESP_LOGI(TAG, "Sentuhan lama: SENANG");
            } else {
                ESP_LOGI(TAG, "Sentuhan lama diabaikan: robot sedang berbicara");
            }
        }

        if (emotion_until_us > 0 && now >= emotion_until_us) {
            const face_state_t current = face_get_state();
            if (current == FACE_HAPPY || current == FACE_ANGRY)
                face_set_state(emotion_previous);
            emotion_until_us = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

bool touch_emotion_start(void)
{
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << TOUCH_EMOTION_GPIO;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_ENABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d init gagal: %s", TOUCH_EMOTION_GPIO, esp_err_to_name(err));
        return false;
    }
    if (xTaskCreate(touch_task, "touch_sensor", 3072, nullptr, 3, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Gagal membuat task sensor sentuh");
        return false;
    }
    return true;
}
