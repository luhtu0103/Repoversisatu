#include "ota_update.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "OTA_UPDATE";
static constexpr char VERSION_URL[] =
    "https://github.com/luhtu0103/Repoversisatu/releases/latest/download/version.txt";
static constexpr char FIRMWARE_URL[] =
    "https://github.com/luhtu0103/Repoversisatu/releases/latest/download/firmware.bin";

struct version_buffer {
    char data[32];
    size_t length;
};

static esp_err_t version_event_handler(esp_http_client_event_t *event)
{
    if (event == nullptr || event->user_data == nullptr) return ESP_OK;
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data == nullptr || event->data_len <= 0) return ESP_OK;
    auto *buffer = static_cast<version_buffer *>(event->user_data);
    const size_t available = sizeof(buffer->data) - 1 - buffer->length;
    const size_t copy_len = static_cast<size_t>(event->data_len) < available
        ? static_cast<size_t>(event->data_len) : available;
    if (copy_len > 0) {
        memcpy(buffer->data + buffer->length, event->data, copy_len);
        buffer->length += copy_len;
        buffer->data[buffer->length] = '\0';
    }
    return ESP_OK;
}

static bool parse_version_number(const char *text, uint32_t *version)
{
    if (text == nullptr || version == nullptr) return false;
    while (*text == ' ' || *text == '\r' || *text == '\n' || *text == '\t') ++text;
    if (*text < '0' || *text > '9') return false;
    char *end = nullptr;
    unsigned long parsed = strtoul(text, &end, 10);
    if (end == text || parsed > UINT32_MAX) return false;
    while (*end == ' ' || *end == '\r' || *end == '\n' || *end == '\t') ++end;
    if (*end != '\0') return false;
    *version = static_cast<uint32_t>(parsed);
    return true;
}

esp_err_t ota_update_check_and_apply(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    uint32_t current_version = 0;
    if (app == nullptr || !parse_version_number(app->version, &current_version)) {
        ESP_LOGW(TAG, "Versi firmware lokal bukan angka; OTA dilewati (version='%s')",
                 app ? app->version : "(null)");
        return ESP_FAIL;
    }

    version_buffer remote = {};
    esp_http_client_config_t version_config = {};
    version_config.url = VERSION_URL;
    version_config.event_handler = version_event_handler;
    version_config.user_data = &remote;
    version_config.crt_bundle_attach = esp_crt_bundle_attach;
    version_config.timeout_ms = 15000;
    version_config.buffer_size = 1024;
    version_config.buffer_size_tx = 1024;

    esp_http_client_handle_t client = esp_http_client_init(&version_config);
    if (client == nullptr) return ESP_ERR_NO_MEM;

    esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "Gagal membaca versi release: err=%s HTTP=%d; OTA dilewati",
                 esp_err_to_name(err), status);
        return err != ESP_OK ? err : ESP_FAIL;
    }

    uint32_t remote_version = 0;
    if (!parse_version_number(remote.data, &remote_version)) {
        ESP_LOGW(TAG, "Isi version.txt tidak valid; OTA dilewati");
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_LOGI(TAG, "Firmware lokal=%lu, release=%lu",
             (unsigned long)current_version, (unsigned long)remote_version);
    if (remote_version <= current_version) {
        ESP_LOGI(TAG, "Firmware sudah terbaru");
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Update OTA dimulai ke versi %lu", (unsigned long)remote_version);
    esp_http_client_config_t ota_http_config = {};
    ota_http_config.url = FIRMWARE_URL;
    ota_http_config.crt_bundle_attach = esp_crt_bundle_attach;
    ota_http_config.timeout_ms = 30000;
    ota_http_config.buffer_size = 4096;
    ota_http_config.buffer_size_tx = 1024;
    ota_http_config.keep_alive_enable = true;

    esp_https_ota_config_t ota_config = {};
    ota_config.http_config = &ota_http_config;
    err = esp_https_ota(&ota_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA gagal: %s; firmware lama tetap dipakai", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "OTA berhasil; reboot ke firmware baru");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}
