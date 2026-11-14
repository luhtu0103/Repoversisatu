#include "websocket_internal.h"
#include "web_config.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static const char *TAG = "WS_TX";

const char *websocket_get_server_url(void)
{
    static char url[256];
    char api_key[128] = {0};

    if (!web_config_load_api_key(api_key, sizeof(api_key))) {
        ESP_LOGE(TAG, "Gemini API key tidak ditemukan di NVS");
        return NULL;
    }

    const size_t key_len = strlen(api_key);
    char prefix[5] = "";
    char suffix[5] = "";
    if (key_len >= 4) {
        memcpy(prefix, api_key, 4);
        memcpy(suffix, api_key + key_len - 4, 4);
        prefix[4] = '\0';
        suffix[4] = '\0';
    }

    ESP_LOGI(TAG, "API key runtime: len=%u prefix=%s suffix=%s valid=%s",
             (unsigned)key_len,
             prefix,
             key_len >= 4 ? suffix : "-",
             web_config_api_key_is_valid(api_key) ? "YES" : "NO");

    if (!web_config_api_key_is_valid(api_key)) {
        ESP_LOGE(TAG, "API key NVS tidak valid; WebSocket URL tidak dibuat");
        return NULL;
    }

    int n = snprintf(
        url, sizeof(url),
        "wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=%s",
        api_key);

    if (n < 0 || (size_t)n >= sizeof(url)) {
        ESP_LOGE(TAG, "WebSocket URL terlalu panjang");
        return NULL;
    }

    return url;
}

bool websocket_send_audio_data(
    const uint8_t *data,
    size_t len
)
{
    if (!data || len == 0 || len > WS_TX_AUDIO_SIZE) {
        if (len > WS_TX_AUDIO_SIZE)
            ESP_LOGE(TAG, "Audio frame terlalu besar: %u byte", (unsigned)len);
        return false;
    }

    if (!is_connected || !setup_complete || websocket_tx_error) {
        static uint32_t status_rejects = 0;
        if ((++status_rejects % 50U) == 1U)
            ESP_LOGW(TAG, "Audio enqueue ditahan: status_invalid connected=%d setup=%d tx_error=%d",
                     (int)is_connected, (int)setup_complete, (int)websocket_tx_error);
        return false;
    }

    uint32_t generation = websocket_connection_generation;
    ws_tx_enqueue_result_t reason = WS_TX_ENQUEUE_OK;
    bool queued = websocket_tx_enqueue_audio(data, len, generation, &reason);
    if (!queued) {
        static uint32_t rejects = 0;
        if ((++rejects % 10U) == 1U)
            ESP_LOGW(TAG, "Audio enqueue gagal: reason=%d generation=%lu queue_messages=%u",
                     (int)reason, (unsigned long)generation,
                     websocket_tx_queue ? (unsigned)uxQueueMessagesWaiting(websocket_tx_queue) : 0U);
    }
    return queued;
}

bool websocket_send_touch_prompt(bool angry)
{
    const char *prompt_json = angry
        ? R"json({"clientContent":{"turns":[{"role":"user","parts":[{"text":"Aku sedang kesal karena disentuh berkali-kali. Tolong tanggapi dengan nada bercanda dan ramah dalam bahasa Indonesia."}]}],"turnComplete":true}})json"
        : R"json({"clientContent":{"turns":[{"role":"user","parts":[{"text":"Aku senang kamu mengelusku! Sapa aku dengan hangat dalam bahasa Indonesia."}]}],"turnComplete":true}})json";
    if (!websocket_is_connected() || !websocket_tx_queue) {
        ESP_LOGI(TAG, "Respons suara sentuhan dilewati: sesi Gemini belum siap");
        return false;
    }
    const size_t len = strlen(prompt_json);
    if (len == 0 || len > UINT16_MAX) return false;
    uint8_t *copy = (uint8_t *)malloc(len);
    if (!copy) {
        ESP_LOGW(TAG, "Touch prompt ditahan: malloc gagal");
        return false;
    }
    memcpy(copy, prompt_json, len);
    ws_tx_command_t cmd = {};
    cmd.type = WS_TX_COMMAND_TEXT;
    cmd.generation = websocket_connection_generation;
    cmd.len = (uint16_t)len;
    cmd.data = copy;
    if (xQueueSend(websocket_tx_queue, &cmd, 0) != pdTRUE) {
        websocket_set_backpressured(true);
        free(copy);
        ESP_LOGW(TAG, "Touch prompt ditahan: TX queue penuh");
        return false;
    }
    ESP_LOGI(TAG, "Touch prompt masuk antrean TX (%s)", angry ? "MARAH" : "SENANG");
    return true;
}
