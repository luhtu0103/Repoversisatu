#include "audio_hal.h"
#include "speaker_id.h"
#include "esp_log.h"
#include "esp_err.h"
#include "afe_audio.h"
#include "esp_heap_caps.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

static const char *TAG = "AUDIO_HAL";
extern "C" void oled2_face_set_mic_level(uint8_t level);
extern "C" void oled2_face_set_speaker_level(uint8_t level);
static volatile bool mic_enabled = true;
static i2s_chan_handle_t rx_handle = NULL;
static i2s_chan_handle_t tx_handle = NULL;
static bool afe_init_attempted = false;
static uint32_t afe_fallback_count = 0;

#define MIC_CAPTURE_SAMPLES 512
#define MIC_CAPTURE_QUEUE_LENGTH 8
typedef struct { int16_t pcm[MIC_CAPTURE_SAMPLES]; } mic_capture_frame_t;
static QueueHandle_t mic_capture_queue = NULL;
static QueueHandle_t mic_capture_free_queue = NULL;
static mic_capture_frame_t *mic_capture_pool[MIC_CAPTURE_QUEUE_LENGTH] = {0};
static bool mic_capture_pool_ready = false;
static TaskHandle_t mic_capture_task_handle = NULL;
static uint32_t mic_capture_drops = 0;
static int16_t *mic_pending_pcm = NULL;
static int16_t *pcm_frame = NULL;
static int16_t *afe_output = NULL;
static int32_t *tx_buffer = NULL;
static int16_t *tone_buffer = NULL;
static size_t mic_pending_offset = MIC_CAPTURE_SAMPLES;

/* Prefer external RAM for large CPU-side buffers; retain an internal-heap
 * fallback so boards/configurations without usable PSRAM keep working. */
static void *audio_buffer_alloc(size_t size)
{
    void *p = NULL;
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0)
        p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p)
        p = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return p;
}

static void mic_capture_release(mic_capture_frame_t *frame)
{
    if (frame && mic_capture_free_queue)
        (void)xQueueSend(mic_capture_free_queue, &frame, 0);
}

static bool mic_capture_pool_init(void)
{
    if (mic_capture_pool_ready) return true;
    if (!mic_capture_queue)
        mic_capture_queue = xQueueCreate(MIC_CAPTURE_QUEUE_LENGTH, sizeof(mic_capture_frame_t *));
    if (!mic_capture_free_queue)
        mic_capture_free_queue = xQueueCreate(MIC_CAPTURE_QUEUE_LENGTH, sizeof(mic_capture_frame_t *));
    if (!mic_capture_queue || !mic_capture_free_queue) return false;

    for (size_t i = 0; i < MIC_CAPTURE_QUEUE_LENGTH; ++i) {
        if (!mic_capture_pool[i]) {
            void *p = NULL;
            if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0)
                p = heap_caps_malloc(sizeof(mic_capture_frame_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!p) p = heap_caps_malloc(sizeof(mic_capture_frame_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (!p) {
                ESP_LOGE(TAG, "Gagal alokasi MIC pool slot=%u", (unsigned)i);
                for (size_t j = 0; j < MIC_CAPTURE_QUEUE_LENGTH; ++j) {
                    if (mic_capture_pool[j]) { heap_caps_free(mic_capture_pool[j]); mic_capture_pool[j] = NULL; }
                }
                vQueueDelete(mic_capture_queue); mic_capture_queue = NULL;
                vQueueDelete(mic_capture_free_queue); mic_capture_free_queue = NULL;
                return false;
            }
            mic_capture_pool[i] = (mic_capture_frame_t *)p;
        }
        if (xQueueSend(mic_capture_free_queue, &mic_capture_pool[i], 0) != pdTRUE) {
            ESP_LOGE(TAG, "MIC free pool queue gagal diisi");
            return false;
        }
    }
    mic_capture_pool_ready = true;
    ESP_LOGI(TAG, "MIC capture pool siap: %u slot, masing-masing %u byte",
             (unsigned)MIC_CAPTURE_QUEUE_LENGTH, (unsigned)sizeof(mic_capture_frame_t));
    return true;
}

static void mic_capture_task(void *arg)
{
    (void)arg;
    int32_t raw[MIC_CAPTURE_SAMPLES];
    size_t raw_samples = 0;
    ESP_LOGI(TAG, "Microphone capture task started: 16kHz PCM16 frames=%u samples queue=%u",
             (unsigned)MIC_CAPTURE_SAMPLES, (unsigned)MIC_CAPTURE_QUEUE_LENGTH);
    for (;;) {
        size_t bytes_read = 0;
        const size_t bytes_needed = (MIC_CAPTURE_SAMPLES - raw_samples) * sizeof(int32_t);
        esp_err_t err = i2s_channel_read(rx_handle, raw + raw_samples, bytes_needed,
                                         &bytes_read, portMAX_DELAY);
        if (err != ESP_OK || bytes_read == 0) continue;
        raw_samples += bytes_read / sizeof(int32_t);
        if (!audio_hal_is_mic_enabled()) {
            // Keep measuring the physical mic for OLED2 while playback runs,
            // but never enqueue this audio to Gemini during the mute window.
            int32_t mic_peak = 0;
            for (size_t i = 0; i < raw_samples; ++i) {
                int32_t sample = raw[i] >> 16;
                if (sample < 0) sample = -sample;
                if (sample > mic_peak) mic_peak = sample;
            }
            uint32_t mic_level = (uint32_t)mic_peak >> 7;
            if (mic_level > 255U) mic_level = 255U;
            oled2_face_set_mic_level((uint8_t)mic_level);
            raw_samples = 0;
            continue;
        }

        if (raw_samples < MIC_CAPTURE_SAMPLES) continue;
        mic_capture_frame_t *frame = NULL;
        if (xQueueReceive(mic_capture_free_queue, &frame, 0) != pdTRUE || !frame) {
            ++mic_capture_drops;
            raw_samples = 0;
            if (mic_capture_drops == 1 || (mic_capture_drops % 100U) == 0U)
                ESP_LOGW(TAG, "MIC capture pool empty: drops=%lu", (unsigned long)mic_capture_drops);
            continue;
        }
        int32_t mic_peak = 0;
        for (size_t i = 0; i < MIC_CAPTURE_SAMPLES; ++i) {
            frame->pcm[i] = static_cast<int16_t>(raw[i] >> 16);
            int32_t sample = frame->pcm[i];
            if (sample < 0) sample = -sample;
            if (sample > mic_peak) mic_peak = sample;
        }
        uint32_t mic_level = (uint32_t)mic_peak >> 7;
        if (mic_level > 255U) mic_level = 255U;
        oled2_face_set_mic_level((uint8_t)mic_level);
        raw_samples = 0;

        if (xQueueSend(mic_capture_queue, &frame, 0) != pdTRUE) {
            mic_capture_release(frame);
            ++mic_capture_drops;
            if (mic_capture_drops == 1 || (mic_capture_drops % 100U) == 0U)
                ESP_LOGW(TAG, "MIC capture queue full: drops=%lu",
                         (unsigned long)mic_capture_drops);
        }
    }
}

static size_t read_captured_pcm(int16_t *dest, size_t max_samples)
{
    if (!dest || max_samples == 0 || !mic_capture_queue || !mic_pending_pcm || !audio_hal_is_mic_enabled()) return 0;
    size_t copied = 0;
    while (copied < max_samples) {
        // Playback may mute the microphone while this consumer is waiting.
        // Never wait forever: a mute transition must release the audio task.
        if (!audio_hal_is_mic_enabled()) {
            mic_pending_offset = MIC_CAPTURE_SAMPLES;
            return 0;
        }
        if (mic_pending_offset >= MIC_CAPTURE_SAMPLES) {
            mic_capture_frame_t *frame = NULL;
            if (xQueueReceive(mic_capture_queue, &frame, pdMS_TO_TICKS(20)) != pdTRUE || !frame) {
                // Return promptly so the caller can service playback/session state.
                break;
            }
            if (!audio_hal_is_mic_enabled()) {
                mic_capture_release(frame);
                mic_pending_offset = MIC_CAPTURE_SAMPLES;
                return 0;
            }
            memcpy(mic_pending_pcm, frame->pcm, MIC_CAPTURE_SAMPLES * sizeof(mic_pending_pcm[0]));
            mic_capture_release(frame);
            mic_pending_offset = 0;
        }
        size_t available = MIC_CAPTURE_SAMPLES - mic_pending_offset;
        size_t take = max_samples - copied;
        if (take > available) take = available;
        memcpy(dest + copied, mic_pending_pcm + mic_pending_offset, take * sizeof(int16_t));
        copied += take;
        mic_pending_offset += take;
    }
    return copied;
}

void audio_hal_set_mic_enabled(bool enabled)
{
    if (mic_enabled == enabled) return;
    mic_enabled = enabled;
    // Discard queued/partial microphone PCM across mute transitions so stale
    // pre-playback audio is not sent when the microphone is reopened.
    if (mic_capture_queue != NULL) {
        mic_capture_frame_t *stale = NULL;
        while (xQueueReceive(mic_capture_queue, &stale, 0) == pdTRUE) {
            if (stale) mic_capture_release(stale);
        }
    }
    mic_pending_offset = MIC_CAPTURE_SAMPLES;
    ESP_LOGI(TAG, "MIC %s (automatic speaker playback control)", enabled ? "ON" : "OFF");
}

bool audio_hal_is_mic_enabled(void)
{
    return mic_enabled;
}

void audio_hal_afe_init(void)
{
    if (afe_init_attempted) return;
    afe_init_attempted = true;
    if (!afe_audio_init()) {
        ESP_LOGW(TAG, "AFE init failed; microphone will use safe PCM fallback");
    }
}
static void log_audio_heap(const char *stage)
{
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "HEAP[%s]: internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u", stage, (unsigned)internal_free, (unsigned)internal_largest, (unsigned)psram_free, (unsigned)psram_largest);
}
void audio_hal_init(void)
{
    speaker_id_init();
    ESP_LOGI(TAG, "Menginisialisasi Audio I2S - proven v6.1.5 / Xiaozhi-compatible...");
    i2s_chan_config_t tx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    tx_chan_cfg.dma_desc_num = 6; tx_chan_cfg.dma_frame_num = 240; tx_chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&tx_chan_cfg, &tx_handle, nullptr));
    i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    rx_chan_cfg.dma_desc_num = 6; rx_chan_cfg.dma_frame_num = 240; rx_chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&rx_chan_cfg, nullptr, &rx_handle));
    i2s_std_config_t rx_cfg = {};
    rx_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE);
    rx_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO);
    rx_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    rx_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED; rx_cfg.gpio_cfg.bclk = MIC_I2S_SCK; rx_cfg.gpio_cfg.ws = MIC_I2S_WS; rx_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED; rx_cfg.gpio_cfg.din = MIC_I2S_SD;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &rx_cfg));
    i2s_std_config_t tx_cfg = {};
    tx_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SPK_SAMPLE_RATE);
    tx_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO);
    tx_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO; tx_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT; tx_cfg.slot_cfg.ws_width = I2S_DATA_BIT_WIDTH_32BIT; tx_cfg.slot_cfg.ws_pol = false; tx_cfg.slot_cfg.bit_shift = true;
    tx_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED; tx_cfg.gpio_cfg.bclk = SPK_I2S_BCLK; tx_cfg.gpio_cfg.ws = SPK_I2S_LRCK; tx_cfg.gpio_cfg.dout = SPK_I2S_DOUT; tx_cfg.gpio_cfg.din = I2S_GPIO_UNUSED;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &tx_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle)); ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
    if (!mic_pending_pcm) mic_pending_pcm = (int16_t *)audio_buffer_alloc(MIC_CAPTURE_SAMPLES * sizeof(int16_t));
    if (!pcm_frame) pcm_frame = (int16_t *)audio_buffer_alloc(1024 * sizeof(int16_t));
    if (!afe_output) afe_output = (int16_t *)audio_buffer_alloc(1024 * sizeof(int16_t));
    if (!tx_buffer) tx_buffer = (int32_t *)audio_buffer_alloc(1024 * sizeof(int32_t));
    ESP_LOGI(TAG, "PCM buffers: pending=%s pcm_frame=%s afe_output=%s tx=%s",
             mic_pending_pcm ? "ready" : "FAILED",
             pcm_frame ? "ready" : "FAILED",
             afe_output ? "ready" : "FAILED",
             tx_buffer ? "ready" : "FAILED");

    if (!mic_capture_pool_init()) {
        ESP_LOGE(TAG, "Failed to initialize microphone capture PSRAM pool");
    } else {
        BaseType_t capture_result = xTaskCreatePinnedToCore(
            mic_capture_task, "mic_capture", 6144, NULL, 6, &mic_capture_task_handle, 0);
        if (capture_result != pdPASS) {
            ESP_LOGE(TAG, "Failed to create microphone capture task");
            vQueueDelete(mic_capture_queue); mic_capture_queue = NULL;
            vQueueDelete(mic_capture_free_queue); mic_capture_free_queue = NULL;
            for (size_t i = 0; i < MIC_CAPTURE_QUEUE_LENGTH; ++i) {
                if (mic_capture_pool[i]) { heap_caps_free(mic_capture_pool[i]); mic_capture_pool[i] = NULL; }
            }
            mic_capture_pool_ready = false;
        }
    }
    log_audio_heap("after_i2s_init"); log_audio_heap("after_audio_init");
    ESP_LOGI(TAG, "Audio siap. MIC=%d Hz 32-bit LEFT -> PCM16, SPEAKER=%d Hz 32-bit LEFT", MIC_SAMPLE_RATE, SPK_SAMPLE_RATE);
}
// Raw PCM path for WakeNet, served from the dedicated I2S capture queue.
size_t audio_read_mic_raw(uint8_t *dest, size_t max_len)
{
    if (!dest || max_len < sizeof(int16_t)) return 0;
    return read_captured_pcm(reinterpret_cast<int16_t *>(dest), max_len / sizeof(int16_t)) * sizeof(int16_t);
}

size_t audio_read_mic(uint8_t *dest, size_t max_len)
{
    if (!rx_handle || !dest || max_len < sizeof(int16_t) || !audio_hal_is_mic_enabled()) return 0;

    if (!afe_init_attempted) audio_hal_afe_init();

    if (!pcm_frame || !afe_output) return 0;

    size_t max_samples = max_len / sizeof(int16_t);
    if (max_samples > 1024) max_samples = 1024;

    const size_t afe_frame_samples = afe_audio_get_feed_samples();
    if (afe_audio_is_ready() && afe_frame_samples > 0 && afe_frame_samples <= 1024 &&
        max_samples >= afe_frame_samples) {
        max_samples = afe_frame_samples;
    }

    const size_t samples = read_captured_pcm(pcm_frame, max_samples);
    if (samples == 0) return 0;

    if (afe_audio_is_ready() && samples == afe_frame_samples) {
        size_t out_capacity = max_len / sizeof(int16_t);
        if (out_capacity > 1024)
            out_capacity = 1024;
        const size_t out_samples = afe_audio_process_frame(
            pcm_frame, samples, afe_output, out_capacity);
        if (out_samples > 0) {
            speaker_id_process_pcm(afe_output, out_samples);
            memcpy(dest, afe_output, out_samples * sizeof(int16_t));
            return out_samples * sizeof(int16_t);
        }
        ++afe_fallback_count;
        if (afe_fallback_count == 1 || (afe_fallback_count % 50U) == 0)
            ESP_LOGW(TAG, "AFE processing failed; passing through safe fallback audio (count=%lu)", (unsigned long)afe_fallback_count);
    }

    // Fallback keeps the assistant usable if AFE cannot initialize or a frame
    // does not match the AFE size. Do not apply a frame-wide limiter that pumps
    // the gain and softens the next quiet consonant.
    int16_t *pcm = reinterpret_cast<int16_t *>(dest);
    const size_t output_samples = samples < max_samples ? samples : max_samples;
    for (size_t i = 0; i < output_samples; ++i) {
        int32_t value = (int32_t)pcm_frame[i] * 2;
        if (value > INT16_MAX) value = INT16_MAX;
        if (value < INT16_MIN) value = INT16_MIN;
        pcm[i] = (int16_t)value;
    }
    speaker_id_process_pcm(pcm, output_samples);
    return output_samples * sizeof(int16_t);
}
size_t audio_write_speaker(const uint8_t *src, size_t len)
{
    if (!tx_handle || !src || len < 2) return 0;
    len &= ~((size_t)1);
    if (!tx_buffer) return 0;
    const int16_t *pcm = reinterpret_cast<const int16_t *>(src);
    const size_t total = len / sizeof(int16_t);
    size_t offset = 0;
    constexpr size_t I2S_WRITE_SAMPLES = 240;
    constexpr uint32_t I2S_WRITE_TIMEOUT_MS = 50;

    while (offset < total) {
        const size_t old_offset = offset;
        size_t n = total - offset;
        if (n > I2S_WRITE_SAMPLES) n = I2S_WRITE_SAMPLES;
        for (size_t i = 0; i < n; ++i)
            tx_buffer[i] = static_cast<int32_t>(pcm[old_offset + i]) << 16;

        size_t written = 0;
        esp_err_t err = i2s_channel_write(tx_handle, tx_buffer, n * sizeof(int32_t),
                                          &written, I2S_WRITE_TIMEOUT_MS);
        size_t samples_written = written / sizeof(int32_t);
        if (samples_written > n) samples_written = n;
        offset += samples_written;

        if (err != ESP_OK || samples_written == 0) {
            ESP_LOGW(TAG, "I2S speaker write timeout/fail: err=%s written=%u/%u timeout=%ums",
                     esp_err_to_name(err), (unsigned)written,
                     (unsigned)(n * sizeof(int32_t)), (unsigned)I2S_WRITE_TIMEOUT_MS);
            vTaskDelay(1);
            break;
        }
    }

    // Return bytes from the original PCM16 input represented by samples
    // successfully accepted by the I2S driver (not the expanded PCM32 bytes).
    return offset * sizeof(int16_t);
}
void audio_i2s_test_tone(void)
{
    static const int16_t sine_table[24] = {0, 2071, 4000, 5657, 6928, 7727, 8000, 7727, 6928, 5657, 4000, 2071, 0, -2071, -4000, -5657, -6928, -7727, -8000, -6928, -5657, -4000, -2071};
    if (!tone_buffer) tone_buffer = (int16_t *)audio_buffer_alloc(2400 * sizeof(int16_t));
    if (!tx_handle || !tone_buffer) return;
    for (size_t i = 0; i < 2400; ++i) tone_buffer[i] = sine_table[i % 24];
    ESP_LOGI(TAG, "I2S TEST TONE: 1kHz PCM16 -> PCM32 I2S, 24kHz, 100ms");
    audio_write_speaker(reinterpret_cast<const uint8_t *>(tone_buffer), 2400 * sizeof(int16_t));
}
