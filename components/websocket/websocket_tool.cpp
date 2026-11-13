#include "websocket_internal.h"
#include "uart_control.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static const char *TAG = "WS_TOOL";

static bool valid_tool_id(const char *id)
{
    if (!id || !id[0]) return false;
    for (const char *p = id; *p; ++p) {
        if (!( (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
               (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' )) return false;
    }
    return true;
}

static bool supported_tool_name(const char *name)
{
    return name && (
        strcmp(name, "control_device") == 0 ||
        strcmp(name, "memory_save") == 0 ||
        strcmp(name, "memory_delete") == 0 ||
        strcmp(name, "memory_read") == 0 ||
        strcmp(name, "web_search") == 0);
}

static bool send_tool_payload(const char *id, const char *name, const char *result)
{
    if (!id || !name || !result || !client || !is_connected || websocket_tx_error) return false;
    if (!esp_websocket_client_is_connected(client)) return false;
    if (!supported_tool_name(name)) return false;
    if (!valid_tool_id(id)) {
        ESP_LOGW(TAG, "Tool call id ditolak karena karakter tidak aman");
        return false;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *response = cJSON_AddObjectToObject(root, "toolResponse");
    cJSON *calls = cJSON_AddArrayToObject(response, "functionResponses");
    cJSON *call = cJSON_CreateObject();
    cJSON_AddStringToObject(call, "id", id);
    cJSON_AddStringToObject(call, "name", name);
    cJSON *body = cJSON_AddObjectToObject(call, "response");
    cJSON_AddStringToObject(body, "result", result);
    cJSON_AddItemToArray(calls, call);
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) return false;

    size_t len = strlen(payload);
    if (len == 0 || len > UINT16_MAX || !websocket_tx_queue) {
        cJSON_free(payload);
        return false;
    }
    uint8_t *copy = (uint8_t *)malloc(len);
    if (!copy) {
        cJSON_free(payload);
        return false;
    }
    memcpy(copy, payload, len);
    ws_tx_command_t cmd = {};
    cmd.type = WS_TX_COMMAND_TEXT;
    cmd.generation = websocket_connection_generation;
    cmd.len = (uint16_t)len;
    cmd.data = copy;
    if (xQueueSend(websocket_tx_queue, &cmd, 0) != pdTRUE) {
        free(copy);
        cJSON_free(payload);
        ESP_LOGW(TAG, "TX queue penuh; tool response ditahan: %s", name);
        return false;
    }
    ESP_LOGI(TAG, "Tool response masuk antrean TX: %s id=%s", name, id);
    cJSON_free(payload);
    return true;
}

bool websocket_send_tool_response(const char *id, const char *name, bool success)
{
    if (name && strcmp(name, "control_device") == 0) {
        const char *sensor_result = uart_control_take_last_sensor_response();
        if (sensor_result && sensor_result[0] != '\0')
            return send_tool_payload(id, name, sensor_result);
    }
    return send_tool_payload(id, name, success ? "ok" : "error");
}

bool websocket_send_tool_response_text(const char *id, const char *name, const char *result)
{
    if (!result || result[0] == '\0') return false;
    return send_tool_payload(id, name, result);
}
