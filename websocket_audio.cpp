#include "websocket_internal.h"
#include "websocket_mgr.h"
#include "audio_hal.h"
#include "display.h"
#include "servo_head.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/semphr.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const char *TAG = "WS_AUDIO";
extern "C" void oled2_face_set_speaker_level(uint8_t level);
static volatile bool audio_clear_pending = false;
static StaticSemaphore_t audio_send_mutex_storage;
static SemaphoreHandle_t audio_send_mutex = NULL;
#define AUDIO_OUTPUT_SAMPLE_RATE 24000U
#define AUDIO_OUTPUT_BYTES_PER_SEC (AUDIO_OUTPUT_SAMPLE_RATE * 2U)
#define AUDIO_RING_BUFFER_SIZE (512 * 1024)
#define AUDIO_PLAYBACK_PREBUFFER_SIZE (32 * 1024)
#define AUDIO_PLAYBACK_RESUME_SIZE (4 * 1024)
#define AUDIO_PLAYBACK_READ_SIZE 2048
#define AUDIO_PLAYBACK_READ_WAIT_MS 5
#define AUDIO_PLAYBACK_TRIGGER_SIZE 1024
#define AUDIO_SEND_CHUNK_SIZE 512
#define AUDIO_SEND_WAIT_MS 50
static volatile uint32_t audio_turn_generation = 0;
static size_t send_realtime_pcm(const uint8_t *data, size_t len)
{
    if (audio_stream == NULL || data == NULL || len == 0) return 0;
    size_t offset = 0;
    while (offset < len) {
        size_t chunk = len - offset;
        if (chunk > AUDIO_SEND_CHUNK_SIZE) chunk = AUDIO_SEND_CHUNK_SIZE;
        chunk &= ~((size_t)1);
        if (chunk == 0) break;
        // RX/event handling must never wait for speaker playback to free space.
        // If the playback ring is full, stop immediately and let accounting below
        // report the unqueued bytes instead of blocking WebSocket fragment delivery.
        if (xStreamBufferSpacesAvailable(audio_stream) < chunk) break;
        size_t written = xStreamBufferSend(audio_stream, data + offset, chunk, 0);
        if (written > 0) {
            if (written > chunk) written = chunk;
            written &= ~((size_t)1);
            offset += written;
            audio_bytes_queued += written;
            if (written < chunk) continue;
            continue;
        }
        ESP_LOGW(TAG, "Audio ring penuh atau TX write tidak tersedia: offset=%u/%u pending=%u spaces=%u", (unsigned)offset, (unsigned)len, (unsigned)xStreamBufferBytesAvailable(audio_stream), (unsigned)xStreamBufferSpacesAvailable(audio_stream));
        break; // zero-wait producer: never spin when the playback ring cannot accept data
    }
    return offset;
}
size_t get_audio_pending_bytes(void) { return audio_stream == NULL ? 0 : xStreamBufferBytesAvailable(audio_stream); }
static volatile bool audio_playback_complete_event = false;

bool websocket_take_audio_playback_complete(void)
{
    bool complete = audio_playback_complete_event;
    audio_playback_complete_event = false;
    return complete;
}

void check_audio_playback_complete(void)
{
    if (!audio_turn_complete_pending || audio_stream == NULL) return;
    if (xStreamBufferBytesAvailable(audio_stream) != 0) return;
    audio_turn_complete_pending = false;
    audio_turn_active = false;
    // Let the I2S DMA speaker tail drain before reopening the microphone.
    vTaskDelay(pdMS_TO_TICKS(70));
    // A new server turn may arrive during the drain delay. Keep the mic muted
    // if playback has already resumed or new PCM is queued.
    if (audio_turn_active || xStreamBufferBytesAvailable(audio_stream) != 0) return;
    audio_hal_set_mic_enabled(true);
    const uint64_t accounted = audio_bytes_queued + audio_bytes_dropped;
    const int64_t balance = (int64_t)audio_bytes_received - (int64_t)accounted;
    ESP_LOGI(TAG, "AUDIO PLAYBACK COMPLETE: received=%llu queued=%llu played=%llu pending=0 queue_dropped=%llu i2s_dropped=%llu balance=%lld", (unsigned long long)audio_bytes_received, (unsigned long long)audio_bytes_queued, (unsigned long long)audio_bytes_played, (unsigned long long)audio_bytes_dropped, (unsigned long long)audio_bytes_playback_dropped, (long long)balance);
    audio_playback_complete_event = true;
    oled2_face_set_speaker_level(0);
    servo_head_set_speaking(false);
    face_set_speech_level(0);
    face_set_state(FACE_THINKING);
}
static void audio_playback_task(void *arg)
{
    (void)arg;
    static uint8_t playback_buffer[AUDIO_PLAYBACK_READ_SIZE];
    bool playback_started = false;
    bool playback_started_once = false;
    bool underrun_reported = false;
    uint32_t playback_generation = 0;
    int64_t last_stats_us = 0;
    ESP_LOGI(TAG, "Audio playback task: 24kHz PCM16 mono, ring=%u, prebuffer=%u, core=%d priority=3", (unsigned)AUDIO_RING_BUFFER_SIZE, (unsigned)AUDIO_PLAYBACK_PREBUFFER_SIZE, xPortGetCoreID());
    for (;;) {
        if (audio_clear_pending) {
            audio_clear_pending = false;
            if (audio_send_mutex != NULL) {
                if (xSemaphoreTake(audio_send_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                    if (audio_stream != NULL) xStreamBufferReset(audio_stream);
                    xSemaphoreGive(audio_send_mutex);
                } else {
                    ESP_LOGW(TAG, "Playback clear mutex busy - clear ditunda");
                    audio_clear_pending = true;
                    vTaskDelay(pdMS_TO_TICKS(2));
                    continue;
                }
            } else if (audio_stream != NULL) xStreamBufferReset(audio_stream);
            audio_turn_complete_pending = false;
            audio_turn_active = false;
            audio_hal_set_mic_enabled(true);
            playback_started = false;
            playback_started_once = false;
            underrun_reported = false;
            playback_generation = audio_turn_generation;
        }
        uint32_t current_generation = audio_turn_generation;
        if (current_generation != playback_generation) {
            playback_generation = current_generation;
            playback_started = false;
            playback_started_once = false;
            underrun_reported = false;
        }
        if (audio_stream == NULL) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        size_t pending = xStreamBufferBytesAvailable(audio_stream);
        const size_t start_threshold = playback_started_once ? AUDIO_PLAYBACK_RESUME_SIZE : AUDIO_PLAYBACK_PREBUFFER_SIZE;
        if (!playback_started && pending < start_threshold && audio_turn_active && !audio_turn_complete_pending) { vTaskDelay(1); continue; }
        if (playback_started && pending == 0 && audio_turn_active && !audio_turn_complete_pending) {
            if (!underrun_reported) {
                ESP_LOGW(TAG, "AUDIO PLAYBACK UNDERRUN: PCM buffer kosong di tengah turn - rebuffer");
                underrun_reported = true;
            }
            playback_started = false;
            vTaskDelay(pdMS_TO_TICKS(AUDIO_PLAYBACK_READ_WAIT_MS));
            continue;
        }
        size_t received = xStreamBufferReceive(audio_stream, playback_buffer, sizeof(playback_buffer), pdMS_TO_TICKS(AUDIO_PLAYBACK_READ_WAIT_MS));
        if (received == 0) { check_audio_playback_complete(); vTaskDelay(1); continue; }
        received &= ~((size_t)1);
        if (received == 0) { vTaskDelay(1); continue; }
        if (!playback_started) { playback_started = true; playback_started_once = true; audio_hal_set_mic_enabled(false); servo_head_set_speaking(true); face_set_state(FACE_SPEAKING); }
        underrun_reported = false;

        // Estimate the outgoing voice level from the same PCM chunk sent to I2S.
        // This keeps the mouth synchronized to actual synthesized audio, not a timer.
        int32_t peak = 0;
        const int16_t *samples = reinterpret_cast<const int16_t *>(playback_buffer);
        const size_t sample_count = received / sizeof(int16_t);
        for (size_t i = 0; i < sample_count; ++i) {
            int32_t sample = samples[i];
            if (sample < 0) sample = -sample;
            if (sample > peak) peak = sample;
        }
        uint32_t level = (uint32_t)peak >> 7;
        if (level > 255U) level = 255U;
        face_set_speech_level((uint8_t)level);
        oled2_face_set_speaker_level((uint8_t)level);

        const size_t written_pcm_bytes = audio_write_speaker(playback_buffer, received);
        audio_write_calls++;
        audio_bytes_played += written_pcm_bytes;
        if (written_pcm_bytes < received) {
            const size_t unwritten_pcm_bytes = received - written_pcm_bytes;
            audio_bytes_playback_dropped += unwritten_pcm_bytes;
            ESP_LOGW(TAG, "Audio playback loss: I2S accepted=%u/%u PCM bytes; dropped=%u",
                     (unsigned)written_pcm_bytes, (unsigned)received,
                     (unsigned)unwritten_pcm_bytes);
        }
        check_audio_playback_complete();
        int64_t now_us = esp_timer_get_time();
        if (last_stats_us == 0 || now_us - last_stats_us >= 1000000) {
            last_stats_us = now_us;
            ESP_LOGI(TAG, "AUDIO FLOW: pending=%u/%u received=%llu queued=%llu played=%llu queue_dropped=%llu i2s_dropped=%llu", (unsigned)xStreamBufferBytesAvailable(audio_stream), (unsigned)AUDIO_RING_BUFFER_SIZE, (unsigned long long)audio_bytes_received, (unsigned long long)audio_bytes_queued, (unsigned long long)audio_bytes_played, (unsigned long long)audio_bytes_dropped, (unsigned long long)audio_bytes_playback_dropped);
        }
        if (!audio_turn_active && xStreamBufferBytesAvailable(audio_stream) == 0) { playback_started = false; playback_started_once = false; underrun_reported = false; }
    }
}
bool start_audio_playback(void)
{
    if (audio_stream != NULL) return true;
    if (audio_send_mutex == NULL) {
        audio_send_mutex = xSemaphoreCreateMutexStatic(&audio_send_mutex_storage);
        if (audio_send_mutex == NULL) { ESP_LOGE(TAG, "Gagal membuat audio send mutex"); return false; }
    }
    uint8_t *buffer_mem = (uint8_t*)heap_caps_malloc(AUDIO_RING_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
    if (buffer_mem == NULL) {
        buffer_mem = (uint8_t*)heap_caps_malloc(AUDIO_RING_BUFFER_SIZE, MALLOC_CAP_INTERNAL);
        if (buffer_mem == NULL) { ESP_LOGE(TAG, "Gagal alokasi %u byte untuk audio buffer", (unsigned)AUDIO_RING_BUFFER_SIZE); return false; }
        ESP_LOGW(TAG, "Menggunakan RAM internal untuk audio buffer");
    }
    static StaticStreamBuffer_t stream_buffer_struct;
    audio_stream = xStreamBufferCreateStatic(AUDIO_RING_BUFFER_SIZE, AUDIO_PLAYBACK_TRIGGER_SIZE, buffer_mem, &stream_buffer_struct);
    if (audio_stream == NULL) { ESP_LOGE(TAG, "Gagal membuat static stream buffer"); heap_caps_free(buffer_mem); return false; }
    BaseType_t result = xTaskCreatePinnedToCore(audio_playback_task, "audio_playback", 4096, NULL, 3, &audio_playback_task_handle, 1);
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Gagal membuat audio_task/playback task: free_internal=%u largest=%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        vStreamBufferDelete(audio_stream);
        audio_stream = NULL;
        audio_playback_task_handle = NULL;
        return false;
    }
    ESP_LOGI(TAG, "Audio ring buffer siap: %u byte, prebuffer=%u, target=%u B/s, playback core=1 priority=3", (unsigned)AUDIO_RING_BUFFER_SIZE, (unsigned)AUDIO_PLAYBACK_PREBUFFER_SIZE, (unsigned)AUDIO_OUTPUT_BYTES_PER_SEC);
    return true;
}

void request_audio_buffer_clear(void) { audio_clear_pending = true; }

void clear_audio_buffer(void)
{
    audio_clear_pending = false;
    audio_playback_complete_event = false;
    if (audio_send_mutex != NULL) {
        xSemaphoreTake(audio_send_mutex, portMAX_DELAY);
        if (audio_stream != NULL) xStreamBufferReset(audio_stream);
        xSemaphoreGive(audio_send_mutex);
    } else if (audio_stream != NULL) {
        xStreamBufferReset(audio_stream);
    }
    audio_turn_complete_pending = false;
    audio_turn_active = false;
    audio_hal_set_mic_enabled(true);
    face_set_speech_level(0);
    oled2_face_set_speaker_level(0);
}

void reset_audio_turn_stats(void)
{
    audio_chunks_received = 0;
    audio_bytes_received = 0;
    audio_bytes_queued = 0;
    audio_write_calls = 0;
    audio_bytes_played = 0;
    audio_bytes_dropped = 0;
    audio_bytes_playback_dropped = 0;
    audio_turn_active = false;
    audio_turn_complete_pending = false;
}

void begin_audio_turn(void)
{
    if (audio_turn_active) return;
    // Prevent a completion event from an earlier audio turn ending a new session.
    audio_playback_complete_event = false;
    audio_chunks_received = 0;
    audio_bytes_received = 0;
    audio_bytes_queued = 0;
    audio_write_calls = 0;
    audio_bytes_played = 0;
    audio_bytes_dropped = 0;
    audio_bytes_playback_dropped = 0;

    if (audio_stream != NULL) {
        size_t stale = xStreamBufferBytesAvailable(audio_stream);
        if (stale > 0) {
            xStreamBufferReset(audio_stream);
            audio_bytes_dropped = stale;
            ESP_LOGW(TAG, "Audio stale PCM dibuang saat turn baru: %u byte", (unsigned)stale);
        }
    }

    uint32_t next_generation = audio_turn_generation + 1U;
    if (next_generation == 0U) next_generation = 1U;
    audio_turn_generation = next_generation;
    audio_turn_active = true;
    audio_turn_complete_pending = false;
}

bool queue_audio_pcm(const uint8_t *pcm, size_t len)
{
    if (pcm == NULL || len == 0) return false;
    len &= ~((size_t)1);
    if (len == 0) return false;
    // Playback buffer/task must be initialized by the manager before WebSocket starts.
    // Do not allocate 512 KB or create a task from the WebSocket event callback.
    if (audio_stream == NULL) {
        ESP_LOGE(TAG, "PCM ditolak: playback belum diinisialisasi");
        audio_bytes_dropped += len;
        return false;
    }
    if (audio_send_mutex == NULL) {
        ESP_LOGE(TAG, "Audio send mutex belum siap");
        return false;
    }
    // This function can be reached from WebSocket RX fragment handling.
    // Never block that path behind playback buffer management.
    if (xSemaphoreTake(audio_send_mutex, 0) != pdTRUE) {
        audio_bytes_dropped += len;
        ESP_LOGW(TAG, "Playback enqueue skipped: audio mutex busy, dropped=%u bytes", (unsigned)len);
        return false;
    }

    begin_audio_turn();

    uint64_t queued_before = audio_bytes_queued;
    uint64_t dropped_before = audio_bytes_dropped;

    // Volume feature removed: Gemini PCM masuk ke playback tanpa modifikasi.
    (void)send_realtime_pcm(pcm, len);

    const uint64_t queued_delta = audio_bytes_queued - queued_before;
    const uint64_t dropped_delta = audio_bytes_dropped - dropped_before;
    const uint64_t accounted_delta = queued_delta + dropped_delta;
    if (accounted_delta < (uint64_t)len) {
        const uint64_t missing = (uint64_t)len - accounted_delta;
        audio_bytes_dropped += missing;
        ESP_LOGW(TAG, "Audio accounting guard: %llu byte -> dropped", (unsigned long long)missing);
    } else if (accounted_delta > (uint64_t)len) {
        ESP_LOGW(TAG, "Audio accounting anomaly: accounted_delta=%llu len=%u", (unsigned long long)accounted_delta, (unsigned)len);
    }

    xSemaphoreGive(audio_send_mutex);
    return queued_delta == (uint64_t)len;
}
