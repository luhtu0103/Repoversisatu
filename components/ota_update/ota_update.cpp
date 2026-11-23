// OTA dari GitHub Release.
//   version.txt  -> berisi nomor build (angka), mis. "57"
//   firmware.bin -> program ESP32-S3
// Keduanya diunggah otomatis oleh GitHub Actions ke Release terbaru.
#include "ota_update.h"
#include "display.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "OTA";

#define OTA_BASE_URL     "https://github.com/luhtu0103/Repoversisatu/releases/latest/download/"
#define OTA_VERSION_URL  OTA_BASE_URL "version.txt"
#define OTA_FIRMWARE_URL OTA_BASE_URL "firmware.bin"

static volatile bool s_busy = false;

typedef struct {
    bool install;
    ota_result_cb_t cb;
    void *ctx;
} ota_job_t;

typedef struct {
    char buf[48];
    size_t len;
} ver_buf_t;

void ota_update_mark_valid(void)
{
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "Program ditandai sehat (hasil=%s)", esp_err_to_name(err));
}

static void current_version(char *out, size_t n)
{
    snprintf(out, n, "0");
    const esp_partition_t *part = esp_ota_get_running_partition();
    esp_app_desc_t desc;
    if (part && esp_ota_get_partition_description(part, &desc) == ESP_OK) {
        snprintf(out, n, "%.31s", desc.version);
    }
}

// Ambil angka pertama dalam teks ("57", "v57", "build-57" -> 57). Tidak ada angka -> 0.
static long parse_number(const char *s)
{
    if (!s) return 0;
    while (*s && (*s < '0' || *s > '9')) s++;
    return *s ? strtol(s, NULL, 10) : 0;
}

static esp_err_t version_http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data &&
        esp_http_client_get_status_code(evt->client) == 200) {
        ver_buf_t *b = (ver_buf_t *)evt->user_data;
        size_t room = sizeof(b->buf) - 1 - b->len;
        size_t n = (size_t)evt->data_len < room ? (size_t)evt->data_len : room;
        if (n > 0) {
            memcpy(b->buf + b->len, evt->data, n);
            b->len += n;
            b->buf[b->len] = '\0';
        }
    }
    return ESP_OK;
}

static bool fetch_remote_version(char *out, size_t out_size)
{
    ver_buf_t b = {};
    esp_http_client_config_t cfg = {};
    cfg.url = OTA_VERSION_URL;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 15000;
    cfg.buffer_size = 4096;      // alamat redirect GitHub panjang
    cfg.buffer_size_tx = 1024;
    cfg.event_handler = version_http_event;
    cfg.user_data = &b;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || b.len == 0) {
        ESP_LOGW(TAG, "Gagal ambil version.txt: err=%s http=%d", esp_err_to_name(err), status);
        return false;
    }
    // Buang spasi/enter di ujung.
    while (b.len > 0 && (b.buf[b.len - 1] == '\n' || b.buf[b.len - 1] == '\r' || b.buf[b.len - 1] == ' ')) {
        b.buf[--b.len] = '\0';
    }
    snprintf(out, out_size, "%.31s", b.buf);
    return out[0] != '\0';
}

static void install_firmware(void)
{
    esp_http_client_config_t cfg = {};
    cfg.url = OTA_FIRMWARE_URL;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 20000;
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 1024;
    cfg.keep_alive_enable = true;

    esp_https_ota_config_t ota_cfg = {};
    ota_cfg.http_config = &cfg;

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin gagal: %s", esp_err_to_name(err));
        display_status("Update gagal");
        return;
    }

    int last_pct = -10;
    while (true) {
        err = esp_https_ota_perform(handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;
        int total = esp_https_ota_get_image_size(handle);
        int got = esp_https_ota_get_image_len_read(handle);
        if (total > 0) {
            int pct = (int)((int64_t)got * 100 / total);
            if (pct >= last_pct + 10) {
                last_pct = pct;
                char line[32];
                snprintf(line, sizeof(line), "Update %d%%", pct);
                display_status(line);
                ESP_LOGI(TAG, "Unduh %d%% (%d/%d byte)", pct, got, total);
            }
        }
    }

    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(handle)) {
        ESP_LOGE(TAG, "Unduhan tidak lengkap: %s", esp_err_to_name(err));
        esp_https_ota_abort(handle);
        display_status("Update gagal");
        return;
    }

    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA finish gagal (program rusak?): %s", esp_err_to_name(err));
        display_status("Update gagal");
        return;
    }

    ESP_LOGI(TAG, "Update berhasil dipasang. Restart...");
    display_status("Update selesai");
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static void ota_task(void *arg)
{
    ota_job_t job = *(ota_job_t *)arg;
    free(arg);

    char cur[32];
    char remote[32] = {};
    char msg[220];
    bool newer = false;
    current_version(cur, sizeof(cur));

    if (!fetch_remote_version(remote, sizeof(remote))) {
        snprintf(msg, sizeof(msg),
                 "Gagal memeriksa pembaruan. Internet bermasalah atau belum ada versi baru di GitHub.");
    } else {
        newer = parse_number(remote) > parse_number(cur);
        if (!newer) {
            snprintf(msg, sizeof(msg), "Perangkat lunak sudah yang terbaru, versi %s.", cur);
        } else if (!job.install) {
            snprintf(msg, sizeof(msg),
                     "Ada versi baru %s, sekarang versi %s. Jika pengguna mau, minta dia mengatakan pasang update.",
                     remote, cur);
        } else {
            snprintf(msg, sizeof(msg),
                     "Mulai mengunduh versi %s (sekarang %s). Robot akan restart sendiri dalam beberapa menit. "
                     "Ucapkan satu kalimat singkat lalu berhenti bicara.",
                     remote, cur);
        }
    }
    ESP_LOGI(TAG, "Versi sekarang=%s, di GitHub=%s -> %s", cur, remote[0] ? remote : "?", msg);

    if (job.cb) job.cb(job.ctx, msg);
    if (newer && job.install) install_firmware();

    s_busy = false;
    vTaskDelete(NULL);
}

bool ota_update_run_async(bool install, ota_result_cb_t cb, void *ctx)
{
    if (s_busy) return false;
    ota_job_t *job = (ota_job_t *)malloc(sizeof(ota_job_t));
    if (!job) return false;
    job->install = install;
    job->cb = cb;
    job->ctx = ctx;
    s_busy = true;
    // Stack di RAM internal: tulis flash tidak boleh memakai stack PSRAM.
    if (xTaskCreate(ota_task, "ota", 8192, job, 4, NULL) != pdPASS) {
        s_busy = false;
        free(job);
        return false;
    }
    return true;
}
