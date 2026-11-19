// Tool Gemini "putar_radio": cari stasiun di task sendiri (butuh internet), lalu jawab Gemini.
// Radio baru dinyalakan oleh main.cpp setelah Gemini selesai mengucapkan konfirmasi.
#include "websocket_internal.h"
#include "radio_player.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "WS_RADIO";

#define RADIO_TASK_STACK 12288

typedef struct {
    char id[96];
    char query[96];
} radio_job_t;

static void radio_lookup_task(void *arg)
{
    radio_job_t *job = (radio_job_t *)arg;
    radio_station_t st = {};
    const bool ok = radio_resolve(job->query, &st);

    char result[400];
    if (ok) {
        radio_set_pending(&st);
        snprintf(result, sizeof(result),
                 "Radio '%s' siap diputar. Ucapkan SATU kalimat singkat bahwa kamu memutar radio %s sekarang "
                 "dan pengguna bisa menyentuh sensor sentuh untuk menghentikannya, lalu berhenti bicara. "
                 "Radio menyala otomatis begitu kamu selesai bicara.",
                 st.name, st.name);
    } else {
        snprintf(result, sizeof(result),
                 "Gagal: stasiun radio '%s' tidak ditemukan atau internet bermasalah. "
                 "Minta maaf singkat dan tawarkan nama stasiun lain, misalnya radio Bali.",
                 job->query);
    }
    if (!websocket_send_tool_response_text(job->id, "putar_radio", result)) {
        ESP_LOGW(TAG, "Hasil radio tidak terkirim ke Gemini (id=%s); batalkan radio", job->id);
        if (ok) radio_clear_pending();
        websocket_set_mic_uplink_paused(false);
    } else {
        ESP_LOGI(TAG, "Hasil radio dikirim ke Gemini (ok=%d); mic tetap ditahan sampai balasan selesai", ok ? 1 : 0);
    }
    free(job);
    vTaskDelete(NULL);
}

bool websocket_radio_start(const char *id, const char *query)
{
    if (!id || strlen(id) >= sizeof(((radio_job_t *)0)->id)) return false;
    radio_job_t *job = (radio_job_t *)calloc(1, sizeof(radio_job_t));
    if (!job) return false;
    snprintf(job->id, sizeof(job->id), "%s", id);
    snprintf(job->query, sizeof(job->query), "%s", query ? query : "");
    if (xTaskCreate(radio_lookup_task, "radio_find", RADIO_TASK_STACK, job, 2, NULL) != pdPASS) {
        free(job);
        ESP_LOGW(TAG, "Tidak bisa membuat task pencarian radio");
        return false;
    }
    ESP_LOGI(TAG, "Pencarian radio dijadwalkan: '%s'", query ? query : "");
    return true;
}
