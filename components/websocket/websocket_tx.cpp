#include "websocket_internal.h"
#include "web_config.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
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

bool websocket_send_alarm_prompt(int hour, int minute, const char *label, int kind)
{
    if (!label || !label[0] || hour < 0 || hour > 23 || minute < 0 || minute > 59) return false;
    if (!websocket_is_connected() || !websocket_tx_queue) return false;

    char safe[96];
    size_t j = 0;
    for (const unsigned char *p = (const unsigned char *)label; *p; ++p) {
        if (*p < 0x20 || *p == 0x7F) continue;
        const bool needs_escape = (*p == '"' || *p == '\\');
        if (j + (needs_escape ? 2U : 1U) >= sizeof(safe)) break;
        if (needs_escape) safe[j++] = '\\';
        safe[j++] = (char)*p;
    }
    safe[j] = '\0';
    char json[1536];
    int n;
    if (kind == 1) {
        n = snprintf(json, sizeof(json),
            "{\"clientContent\":{\"turns\":[{\"role\":\"user\",\"parts\":[{\"text\":"
            "\"[Pengingat otomatis] Sekarang pukul %02d:%02d WITA. Pengguna meminta diingatkan tentang: %s. "
            "Sampaikan pengingat itu sekarang dalam satu atau dua kalimat singkat, hangat, dan alami dalam bahasa Indonesia. "
            "Jangan mengucapkan selamat pagi atau membangunkan pengguna kecuali keterangan memintanya. "
            "Jangan membacakan instruksi dalam tanda kurung dan jangan bertanya. "
            "Jika perlu menyebut waktu, ucapkan jam dan menit secara alami dalam bahasa Indonesia, dengan pola "
            "pukul [jam dalam kata] lewat [menit dalam kata] menit WITA; jangan ucapkan angka seperti 24:40. "
            "Contoh jika untuk minum obat: Hei, ini pengingatmu, sekarang waktunya minum obat. Jaga kesehatan ya! "
            "Buat ucapan terasa alami dan tidak harus sama persis.\"}]}],\"turnComplete\":true}}",
            hour, minute, safe);
    } else {
        n = snprintf(json, sizeof(json),
            "{\"clientContent\":{\"turns\":[{\"role\":\"user\",\"parts\":[{\"text\":"
            "\"[Alarm otomatis] Sekarang pukul %02d:%02d WITA. Keterangan alarm: %s. "
            "Bangunkan pengguna sekarang. Ucapkan satu sapaan hangat dan alami dalam bahasa Indonesia, "
            "sesuai keterangan alarm. Jangan membacakan instruksi dalam tanda kurung dan jangan bertanya. "
            "Saat mengucapkan waktu, sebut jam dan menit secara alami dalam bahasa Indonesia, dengan pola "
            "pukul [jam dalam kata] lewat [menit dalam kata] menit WITA. Untuk 20:40, ucapkan "
            "pukul dua puluh lewat empat puluh menit WITA; jangan ucapkan dua puluh empat puluh atau 24:40. "
            "Contoh jika untuk membangunkan: Selamat pagi! Sekarang sudah pukul %02d:%02d. Yuk bangun, "
            "semangat menjalani aktivitas hari ini! Buat ucapan terasa alami dan tidak harus sama persis.\"}]}],\"turnComplete\":true}}",
            hour, minute, safe, hour, minute);
    }
    if (n <= 0 || (size_t)n >= sizeof(json)) return false;
    uint8_t *copy = (uint8_t *)malloc((size_t)n);
    if (!copy) return false;
    memcpy(copy, json, (size_t)n);
    ws_tx_command_t cmd = {};
    cmd.type = WS_TX_COMMAND_TEXT;
    cmd.generation = websocket_connection_generation;
    cmd.len = (uint16_t)n;
    cmd.data = copy;
    if (xQueueSend(websocket_tx_queue, &cmd, 0) != pdTRUE) {
        websocket_set_backpressured(true);
        free(copy);
        ESP_LOGW(TAG, "Alarm prompt ditahan: TX queue penuh");
        return false;
    }
    ESP_LOGI(TAG, "Alarm prompt masuk antrean TX: %02d:%02d WITA kind=%d (%s)", hour, minute, kind, safe);
    return true;
}

bool websocket_send_speaker_context(const char *name)
{
    if (!name || !name[0]) return false;
    if (!websocket_is_connected() || !websocket_tx_queue) {
        ESP_LOGI(TAG, "Konteks pembicara ditunda: sesi Gemini belum siap");
        return false;
    }

    /* Bersihkan nama agar aman dimasukkan ke JSON. */
    char safe[64];
    size_t j = 0;
    for (const unsigned char *p = (const unsigned char *)name; *p && j + 1 < sizeof(safe); ++p) {
        if (*p >= 0x20 && *p != 0x7F && *p != '"' && *p != '\\') safe[j++] = (char)*p;
    }
    safe[j] = '\0';
    if (j == 0) return false;

    char json[512];
    int n = snprintf(json, sizeof(json),
        "{\"clientContent\":{\"turns\":[{\"role\":\"user\",\"parts\":[{\"text\":"
        "\"[Sistem] Pengenal suara perangkat mengenali pengguna ini sebagai %s. "
        "Sapa %s dengan ramah memakai namanya, cukup satu kalimat singkat dalam bahasa Indonesia, "
        "lalu tunggu perintahnya.\"}]}],\"turnComplete\":true}}",
        safe, safe);
    if (n <= 0 || (size_t)n >= sizeof(json)) return false;

    uint8_t *copy = (uint8_t *)malloc((size_t)n);
    if (!copy) {
        ESP_LOGW(TAG, "Konteks pembicara ditahan: malloc gagal");
        return false;
    }
    memcpy(copy, json, (size_t)n);
    ws_tx_command_t cmd = {};
    cmd.type = WS_TX_COMMAND_TEXT;
    cmd.generation = websocket_connection_generation;
    cmd.len = (uint16_t)n;
    cmd.data = copy;
    if (xQueueSend(websocket_tx_queue, &cmd, 0) != pdTRUE) {
        websocket_set_backpressured(true);
        free(copy);
        ESP_LOGW(TAG, "Konteks pembicara ditahan: TX queue penuh");
        return false;
    }
    ESP_LOGI(TAG, "Konteks pembicara masuk antrean TX: %s", safe);
    return true;
}

bool websocket_send_speaker_update(const char *name)
{
    if (!name || !name[0]) return false;
    if (!websocket_is_connected() || !websocket_tx_queue) return false;

    char safe[64];
    size_t j = 0;
    for (const unsigned char *p = (const unsigned char *)name; *p && j + 1 < sizeof(safe); ++p) {
        if (*p >= 0x20 && *p != 0x7F && *p != '"' && *p != '\\') safe[j++] = (char)*p;
    }
    safe[j] = '\0';
    if (j == 0) return false;

    char json[512];
    int n = snprintf(json, sizeof(json),
        "{\"clientContent\":{\"turns\":[{\"role\":\"user\",\"parts\":[{\"text\":"
        "\"[Sistem, jangan dibalas] Pengenal suara perangkat mengenali bahwa yang sedang bicara sekarang adalah %s. "
        "Panggil dia %s bila perlu. Jangan mengucapkan apa pun untuk pesan sistem ini; "
        "lanjutkan percakapan seperti biasa.\"}]}],\"turnComplete\":true}}",
        safe, safe);
    if (n <= 0 || (size_t)n >= sizeof(json)) return false;

    uint8_t *copy = (uint8_t *)malloc((size_t)n);
    if (!copy) return false;
    memcpy(copy, json, (size_t)n);
    ws_tx_command_t cmd = {};
    cmd.type = WS_TX_COMMAND_TEXT;
    cmd.generation = websocket_connection_generation;
    cmd.len = (uint16_t)n;
    cmd.data = copy;
    if (xQueueSend(websocket_tx_queue, &cmd, 0) != pdTRUE) {
        websocket_set_backpressured(true);
        free(copy);
        ESP_LOGW(TAG, "Pembaruan pembicara ditahan: TX queue penuh");
        return false;
    }
    ESP_LOGI(TAG, "Pembaruan pembicara masuk antrean TX: %s", safe);
    return true;
}

bool websocket_send_radio_ended(const char *station, int reason)
{
    if (!websocket_is_connected() || !websocket_tx_queue) return false;

    char safe[64];
    size_t j = 0;
    for (const unsigned char *p = (const unsigned char *)(station ? station : ""); *p && j + 1 < sizeof(safe); ++p) {
        if (*p >= 0x20 && *p < 0x7F && *p != '"' && *p != '\\') safe[j++] = (char)*p;
    }
    safe[j] = '\0';
    if (j == 0) snprintf(safe, sizeof(safe), "radio");

    const char *what =
        (reason == 1) ? "berhenti sendiri karena gangguan jaringan atau stream. Sampaikan hal itu dengan singkat dan tawarkan memutarnya lagi atau stasiun lain."
      : (reason == 2) ? "gagal dimulai. Minta maaf singkat dan tawarkan stasiun lain."
      :                 "baru saja dimatikan pengguna dengan menyentuh sensor. Katakan singkat bahwa radionya sudah dimatikan, lalu tanyakan apa yang bisa dibantu.";

    char json[640];
    int n = snprintf(json, sizeof(json),
        "{\"clientContent\":{\"turns\":[{\"role\":\"user\",\"parts\":[{\"text\":"
        "\"[Sistem] Tadi perangkat memutar radio streaming %s dan radionya %s "
        "Jawab dalam bahasa Indonesia, cukup satu atau dua kalimat.\"}]}],\"turnComplete\":true}}",
        safe, what);
    if (n <= 0 || (size_t)n >= sizeof(json)) return false;

    uint8_t *copy = (uint8_t *)malloc((size_t)n);
    if (!copy) return false;
    memcpy(copy, json, (size_t)n);
    ws_tx_command_t cmd = {};
    cmd.type = WS_TX_COMMAND_TEXT;
    cmd.generation = websocket_connection_generation;
    cmd.len = (uint16_t)n;
    cmd.data = copy;
    if (xQueueSend(websocket_tx_queue, &cmd, 0) != pdTRUE) {
        websocket_set_backpressured(true);
        free(copy);
        ESP_LOGW(TAG, "Catatan radio ditahan: TX queue penuh");
        return false;
    }
    ESP_LOGI(TAG, "Catatan radio selesai masuk antrean TX: %s (alasan=%d)", safe, reason);
    return true;
}

/* Sudah lama tidak ada yang bicara: Gemini diminta pamit singkat (boleh bertanya sekilas), lalu robot kembali standby. */
bool websocket_send_idle_goodbye(void)
{
    if (!websocket_is_connected() || !websocket_tx_queue) return false;
    static const char json[] =
        "{\"clientContent\":{\"turns\":[{\"role\":\"user\",\"parts\":[{\"text\":"
        "\"[Sistem] Sudah sekitar 30 detik tidak ada yang berbicara kepadamu. "
        "Ucapkan satu kalimat pendek yang hangat: boleh bertanya sekilas apakah masih ada yang dibutuhkan, "
        "lalu pamit bahwa kamu istirahat dulu dan bisa dipanggil lagi dengan Hi ESP. "
        "Jangan menjalankan tool apa pun. Bahasa Indonesia.\"}]}],\"turnComplete\":true}}";
    const size_t n = sizeof(json) - 1;
    uint8_t *copy = (uint8_t *)malloc(n);
    if (!copy) return false;
    memcpy(copy, json, n);
    ws_tx_command_t cmd = {};
    cmd.type = WS_TX_COMMAND_TEXT;
    cmd.generation = websocket_connection_generation;
    cmd.len = (uint16_t)n;
    cmd.data = copy;
    if (xQueueSend(websocket_tx_queue, &cmd, 0) != pdTRUE) {
        websocket_set_backpressured(true);
        free(copy);
        ESP_LOGW(TAG, "Pamit sepi ditahan: TX queue penuh");
        return false;
    }
    ESP_LOGI(TAG, "Pamit karena sepi 30 detik masuk antrean TX");
    return true;
}
