/* Laporan kesehatan robot ke DevKit (tanpa biaya, tanpa Gemini).
 * Format satu baris lewat UART:
 *   STAT|<versi>|<uptime_detik>|<rssi>|<kanal>|<ram_bebas>|<ram_min>|<psram_bebas>|<ws 0/1>|<putus_sejak_boot>
 *   EVT|<jenis>|<rssi>|<ram_bebas>|<ws 0/1>[|<keterangan>]
 * Keterangan: wifi_putus -> "r<kode alasan Wi-Fi>";
 *             gemini_putus -> "<sumber>:<errno>:<kode tutup>:<lama sesi detik>:<aktivitas>"
 */
#include "robot_stats.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "esp_heap_caps.h"
#include "uart_control.h"
#include "display.h"

static const char *TAG = "robot_stats";

static volatile bool s_ws_up = false;
static volatile unsigned s_drops = 0;
static volatile bool s_started = false;
static volatile int s_wifi_reason = 0;
static volatile int s_close_code = 0;
static volatile int64_t s_ws_up_since_us = 0;

static int current_rssi(int *channel)
{
    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        if (channel) *channel = ap.primary;
        return ap.rssi;
    }
    if (channel) *channel = 0;
    return 0;   /* 0 = tidak tersambung */
}

static void send_event(const char *kind, const char *detail)
{
    char line[160];
    int ch = 0;
    int rssi = current_rssi(&ch);
    snprintf(line, sizeof(line), "EVT|%s|%d|%u|%d|%s", kind, rssi,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), s_ws_up ? 1 : 0,
             detail ? detail : "");
    uart_control_send(line);
}

static const char *activity_name(void)
{
    switch (face_get_state()) {
        case FACE_SPEAKING: return "bicara";
        case FACE_THINKING: return "mikir";
        case FACE_SLEEP:    return "tidur";
        default:            return "lain";
    }
}

static void send_stat(void)
{
    char ver[24] = "?";
    const esp_app_desc_t *d = esp_app_get_description();
    if (d) {
        size_t j = 0;
        for (size_t i = 0; d->version[i] && j < sizeof(ver) - 1; ++i) {
            char c = d->version[i];
            if (c != '|' && c >= 0x20) ver[j++] = c;
        }
        ver[j] = '\0';
    }
    int ch = 0;
    int rssi = current_rssi(&ch);
    char line[200];
    snprintf(line, sizeof(line), "STAT|%s|%lu|%d|%d|%u|%u|%u|%d|%u", ver,
             (unsigned long)(esp_timer_get_time() / 1000000LL), rssi, ch,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             s_ws_up ? 1 : 0, s_drops);
    uart_control_send(line);
}

static const char *reset_name(esp_reset_reason_t r)
{
    switch (r) {
        case ESP_RST_POWERON:   return "boot_listrik";
        case ESP_RST_SW:        return "boot_restart";
        case ESP_RST_PANIC:     return "boot_crash";
        case ESP_RST_INT_WDT:   return "boot_wdt";
        case ESP_RST_TASK_WDT:  return "boot_wdt";
        case ESP_RST_WDT:       return "boot_wdt";
        case ESP_RST_BROWNOUT:  return "boot_brownout";
        default:                return "boot_lain";
    }
}

static volatile int s_wifi_drop_pending = 0;

static void wifi_event_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    /* Jalan di task event (stack kecil): hanya tandai. Task laporan yang mengirim. */
    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        if (data) s_wifi_reason = ((wifi_event_sta_disconnected_t *)data)->reason;
        s_wifi_drop_pending = s_wifi_drop_pending + 1;
    }
}

static void stats_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(8000));
    send_event(reset_name(esp_reset_reason()), "");
    s_wifi_drop_pending = 0;
    int ticks = 52;   /* laporan pertama beberapa detik setelah boot */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (s_wifi_drop_pending > 0) {
            s_wifi_drop_pending = 0;
            char det[16];
            snprintf(det, sizeof(det), "r%d", (int)s_wifi_reason);
            send_event("wifi_putus", det);
        }
        if (++ticks >= 60) {
            ticks = 0;
            send_stat();
        }
    }
}

void robot_stats_start(void)
{
    if (s_started) return;
    s_started = true;
    esp_err_t e = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_event_cb, NULL);
    if (e != ESP_OK) ESP_LOGW(TAG, "Pantau Wi-Fi gagal didaftarkan: %d", (int)e);
    if (xTaskCreate(stats_task, "robot_stats", 3072, NULL, 1, NULL) != pdPASS) {
        ESP_LOGW(TAG, "Task laporan tidak bisa dibuat");
        s_started = false;
    }
}

void robot_stats_ws_state(bool connected)
{
    if (connected && !s_ws_up) s_ws_up_since_us = esp_timer_get_time();
    s_ws_up = connected;
}

void robot_stats_note_close_code(int code)
{
    s_close_code = code;
}

void robot_stats_note_ws_drop(const char *src, int code)
{
    if (!s_started || !s_ws_up) return;   /* hitung sekali per putus (ERROR lalu DISCONNECTED tidak dobel) */
    s_ws_up = false;
    s_drops = s_drops + 1;
    long sesi = 0;
    if (s_ws_up_since_us > 0) sesi = (long)((esp_timer_get_time() - s_ws_up_since_us) / 1000000LL);
    char det[64];
    snprintf(det, sizeof(det), "%s:%d:%d:%ld:%s", (src && src[0]) ? src : "x", code,
             (int)s_close_code, sesi, activity_name());
    s_close_code = 0;
    send_event("gemini_putus", det);
}

void robot_stats_event(const char *kind)
{
    if (!kind || !kind[0]) return;
    char safe[24];
    size_t j = 0;
    for (size_t i = 0; kind[i] && j < sizeof(safe) - 1; ++i) {
        char c = kind[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') safe[j++] = c;
    }
    safe[j] = '\0';
    if (j) send_event(safe, "");
}
