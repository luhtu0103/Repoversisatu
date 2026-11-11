#include "wakeword.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"
#include "audio_hal.h"

static const char *TAG = "WAKEWORD";
static constexpr const char *WAKE_MODEL_NAME = "wn9_hiesp";

// DET_MODE_95 is ESP-SR's aggressive mode: higher wake-up recall, with a
// corresponding increase in false activations compared with DET_MODE_90.
static constexpr det_mode_t WAKE_DETECTION_MODE = DET_MODE_95;

static srmodel_list_t *s_models = nullptr;
static const esp_wn_iface_t *s_iface = nullptr;
static model_iface_data_t *s_model = nullptr;
static int s_chunk_samples = 0;

extern "C" bool wakeword_init(void)
{
    if (s_iface && s_model && s_chunk_samples > 0) return true;

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "WakeNet init: model=%s mode=DET_MODE_95", WAKE_MODEL_NAME);

    s_models = esp_srmodel_init("model");
    if (!s_models) {
        ESP_LOGE(TAG, "ESP-SR model loader failed: partition 'model' unavailable or invalid");
        return false;
    }

    ESP_LOGI(TAG, "ESP-SR models loaded: count=%d", s_models->num);
    if (esp_srmodel_exists(s_models, (char *)WAKE_MODEL_NAME) < 0) {
        ESP_LOGE(TAG, "WakeNet model missing from srmodels.bin: %s", WAKE_MODEL_NAME);
        esp_srmodel_deinit(s_models);
        s_models = nullptr;
        return false;
    }

    s_iface = esp_wn_handle_from_name(WAKE_MODEL_NAME);
    if (!s_iface) {
        ESP_LOGE(TAG, "WakeNet interface unavailable: %s", WAKE_MODEL_NAME);
        esp_srmodel_deinit(s_models);
        s_models = nullptr;
        return false;
    }

    ESP_LOGI(TAG, "Heap before model create: PSRAM free=%u largest=%u INTERNAL free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    s_model = s_iface->create(WAKE_MODEL_NAME, WAKE_DETECTION_MODE);
    if (!s_model) {
        ESP_LOGE(TAG, "Failed to create WakeNet model: %s", WAKE_MODEL_NAME);
        s_iface = nullptr;
        esp_srmodel_deinit(s_models);
        s_models = nullptr;
        return false;
    }

    s_chunk_samples = s_iface->get_samp_chunksize(s_model);
    const int sample_rate = s_iface->get_samp_rate(s_model);
    const int channels = s_iface->get_channel_num(s_model);

    ESP_LOGI(TAG, "WakeNet ready: rate=%d Hz chunk=%d samples channels=%d",
             sample_rate, s_chunk_samples, channels);
    if (sample_rate != MIC_SAMPLE_RATE || channels != 1 || s_chunk_samples <= 0) {
        ESP_LOGE(TAG, "WakeNet audio mismatch: expected %d Hz mono and valid chunk size",
                 MIC_SAMPLE_RATE);
        s_iface->destroy(s_model);
        s_model = nullptr;
        s_iface = nullptr;
        s_chunk_samples = 0;
        esp_srmodel_deinit(s_models);
        s_models = nullptr;
        return false;
    }

    for (int i = 1; i <= s_iface->get_word_num(s_model); ++i) {
        ESP_LOGI(TAG, "Wake word %d threshold=%.4f", i,
                 s_iface->get_det_threshold(s_model, i));
    }
    ESP_LOGI(TAG, "Wake word active: HI, ESP (aggressive sensitivity)");
    ESP_LOGI(TAG, "========================================");
    return true;
}

extern "C" bool wakeword_is_ready(void)
{
    return s_iface != nullptr && s_model != nullptr && s_chunk_samples > 0;
}

extern "C" int wakeword_get_chunk_samples(void)
{
    return wakeword_is_ready() ? s_chunk_samples : 0;
}

extern "C" int wakeword_detect(int16_t *pcm)
{
    if (!wakeword_is_ready() || !pcm) return 0;
    return (int)s_iface->detect(s_model, pcm);
}
