#include "alarm.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include <time.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "ALARM";
static const char *NS = "alarmcfg";
static alarm_info_t current_alarm = {};
static bool due_pending = false;
static portMUX_TYPE alarm_mux = portMUX_INITIALIZER_UNLOCKED;

static bool persist_alarm(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = nvs_set_u8(h, "active", current_alarm.active ? 1 : 0);
    if (e == ESP_OK) e = nvs_set_i32(h, "hour", current_alarm.hour);
    if (e == ESP_OK) e = nvs_set_i32(h, "minute", current_alarm.minute);
    if (e == ESP_OK) e = nvs_set_i64(h, "target", current_alarm.target_epoch);
    if (e == ESP_OK) e = nvs_set_str(h, "label", current_alarm.label);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}

bool alarm_init(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "NVS alarm belum tersedia; alarm dimulai kosong");
        return false;
    }
    uint8_t active = 0;
    int32_t hour = 0, minute = 0;
    int64_t target = 0;
    size_t label_len = sizeof(current_alarm.label);
    (void)nvs_get_u8(h, "active", &active);
    (void)nvs_get_i32(h, "hour", &hour);
    (void)nvs_get_i32(h, "minute", &minute);
    (void)nvs_get_i64(h, "target", &target);
    esp_err_t label_err = nvs_get_str(h, "label", current_alarm.label, &label_len);
    nvs_close(h);
    if (label_err != ESP_OK) strlcpy(current_alarm.label, "Alarm", sizeof(current_alarm.label));
    current_alarm.active = active != 0 && hour >= 0 && hour <= 23 &&
        minute >= 0 && minute <= 59 && target > 0;
    current_alarm.hour = hour;
    current_alarm.minute = minute;
    current_alarm.target_epoch = target;
    ESP_LOGI(TAG, "Alarm NVS: active=%d time=%02d:%02d label=%s target=%lld",
             current_alarm.active, hour, minute, current_alarm.label, (long long)target);
    return true;
}

bool alarm_set(int hour, int minute, const char *label, char *result, size_t result_size)
{
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || !label || !label[0]) {
        if (result && result_size) snprintf(result, result_size, "Gagal: waktu atau keterangan alarm tidak valid.");
        return false;
    }
    time_t now = time(NULL);
    struct tm local = {};
    localtime_r(&now, &local);
    if (local.tm_year < (2024 - 1900)) {
        if (result && result_size) snprintf(result, result_size, "Gagal: waktu perangkat belum tersinkronisasi. Coba lagi setelah internet dan NTP siap.");
        return false;
    }
    local.tm_hour = hour;
    local.tm_min = minute;
    local.tm_sec = 0;
    time_t target = mktime(&local);
    if (target <= now) {
        local.tm_mday += 1;
        target = mktime(&local);
    }
    portENTER_CRITICAL(&alarm_mux);
    current_alarm.active = true;
    current_alarm.hour = hour;
    current_alarm.minute = minute;
    current_alarm.target_epoch = (int64_t)target;
    strlcpy(current_alarm.label, label, sizeof(current_alarm.label));
    portEXIT_CRITICAL(&alarm_mux);
    if (!persist_alarm()) {
        portENTER_CRITICAL(&alarm_mux);
        current_alarm.active = false;
        portEXIT_CRITICAL(&alarm_mux);
        if (result && result_size) snprintf(result, result_size, "Gagal menyimpan alarm ke NVS.");
        return false;
    }
    ESP_LOGI(TAG, "Alarm disetel %02d:%02d WITA label=%s target=%lld",
             hour, minute, current_alarm.label, (long long)target);
    if (result && result_size) snprintf(result, result_size,
        "Alarm aktif pukul %02d:%02d WITA. Keterangan: %s. Alarm akan berbunyi melalui ucapan Gemini.",
        hour, minute, current_alarm.label);
    return true;
}

bool alarm_cancel(char *result, size_t result_size)
{
    portENTER_CRITICAL(&alarm_mux);
    current_alarm.active = false;
    due_pending = false;
    portEXIT_CRITICAL(&alarm_mux);
    bool ok = persist_alarm();
    if (result && result_size) snprintf(result, result_size, "%s",
        ok ? "Alarm berhasil dibatalkan." : "Gagal menyimpan pembatalan alarm.");
    return ok;
}

bool alarm_get(alarm_info_t *info)
{
    if (!info) return false;
    portENTER_CRITICAL(&alarm_mux);
    *info = current_alarm;
    portEXIT_CRITICAL(&alarm_mux);
    return true;
}

bool alarm_take_due(alarm_info_t *info)
{
    bool taken = false;
    portENTER_CRITICAL(&alarm_mux);
    if (due_pending) {
        if (info) *info = current_alarm;
        due_pending = false;
        taken = true;
    }
    portEXIT_CRITICAL(&alarm_mux);
    return taken;
}

/* Called frequently from the existing audio task; no extra FreeRTOS task needed. */
extern "C" bool alarm_poll_due_internal(void)
{
    const time_t now = time(NULL);
    bool fired = false;
    portENTER_CRITICAL(&alarm_mux);
    if (current_alarm.active && current_alarm.target_epoch > 0 &&
        now >= (time_t)current_alarm.target_epoch) {
        current_alarm.active = false;  // one-shot: mark consumed before triggering Gemini
        due_pending = true;
        fired = true;
    }
    portEXIT_CRITICAL(&alarm_mux);
    if (fired) {
        if (!persist_alarm()) ESP_LOGE(TAG, "Gagal menyimpan status alarm sudah terpicu");
        ESP_LOGW(TAG, "ALARM DUE: %02d:%02d WITA - %s",
                 current_alarm.hour, current_alarm.minute, current_alarm.label);
    }
    return fired;
}
