#include "afe_audio.h"

#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_nsn_models.h"
#include "model_path.h"
#include "freertos/FreeRTOS.h"
#include "esp_timer.h"

#include <string.h>

static const char *TAG = "AFE_AUDIO";
static srmodel_list_t *afe_models = nullptr;
static afe_config_t *afe_config = nullptr;
static const esp_afe_sr_iface_t *afe_handle = nullptr;
static esp_afe_sr_data_t *afe_data = nullptr;
static size_t feed_samples = 0;
static size_t fetch_samples = 0;
static bool ready = false;
static uint32_t feed_fail_count = 0;
static uint32_t fetch_fail_count = 0;
static uint32_t feed_ok_count = 0;
static uint32_t fetch_ok_count = 0;

bool afe_audio_init(void)
{
    if (ready) return true;

    afe_models = esp_srmodel_init("model");
    if (!afe_models) {
        ESP_LOGE(TAG, "ESP-SR model partition unavailable; AFE disabled");
        return false;
    }

    afe_config = afe_config_init("M", afe_models, AFE_TYPE_VC, AFE_MODE_HIGH_PERF);
    if (!afe_config) {
        ESP_LOGE(TAG, "afe_config_init failed");
        esp_srmodel_deinit(afe_models);
        afe_models = nullptr;
        return false;
    }

    // Single INMP441 microphone, 16 kHz mono. AEC remains disabled.
    afe_config->aec_init = false;
    afe_config->se_init = false;
    afe_config->wakenet_init = false;
    afe_config->ns_init = true;
    afe_config->vad_init = true;
    afe_config->agc_init = true;
    afe_config->agc_mode = AFE_AGC_MODE_WEBRTC;
    afe_config->agc_compression_gain_db = 9;
    afe_config->agc_target_level_dbfs = 3;
    afe_config->afe_linear_gain = 1.0f;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    afe_config->fixed_output_channel = true;

    // Prefer NSNet2 when it is present in the model partition. Keep AFE's
    // supported WebRTC NS fallback if the model image does not contain NSNet.
    char *ns_model_name = esp_srmodel_filter(afe_models, ESP_NSNET_PREFIX, nullptr);
    if (ns_model_name) {
        afe_config->ns_model_name = ns_model_name;
        afe_config->afe_ns_mode = AFE_NS_MODE_NET;
        ESP_LOGI(TAG, "NSNet model selected: %s", ns_model_name);
    } else {
        afe_config->ns_model_name = nullptr;
        afe_config->afe_ns_mode = AFE_NS_MODE_WEBRTC;
        ESP_LOGW(TAG, "NSNet2 not found; requesting AFE WebRTC noise suppression");
    }

    afe_config_print(afe_config);
    // esp_afe_handle_from_config is declared by esp_afe_sr_models.h in ESP-SR 2.4.5.
    afe_handle = esp_afe_handle_from_config(afe_config);
    if (!afe_handle) {
        ESP_LOGE(TAG, "esp_afe_handle_from_config failed");
        afe_config_free(afe_config);
        afe_config = nullptr;
        esp_srmodel_deinit(afe_models);
        afe_models = nullptr;
        return false;
    }

    afe_data = afe_handle->create_from_config(afe_config);
    afe_config_free(afe_config);
    afe_config = nullptr;

    if (!afe_data) {
        ESP_LOGE(TAG, "AFE create_from_config failed");
        afe_handle = nullptr;
        esp_srmodel_deinit(afe_models);
        afe_models = nullptr;
        return false;
    }

    feed_samples = (size_t)afe_handle->get_feed_chunksize(afe_data);
    fetch_samples = (size_t)afe_handle->get_fetch_chunksize(afe_data);
    const int channels = afe_handle->get_feed_channel_num(afe_data);
    const int sample_rate = afe_handle->get_samp_rate(afe_data);

    if (feed_samples == 0 || fetch_samples == 0 || channels != 1 || sample_rate != 16000) {
        ESP_LOGE(TAG, "Unsupported AFE format: feed=%u fetch=%u channels=%d rate=%d",
                 (unsigned)feed_samples, (unsigned)fetch_samples, channels, sample_rate);
        afe_handle->destroy(afe_data);
        afe_data = nullptr;
        afe_handle = nullptr;
        feed_samples = fetch_samples = 0;
        esp_srmodel_deinit(afe_models);
        afe_models = nullptr;
        return false;
    }

    afe_handle->print_pipeline(afe_data);
    ready = true;
    ESP_LOGI(TAG, "AFE READY: 16kHz mono feed=%u samples fetch=%u samples; NS=ON AGC=ON VAD=ON AEC=OFF",
             (unsigned)feed_samples, (unsigned)fetch_samples);
    return true;
}

bool afe_audio_is_ready(void)
{
    return ready;
}

size_t afe_audio_get_feed_samples(void)
{
    return ready ? feed_samples : 0;
}

size_t afe_audio_process_frame(const int16_t *input,
                               size_t input_samples,
                               int16_t *output,
                               size_t output_capacity_samples)
{
    if (!ready || !afe_handle || !afe_data || !input || !output) return 0;
    if (input_samples != feed_samples) {
        ESP_LOGE(TAG, "AFE input frame mismatch: got=%u expected=%u",
                 (unsigned)input_samples, (unsigned)feed_samples);
        return 0;
    }

    const int64_t fetch_start_us = esp_timer_get_time();
    int feed_result = afe_handle->feed(afe_data, input);
    if (feed_result < 0) {
        ++feed_fail_count;
        if (feed_fail_count == 1 || (feed_fail_count % 50U) == 0)
            ESP_LOGW(TAG, "AFE feed failed: %d (count=%lu)", feed_result, (unsigned long)feed_fail_count);
        return 0;
    }

    ++feed_ok_count;
    // Keep the wait bounded: a missing frame falls back to raw PCM rather than
    // blocking the live microphone pipeline indefinitely.
    afe_fetch_result_t *result = afe_handle->fetch_with_delay(afe_data, pdMS_TO_TICKS(40));
    const int64_t fetch_elapsed_us = esp_timer_get_time() - fetch_start_us;
    if (!result || result->ret_value == ESP_FAIL || !result->data || result->data_size <= 0) {
        ++fetch_fail_count;
        if (fetch_fail_count == 1 || (fetch_fail_count % 25U) == 0) {
            const int ret_value = result ? result->ret_value : -999;
            const int data_size = result ? result->data_size : -1;
            ESP_LOGW(TAG, "AFE fetch no audio: count=%lu feed_ok=%lu fetch_ok=%lu result=%s ret=%d data_size=%d wait=%lldms",
                     (unsigned long)fetch_fail_count, (unsigned long)feed_ok_count,
                     (unsigned long)fetch_ok_count, result ? "present" : "null",
                     ret_value, data_size, (long long)(fetch_elapsed_us / 1000));
        }
        return 0;
    }

    const size_t output_samples = (size_t)result->data_size / sizeof(int16_t);
    if (output_samples > output_capacity_samples) {
        ESP_LOGE(TAG, "AFE output too large: %u samples, capacity=%u",
                 (unsigned)output_samples, (unsigned)output_capacity_samples);
        return 0;
    }

    memcpy(output, result->data, output_samples * sizeof(int16_t));
    ++fetch_ok_count;
    if (fetch_ok_count == 1 || (fetch_ok_count % 250U) == 0)
        ESP_LOGI(TAG, "AFE pipeline: feed_ok=%lu fetch_ok=%lu input=%u output=%u wait=%lldms",
                 (unsigned long)feed_ok_count, (unsigned long)fetch_ok_count,
                 (unsigned)input_samples, (unsigned)output_samples,
                 (long long)(fetch_elapsed_us / 1000));
    return output_samples;
}
