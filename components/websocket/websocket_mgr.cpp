#include "websocket_mgr.h"
#include "websocket_internal.h"
#include "display.h"
#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "mbedtls/base64.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <stddef.h>
#include <stdint.h>

static const char *TAG = "WS_MGR";
esp_websocket_client_handle_t client = NULL;
volatile bool is_connected = false;
volatile bool setup_complete = false;
volatile bool websocket_tx_error = false;
volatile uint32_t websocket_connection_generation = 0;
volatile uint32_t websocket_last_rx_ms = 0;
volatile uint32_t websocket_last_tx_ms = 0;
volatile uint32_t websocket_tx_fail_count = 0;
char session_handle[SESSION_HANDLE_MAX_LEN] = {0};
bool session_resumable = false;

StreamBufferHandle_t audio_stream = NULL;
TaskHandle_t audio_playback_task_handle = NULL;
volatile bool audio_turn_active = false;
volatile bool audio_turn_complete_pending = false;
uint32_t audio_chunks_received = 0;
uint64_t audio_bytes_received = 0;
uint64_t audio_bytes_queued = 0;
uint32_t audio_write_calls = 0;
uint64_t audio_bytes_played = 0;
uint64_t audio_bytes_dropped = 0;
uint64_t audio_bytes_playback_dropped = 0;
static volatile bool ws_started = false;
QueueHandle_t websocket_tx_queue = NULL;
TaskHandle_t websocket_tx_task_handle = NULL;
QueueHandle_t websocket_rx_queue = NULL;
TaskHandle_t websocket_rx_task_handle = NULL;
static TaskHandle_t websocket_cleanup_task_handle = NULL;
/* Serialize TX writes with stop/destroy during WebSocket cleanup. */
static SemaphoreHandle_t websocket_client_api_mutex = NULL;
static volatile bool websocket_start_pending = false;
static volatile bool websocket_standby_request = false;
static volatile bool websocket_fresh_session_reset_pending = false;
/* Only the lifecycle cleanup worker may close the client. TX, main, and
 * reconnect paths request closure here to avoid concurrent close/abort calls. */
static volatile bool websocket_close_requested = false;

static void log_ws_heap(const char *stage)
{
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "HEAP[%s]: internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u",
             stage, (unsigned)internal_free, (unsigned)internal_largest,
             (unsigned)psram_free, (unsigned)psram_largest);
}

void websocket_tx_flush_queue(void)
{
    if (!websocket_tx_queue) return;
    ws_tx_command_t stale = {};
    size_t flushed = 0;
    while (xQueueReceive(websocket_tx_queue, &stale, 0) == pdTRUE) {
        if (stale.data) free(stale.data);
        flushed++;
    }
    if (flushed) ESP_LOGW(TAG, "TX queue dibersihkan: %u command", (unsigned)flushed);
}

static int websocket_send_text_locked(esp_websocket_client_handle_t ws, const char *data, int len, TickType_t timeout)
{
    if (!websocket_client_api_mutex || xSemaphoreTake(websocket_client_api_mutex, portMAX_DELAY) != pdTRUE) return -1;
    int sent = -1;
    if (ws != NULL && client == ws && is_connected && !websocket_tx_error && esp_websocket_client_is_connected(ws))
        sent = esp_websocket_client_send_text(ws, data, len, timeout);
    xSemaphoreGive(websocket_client_api_mutex);
    return sent;
}

static void websocket_cleanup_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "WebSocket lifecycle cleanup worker siap");
    for (;;) {
        esp_websocket_client_handle_t close_target = client;
        if (websocket_close_requested) {
            if (close_target == NULL) {
                websocket_close_requested = false;
            } else if (esp_websocket_client_is_connected(close_target)) {
                esp_err_t close_err = esp_websocket_client_close(close_target, pdMS_TO_TICKS(1000));
                if (close_err == ESP_OK) {
                    websocket_close_requested = false;
                    ESP_LOGI(TAG, "Cleanup worker: permintaan close WebSocket diproses");
                } else {
                    ESP_LOGW(TAG, "Cleanup worker: close WebSocket akan dicoba lagi, err=0x%x", (unsigned)close_err);
                }
            } else {
                /* A disconnected socket is not proof that the WebSocket task
                 * has exited. Stop it outside the event callback, then wait
                 * for WEBSOCKET_EVENT_FINISH before destroying the client. */
                esp_err_t stop_err = ESP_FAIL;
                if (websocket_client_api_mutex && xSemaphoreTake(websocket_client_api_mutex, portMAX_DELAY) == pdTRUE) {
                    if (client == close_target) stop_err = esp_websocket_client_stop(close_target);
                    else stop_err = ESP_ERR_INVALID_STATE;
                    xSemaphoreGive(websocket_client_api_mutex);
                }
                if (stop_err == ESP_OK) {
                    websocket_close_requested = false;
                    ESP_LOGI(TAG, "Cleanup worker: stop diminta pada client terputus; menunggu FINISH");
                } else {
                    ESP_LOGW(TAG, "Cleanup worker: stop client terputus akan dicoba lagi, err=0x%x", (unsigned)stop_err);
                }
            }
        }
        if (websocket_cleanup_is_pending()) {
            esp_websocket_client_handle_t ws = client;
            if (ws != NULL && !esp_websocket_client_is_connected(ws)) {
                ESP_LOGI(TAG, "Cleanup worker: destroy client dari task manager");
                esp_err_t err = ESP_FAIL;
                if (websocket_client_api_mutex && xSemaphoreTake(websocket_client_api_mutex, portMAX_DELAY) == pdTRUE) {
                    if (client == ws) err = esp_websocket_client_destroy(ws);
                    else err = ESP_ERR_INVALID_STATE;
                    if (err == ESP_OK) client = NULL;
                    xSemaphoreGive(websocket_client_api_mutex);
                }
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "Cleanup worker: client berhasil dihancurkan");
                    if (websocket_fresh_session_reset_pending) {
                        websocket_fresh_session_reset_pending = false;
                        clear_session_handle();
                        clear_audio_buffer();
                        reset_audio_turn_stats();
                        websocket_tx_flush_queue();
                        ESP_LOGI(TAG, "FRESH SESSION: handle Gemini, audio, RX/TX state dibersihkan");
                    }
                    websocket_cleanup_complete();
                    if (websocket_start_pending && !websocket_fresh_session_reset_pending) {
                        websocket_start_pending = false;
                        ESP_LOGW(TAG, "Cleanup worker: memulai ulang Gemini dengan session-resumption handle");
                        websocket_app_start();
                    }
                } else {
                    ESP_LOGW(TAG, "Cleanup worker: destroy ditunda, err=0x%x", (unsigned)err);
                }
            } else if (ws == NULL) {
                if (websocket_fresh_session_reset_pending) {
                    websocket_fresh_session_reset_pending = false;
                    clear_session_handle();
                    clear_audio_buffer();
                    reset_audio_turn_stats();
                    websocket_tx_flush_queue();
                    ESP_LOGI(TAG, "FRESH SESSION: state dibersihkan tanpa client aktif");
                }
                websocket_cleanup_complete();
                if (websocket_start_pending && !websocket_fresh_session_reset_pending) {
                    websocket_start_pending = false;
                    ESP_LOGW(TAG, "Cleanup worker: memulai ulang Gemini dengan session-resumption handle");
                    websocket_app_start();
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void websocket_tx_task(void *arg)
{
    (void)arg;
    ws_tx_command_t cmd = {};
    ESP_LOGI(TAG, "WebSocket TX worker dimulai - V7.0.32");
    for (;;) {
        if (xQueueReceive(websocket_tx_queue, &cmd, portMAX_DELAY) != pdTRUE) continue;
        uint8_t *audio_data = cmd.data;
        cmd.data = NULL;
        if (cmd.generation != websocket_connection_generation || !is_connected || websocket_tx_error || !client) { free(audio_data); continue; }
        esp_websocket_client_handle_t ws = client;
        if (!esp_websocket_client_is_connected(ws)) { free(audio_data); continue; }
        if (cmd.type == WS_TX_COMMAND_SETUP) {
            char *setup_json = NULL; size_t setup_len = 0;
            if (!build_gemini_setup(&setup_json, &setup_len)) { free(audio_data); continue; }
            if (cmd.generation != websocket_connection_generation || !is_connected || websocket_tx_error || client != ws || !esp_websocket_client_is_connected(ws)) {
                free(setup_json); free(audio_data); continue;
            }
            int64_t send_start_us = esp_timer_get_time();
            int sent = websocket_send_text_locked(ws, setup_json, (int)setup_len, pdMS_TO_TICKS(5000));
            if (sent == (int)setup_len) websocket_last_tx_ms = (uint32_t)(esp_timer_get_time() / 1000);
            else __atomic_add_fetch(&websocket_tx_fail_count, 1U, __ATOMIC_RELAXED);
            int64_t send_elapsed_ms = (esp_timer_get_time() - send_start_us) / 1000;
            if (send_elapsed_ms >= 100 || sent != (int)setup_len)
                ESP_LOGW(TAG, "TX setup send_text duration=%lldms sent=%d expected=%u",
                         (long long)send_elapsed_ms, sent, (unsigned)setup_len);
            if (sent != (int)setup_len) {
                /* A failed setup write is not by itself a confirmed socket error.
                 * Keep closure to WebSocket lifecycle events or explicit STANDBY;
                 * the existing 15-second connection timeout handles setup stalls. */
                ESP_LOGE(TAG, "TX setup gagal: sent=%d expected=%u; menunggu event WebSocket/timeout 15 detik",
                         sent, (unsigned)setup_len);
            } else {
                ESP_LOGI(TAG, "Setup Gemini terkirim: %d byte generation=%lu duration=%lldms", sent, (unsigned)cmd.generation, (long long)send_elapsed_ms);
            }
            free(setup_json); free(audio_data); continue;
        }
        if (cmd.type == WS_TX_COMMAND_TEXT) {
            if (!audio_data || cmd.len == 0) { free(audio_data); continue; }
            if (cmd.generation != websocket_connection_generation || !is_connected || websocket_tx_error ||
                client != ws || !esp_websocket_client_is_connected(ws)) {
                free(audio_data);
                continue;
            }
            int sent = websocket_send_text_locked(ws, (const char *)audio_data, (int)cmd.len, pdMS_TO_TICKS(1000));
            if (sent == (int)cmd.len) websocket_last_tx_ms = (uint32_t)(esp_timer_get_time() / 1000);
            else __atomic_add_fetch(&websocket_tx_fail_count, 1U, __ATOMIC_RELAXED);
            if (sent != (int)cmd.len)
                ESP_LOGW(TAG, "Touch prompt TX gagal: sent=%d expected=%u", sent, (unsigned)cmd.len);
            else
                ESP_LOGI(TAG, "Touch prompt terkirim ke Gemini: %u byte", (unsigned)cmd.len);
            free(audio_data);
            continue;
        }
        if (cmd.type == WS_TX_COMMAND_AUDIO) {
            static char *b64_buf = NULL;
            static char *json_buf = NULL;
            if (!b64_buf) {
                if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0)
                    b64_buf = (char *)heap_caps_malloc(1800, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!b64_buf)
                    b64_buf = (char *)heap_caps_malloc(1800, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            }
            if (!json_buf) {
                if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0)
                    json_buf = (char *)heap_caps_malloc(2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!json_buf)
                    json_buf = (char *)heap_caps_malloc(2048, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            }
            if (!b64_buf || !json_buf) {
                ESP_LOGE(TAG, "TX buffer allocation gagal (base64/json)");
                free(audio_data);
                continue;
            }
            // 1280 PCM bytes = 40 ms at 16 kHz mono PCM16; reduces per-send overhead.
            constexpr size_t PCM_SEND_CHUNK = 1280;
            // Fail fast enough that a stalled socket cannot consume the
            // entire bounded microphone capture budget before TX recovery starts.
            /* Match RepoV3: allow TLS/WebSocket writes enough time under load. */
            constexpr TickType_t AUDIO_SEND_TIMEOUT = pdMS_TO_TICKS(3000);
            constexpr TickType_t AUDIO_SEND_RETRY_DELAY = pdMS_TO_TICKS(30);
            constexpr int AUDIO_SEND_RETRIES = 1;
            if (!audio_data || !cmd.len) { free(audio_data); continue; }

            size_t offset = 0;
            bool send_failed = false;
            while (offset < cmd.len) {
                if (cmd.generation != websocket_connection_generation || !is_connected || websocket_tx_error || client != ws || !esp_websocket_client_is_connected(ws)) {
                    send_failed = true;
                    break;
                }
                size_t chunk_len = cmd.len - offset;
                if (chunk_len > PCM_SEND_CHUNK) chunk_len = PCM_SEND_CHUNK;
                size_t encoded_len = 0;
                int ret = mbedtls_base64_encode((unsigned char *)b64_buf, 1800 - 1, &encoded_len, audio_data + offset, chunk_len);
                if (ret != 0) {
                    ESP_LOGW(TAG, "TX audio base64 gagal: ret=%d chunk=%u", ret, (unsigned)chunk_len);
                    send_failed = true;
                    break;
                }
                b64_buf[encoded_len] = '\0';
                int json_len = snprintf(json_buf, 2048, "{\"realtimeInput\":{\"audio\":{\"mimeType\":\"audio/pcm;rate=16000\",\"data\":\"%s\"}}}", b64_buf);
                if (json_len < 0 || (size_t)json_len >= 2048) {
                    ESP_LOGW(TAG, "TX audio JSON terlalu besar: chunk=%u", (unsigned)chunk_len);
                    send_failed = true;
                    break;
                }
                bool chunk_sent = false;
                for (int attempt = 0; attempt <= AUDIO_SEND_RETRIES; ++attempt) {
                    if (cmd.generation != websocket_connection_generation || !is_connected || websocket_tx_error || client != ws || !esp_websocket_client_is_connected(ws)) break;
                    if (attempt > 0) {
                        vTaskDelay(AUDIO_SEND_RETRY_DELAY);
                        if (cmd.generation != websocket_connection_generation || !is_connected || websocket_tx_error || client != ws || !esp_websocket_client_is_connected(ws)) break;
                    }
                    int64_t send_start_us = esp_timer_get_time();
                    int sent = websocket_send_text_locked(ws, json_buf, json_len, AUDIO_SEND_TIMEOUT);
                    if (sent == json_len) websocket_last_tx_ms = (uint32_t)(esp_timer_get_time() / 1000);
                    else __atomic_add_fetch(&websocket_tx_fail_count, 1U, __ATOMIC_RELAXED);
                    int64_t send_elapsed_ms = (esp_timer_get_time() - send_start_us) / 1000;
                    if (sent == json_len) {
                        if (send_elapsed_ms >= 100)
                            ESP_LOGW(TAG, "TX audio send_text slow: duration=%lldms bytes=%d pcm_chunk=%u offset=%u/%u",
                                     (long long)send_elapsed_ms, json_len, (unsigned)chunk_len, (unsigned)offset, (unsigned)cmd.len);
                        chunk_sent = true;
                        break;
                    }
                    ESP_LOGW(TAG, "TX audio write timeout/fail: attempt=%d sent=%d expected=%d duration=%lldms pcm_chunk=%u offset=%u/%u timeout=3000ms",
                             attempt + 1, sent, json_len, (long long)send_elapsed_ms, (unsigned)chunk_len, (unsigned)offset, (unsigned)cmd.len);
                }
                if (!chunk_sent) { send_failed = true; break; }
                offset += chunk_len;
            }
            if (send_failed) {
                /* Match RepoV3: a command that exhausts its bounded audio-send
                 * retries is dropped, but a failed write alone does not close
                 * the WebSocket or poison the whole Gemini session. A genuine
                 * transport disconnect is handled by WebSocket lifecycle events. */
                ESP_LOGE(TAG, "TX audio command gagal setelah retry; command dibuang, WebSocket dipertahankan: sent_pcm=%u/%u command_generation=%lu current_generation=%lu connected=%d socket_connected=%d",
                         (unsigned)offset, (unsigned)cmd.len,
                         (unsigned long)cmd.generation,
                         (unsigned long)websocket_connection_generation,
                         (int)is_connected,
                         (int)esp_websocket_client_is_connected(ws));
            }
            free(audio_data);
            continue;
        }
        free(audio_data);
    }
}

bool websocket_tx_init(void)
{
    if (!websocket_client_api_mutex) {
        websocket_client_api_mutex = xSemaphoreCreateMutex();
        if (!websocket_client_api_mutex) return false;
    }
    if (!websocket_tx_queue) {
        websocket_tx_queue = xQueueCreate(WS_TX_QUEUE_LENGTH, sizeof(ws_tx_command_t));
        if (!websocket_tx_queue) return false;
    }
    if (!websocket_tx_task_handle) {
        if (xTaskCreate(websocket_tx_task, "ws_tx", 8192, NULL, 4, &websocket_tx_task_handle) != pdPASS) return false;
    }
    if (!websocket_cleanup_task_handle) {
        if (xTaskCreate(websocket_cleanup_task, "ws_cleanup", 3072, NULL, 3, &websocket_cleanup_task_handle) != pdPASS) return false;
    }
    log_ws_heap("after_ws_tx_init");
    return true;
}

bool websocket_tx_enqueue_audio(const uint8_t *data, size_t len, uint32_t generation, ws_tx_enqueue_result_t *result)
{
    if (result) *result = WS_TX_ENQUEUE_OK;
    if (!data || !len || len > WS_TX_AUDIO_SIZE) {
        if (result) *result = WS_TX_ENQUEUE_INVALID_FRAME;
        return false;
    }
    if (!websocket_tx_queue) {
        if (result) *result = WS_TX_ENQUEUE_QUEUE_UNAVAILABLE;
        return false;
    }
    if (!is_connected || !setup_complete || websocket_tx_error) {
        if (result) *result = WS_TX_ENQUEUE_STATUS_INVALID;
        return false;
    }
    if (generation != websocket_connection_generation) {
        if (result) *result = WS_TX_ENQUEUE_GENERATION_MISMATCH;
        return false;
    }
    uint8_t *copy = (uint8_t *)malloc(len);
    if (!copy) {
        if (result) *result = WS_TX_ENQUEUE_MALLOC_FAILED;
        return false;
    }
    memcpy(copy, data, len);
    ws_tx_command_t cmd = {};
    cmd.type = WS_TX_COMMAND_AUDIO; cmd.generation = generation; cmd.len = (uint16_t)len; cmd.data = copy;
    /* Preserve queued PCM on backpressure. The caller retains and retries this
     * frame when enqueue fails, instead of silently evicting older audio. */
    if (xQueueSend(websocket_tx_queue, &cmd, 0) != pdTRUE) {
        free(copy);
        if (result) *result = WS_TX_ENQUEUE_QUEUE_FULL;
        return false;
    }
    return true;
}

void websocket_schedule_setup(uint32_t generation)
{
    if (!websocket_tx_queue || !is_connected || websocket_tx_error || generation != websocket_connection_generation) return;
    ws_tx_command_t cmd = {};
    cmd.type = WS_TX_COMMAND_SETUP; cmd.generation = generation;
    if (xQueueSend(websocket_tx_queue, &cmd, pdMS_TO_TICKS(1000)) == pdTRUE) ESP_LOGI(TAG, "Setup Gemini dijadwalkan melalui TX worker: generation=%lu", (unsigned long)generation);
}

void websocket_app_start(void)
{
    ESP_LOGI(TAG, "Memulai Gemini WebSocket V7.0.22");
    if (!wifi_is_ready() || client || ws_started) return;
    log_ws_heap("before_audio_playback");
    if (!start_audio_playback()) return;
    log_ws_heap("after_audio_playback");
    clear_audio_buffer(); reset_audio_turn_stats(); reset_rx_buffer(); websocket_tx_flush_queue();
    if (!websocket_tx_init() || !websocket_rx_init()) return;
    log_ws_heap("after_ws_tx_rx_init");
    is_connected = false; setup_complete = false; websocket_tx_error = false; ws_started = false;
    esp_websocket_client_config_t cfg = {};
    cfg.uri = WEBSOCKET_SERVER_URL;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.skip_cert_common_name_check = false;
    cfg.cert_common_name = "generativelanguage.googleapis.com";
    cfg.network_timeout_ms = 15000;
    cfg.disable_auto_reconnect = true;
    /* Match RepoV3's transport keepalive policy. Keep Repo5's smaller TLS
     * buffer to reduce internal-heap pressure on ESP32-S3. */
    cfg.keep_alive_enable = true;
    cfg.keep_alive_idle = 30;
    cfg.keep_alive_interval = 10;
    cfg.keep_alive_count = 3;
    cfg.buffer_size = 4096;
    ESP_LOGI(TAG, "WS keepalive: TCP idle=30s interval=10s count=3; audio write timeout=3000ms retry=1");
    log_ws_heap("before_websocket_client_init");

    if (websocket_client_api_mutex && xSemaphoreTake(websocket_client_api_mutex, portMAX_DELAY) == pdTRUE) {
        client = esp_websocket_client_init(&cfg);
        xSemaphoreGive(websocket_client_api_mutex);
    } else client = NULL;
    if (!client) {
        log_ws_heap("websocket_client_init_FAILED");
        return;
    }
    log_ws_heap("after_websocket_client_init");
    esp_err_t err;
    if (websocket_client_api_mutex && xSemaphoreTake(websocket_client_api_mutex, portMAX_DELAY) == pdTRUE) {
        err = esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void *)client);
        xSemaphoreGive(websocket_client_api_mutex);
    } else err = ESP_FAIL;
    if (err != ESP_OK) { esp_websocket_client_destroy(client); client = NULL; return; }
    if (websocket_client_api_mutex && xSemaphoreTake(websocket_client_api_mutex, portMAX_DELAY) == pdTRUE) {
        err = esp_websocket_client_start(client);
        xSemaphoreGive(websocket_client_api_mutex);
    } else err = ESP_FAIL;
    if (err != ESP_OK) { esp_websocket_client_destroy(client); client = NULL; return; }
    ws_started = true;
}

bool websocket_is_connected(void)
{
    return is_connected && setup_complete && !websocket_tx_error;
}

bool websocket_lifecycle_busy(void)
{
    return client != NULL || ws_started || websocket_cleanup_is_pending();
}

/* Transport may be up while Gemini setup is still stalled. */
bool websocket_transport_connected(void)
{
    return is_connected;
}

void websocket_disconnect(void)
{
    /* Closure is serialized by websocket_cleanup_task; callers only request it. */
    websocket_close_requested = true;
}

void websocket_request_resume_reconnect(void)
{
    /* Explicit standby/fresh-session reset must win over an old reconnect. */
    if (websocket_fresh_session_reset_pending || websocket_standby_request) {
        ESP_LOGI(TAG, "Resume reconnect dilewati: perangkat sedang menuju standby/sesi baru");
        return;
    }
    if (websocket_start_pending) return; /* one reconnect attempt per disconnect */
    if (!client || !session_resumable || session_handle[0] == '\0') {
        ESP_LOGW(TAG, "Resume reconnect tidak tersedia: client/handle tidak valid");
        return;
    }
    websocket_start_pending = true;
    ESP_LOGW(TAG, "Koneksi terputus/GOAWAY: jadwalkan reconnect dengan handle sesi Gemini");
    websocket_disconnect();
}

bool websocket_take_standby_request(void)
{
    bool requested = websocket_standby_request;
    websocket_standby_request = false;
    return requested;
}

void websocket_request_standby(void)
{
    websocket_standby_request = true;
}

void websocket_request_fresh_session_reset(void)
{
    /* Fresh-session reset permanently cancels any old-session continuation. */
    websocket_start_pending = false;
    websocket_fresh_session_reset_pending = true;
    websocket_standby_request = false;
    if (client == NULL) {
        websocket_fresh_session_reset_pending = false;
        clear_session_handle();
        clear_audio_buffer();
        reset_audio_turn_stats();
        reset_rx_buffer();
        websocket_tx_flush_queue();
        ESP_LOGI(TAG, "FRESH SESSION: state dibersihkan (client sudah NULL)");
    }
}

void websocket_reset_started(void)
{
    ws_started = false;
}
