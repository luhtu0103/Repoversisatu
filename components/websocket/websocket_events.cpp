#include "websocket_internal.h"
#include "display.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include <stdint.h>
#include <string.h>

static const char *TAG = "WS_EVENT";
static volatile bool lifecycle_invalidated = false;
static volatile bool websocket_cleanup_pending = false;
static volatile bool websocket_finish_received = false;

static void invalidate_connection_generation(void)
{
    if (lifecycle_invalidated) return;
    lifecycle_invalidated = true;
    websocket_connection_generation = websocket_connection_generation + 1;
    ESP_LOGW(TAG, "Connection generation invalidated: %lu",
             (unsigned long)websocket_connection_generation);
}

void websocket_event_handler(void *handler_args, esp_event_base_t base,
                             int32_t event_id, void *event_data)
{
    (void)base;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    esp_websocket_client_handle_t event_client =
        (esp_websocket_client_handle_t)handler_args;

    if (client != NULL && event_client != NULL && event_client != client) {
        ESP_LOGW(TAG, "Event dari client lama diabaikan: event=%ld", (long)event_id);
        return;
    }

    uint32_t now_ms = 0;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            websocket_last_rx_ms = (uint32_t)(esp_timer_get_time() / 1000);
            websocket_last_tx_ms = websocket_last_rx_ms;
            websocket_tx_fail_count = 0;
            ESP_LOGI(TAG, "WebSocket TERHUBUNG ke Gemini!");
            lifecycle_invalidated = false;
            websocket_cleanup_pending = false;
            websocket_finish_received = false;
            is_connected = true;
            setup_complete = false;
            websocket_tx_error = false;
            websocket_connection_generation = websocket_connection_generation + 1;
            ESP_LOGI(TAG, "Connection generation=%lu",
                     (unsigned long)websocket_connection_generation);
            websocket_tx_flush_queue();
            websocket_rx_flush_queue();
            websocket_rx_request_reset();
            request_audio_buffer_clear();
            display_status("AI terhubung");
            websocket_schedule_setup(websocket_connection_generation);
            break;

        case WEBSOCKET_EVENT_DATA:
            if (!data) break;
            if (!is_connected || websocket_tx_error) break;
            if (data->op_code == 0x08) {
                ESP_LOGW(TAG, "GEMINI CLOSE FRAME");
                if (data->data_ptr && data->data_len >= 2) {
                    uint16_t code = ((uint8_t)data->data_ptr[0] << 8) |
                                    (uint8_t)data->data_ptr[1];
                    ESP_LOGW(TAG, "CLOSE CODE: %u (0x%04X)",
                             (unsigned)code, (unsigned)code);
                }
                break;
            }
            if ((data->op_code == 0x00 || data->op_code == 0x01 || data->op_code == 0x02) &&
                data->data_ptr && data->data_len > 0) {
                websocket_last_rx_ms = (uint32_t)(esp_timer_get_time() / 1000);
                (void)websocket_rx_enqueue_data(data, websocket_connection_generation);
            }
            break;

        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "WebSocket Error!");
            ESP_LOGE(TAG, "HEAP at WS error: internal_free=%u internal_largest=%u internal_min_free=%u psram_free=%u psram_largest=%u",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            now_ms = (uint32_t)(esp_timer_get_time() / 1000);
            ESP_LOGE(TAG, "WS activity before error: connected=%d setup=%d tx_error=%d generation=%lu rx_age_ms=%lu tx_age_ms=%lu tx_fail_count=%lu",
                     (int)is_connected, (int)setup_complete, (int)websocket_tx_error,
                     (unsigned long)websocket_connection_generation,
                     websocket_last_rx_ms ? (unsigned long)(now_ms - websocket_last_rx_ms) : 0xFFFFFFFFUL,
                     websocket_last_tx_ms ? (unsigned long)(now_ms - websocket_last_tx_ms) : 0xFFFFFFFFUL,
                     (unsigned long)websocket_tx_fail_count);
            if (data) {
                ESP_LOGE(TAG,
                         "WS error_type=%d sock_errno=%d tls_esp_err=0x%x "
                         "tls_stack_err=0x%x handshake=%d",
                         (int)data->error_handle.error_type,
                         data->error_handle.esp_transport_sock_errno,
                         (unsigned)data->error_handle.esp_tls_last_esp_err,
                         (unsigned)data->error_handle.esp_tls_stack_err,
                         data->error_handle.esp_ws_handshake_status_code);
            }
            is_connected = false;
            setup_complete = false;
            websocket_tx_error = true;
            face_set_state(FACE_SLEEP);
            display_status("ERROR: AI");
            invalidate_connection_generation();
            websocket_tx_flush_queue();
            websocket_rx_flush_queue();
            websocket_rx_request_reset();
            request_audio_buffer_clear();
            /* Only mark cleanup pending. The websocket callback must never
             * close, abort, or destroy its own client. */
            websocket_cleanup_pending = true;
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
            now_ms = (uint32_t)(esp_timer_get_time() / 1000);
            ESP_LOGW(TAG, "WebSocket TERPUTUS dari Gemini: rx_age_ms=%lu tx_age_ms=%lu tx_fail_count=%lu generation=%lu",
                     websocket_last_rx_ms ? (unsigned long)(now_ms - websocket_last_rx_ms) : 0xFFFFFFFFUL,
                     websocket_last_tx_ms ? (unsigned long)(now_ms - websocket_last_tx_ms) : 0xFFFFFFFFUL,
                     (unsigned long)websocket_tx_fail_count,
                     (unsigned long)websocket_connection_generation);
            is_connected = false;
            setup_complete = false;
            websocket_tx_error = true;
            invalidate_connection_generation();
            websocket_tx_flush_queue();
            websocket_rx_flush_queue();
            websocket_rx_request_reset();
            request_audio_buffer_clear();
            display_status("AI terputus");
            if (session_resumable && session_handle[0] != '\0')
                ESP_LOGI(TAG, "Session resumption handle dipertahankan");
            websocket_cleanup_pending = true;
            websocket_request_resume_reconnect();
            break;

        case WEBSOCKET_EVENT_CLOSED:
            ESP_LOGW(TAG, "WebSocket CLOSED");
            is_connected = false;
            setup_complete = false;
            websocket_tx_error = true;
            invalidate_connection_generation();
            websocket_tx_flush_queue();
            websocket_rx_flush_queue();
            websocket_rx_request_reset();
            request_audio_buffer_clear();
            display_status("AI terputus");
            websocket_cleanup_pending = true;
            websocket_request_resume_reconnect();
            break;

        case WEBSOCKET_EVENT_FINISH:
            ESP_LOGI(TAG, "WebSocket FINISH");
            /* FINISH is the hand-off point: the websocket task has finished
             * its lifecycle. A separate manager task may now destroy client. */
            websocket_finish_received = true;
            websocket_cleanup_pending = true;
            websocket_reset_started();
            break;

        default:
            break;
    }
}

bool websocket_cleanup_is_pending(void)
{
    return websocket_cleanup_pending && websocket_finish_received;
}

void websocket_cleanup_complete(void)
{
    websocket_cleanup_pending = false;
    websocket_finish_received = false;
}
