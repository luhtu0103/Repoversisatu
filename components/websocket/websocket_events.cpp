#include "websocket_internal.h"
#include "display.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_timer.h"
#include "esp_wifi.h"

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
                    if (data->data_len > 2) {
                        char reason[96] = {};
                        size_t rl = (size_t)data->data_len - 2;
                        if (rl > sizeof(reason) - 1) rl = sizeof(reason) - 1;
                        memcpy(reason, data->data_ptr + 2, rl);
                        for (size_t i = 0; i < rl; ++i)
                            if ((unsigned char)reason[i] < 0x20) reason[i] = ' ';
                        ESP_LOGW(TAG, "CLOSE ALASAN dari Gemini: %s", reason);
                    }
                    /* 1008 = ditolak server. Setelah putus mendadak, melanjutkan sesi dengan handle lama
                     * ditolak terus-menerus (lingkaran sambung ulang sampai 15 detik). Buang handle supaya
                     * sambung ulang berikutnya memakai sesi Gemini yang baru dan langsung berhasil. */
                    if (code == 1008 && session_handle[0] != '\0') {
                        ESP_LOGW(TAG, "Handle sesi ditolak server (1008); dibuang, sambung ulang memakai sesi baru");
                        clear_session_handle();
                    }
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
            {
                wifi_ap_record_t ap = {};
                if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
                    ESP_LOGE(TAG, "Wi-Fi saat error: RSSI=%d dBm kanal=%d (lebih kecil dari -75 = sinyal lemah)",
                             (int)ap.rssi, (int)ap.primary);
                else
                    ESP_LOGE(TAG, "Wi-Fi saat error: tidak terhubung ke AP");
            }
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
            /* Recover even if the transport reports ERROR without a later
             * DISCONNECTED event. Cleanup/stop remains owned by the manager task. */
            websocket_request_resume_reconnect();
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
            /* FINISH tanpa ERROR/DISCONNECTED/CLOSED sebelumnya (mis. koneksi mati saat
             * setup dikirim): tanpa baris ini tidak ada yang menjadwalkan sambung ulang,
             * lalu sesi berakhir setelah 15 detik. Fungsi ini aman: ia melewati sendiri
             * kasus standby/sesi baru dan kasus yang sudah dijadwalkan. */
            websocket_request_resume_reconnect();
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
