#include "display.h"
#include "wifi_manager.h"
#include "websocket_mgr.h"
#include "audio_hal.h"
#include "websocket_internal.h"
#include "uart_control.h"
#include "ai_memory.h"
#include "web_config.h"
#include "wakeword.h"
#include "touch_emotion.h"
#include "servo_head.h"
#include "led_rgb.h"
#include "speaker_id.h"
#include "alarm.h"
#include "radio_player.h"
#include "ota_update.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "nvs_flash.h"
#include "esp_sntp.h"
#include "robot_stats.h"

#include <sys/time.h>
#include <time.h>

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

constexpr uint32_t AUDIO_VOICE_ENERGY_THRESHOLD_SQ = 360000;  // 600^2; lower gate for quiet speech
constexpr time_t NTP_FALLBACK_TIMESTAMP = 1770000000;  // 2026-02-02 UTC

static const char *TAG = "MAIN";
#define BOOT_BUTTON_GPIO GPIO_NUM_0
static bool resolve_host(const char *label, const char *host, const char *port, char *resolved_ip, size_t resolved_ip_len)
{
    ESP_LOGI(TAG, "DNS [%s]: getaddrinfo(%s:%s)", label, host, port);
    struct addrinfo hints = {};
    struct addrinfo *result = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int saved_errno = 0;
    int64_t start_us = esp_timer_get_time();
    int err = getaddrinfo(host, port, &hints, &result);
    int64_t elapsed_us = esp_timer_get_time() - start_us;
    saved_errno = errno;
    ESP_LOGI(TAG, "DNS [%s]: err=%d errno=%d elapsed=%lld ms", label, err, saved_errno, (long long)(elapsed_us / 1000));
    if (err != 0 || result == nullptr) {
        ESP_LOGE(TAG, "DNS [%s]: FAILED", label);
        return false;
    }

    bool found_ipv4 = false;
    if (resolved_ip && resolved_ip_len > 0) resolved_ip[0] = '\0';
    for (struct addrinfo *p = result; p != nullptr; p = p->ai_next) {
        if (p->ai_family != AF_INET || !p->ai_addr) continue;
        struct sockaddr_in *addr = (struct sockaddr_in *)p->ai_addr;
        char ip[INET_ADDRSTRLEN] = {};
        if (inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip)) != nullptr) {
            ESP_LOGI(TAG, "DNS [%s]: IPv4=%s", label, ip);
            if (!found_ipv4 && resolved_ip && resolved_ip_len > 0) {
                strlcpy(resolved_ip, ip, resolved_ip_len);
            }
        }
        found_ipv4 = true;
    }
    freeaddrinfo(result);
    if (!found_ipv4) {
        ESP_LOGE(TAG, "DNS [%s]: OK tetapi tidak ada IPv4", label);
        return false;
    }
    ESP_LOGI(TAG, "DNS [%s]: RESULT=OK", label);
    return true;
}

static bool debug_dns_server(const char *label, const char *dns_ip)
{
    if (!dns_ip || dns_ip[0] == '\0' || strcmp(dns_ip, "0.0.0.0") == 0) {
        ESP_LOGW(TAG, "DNS SERVER [%s]: tidak dikonfigurasi", label);
        return false;
    }
    ESP_LOGI(TAG, "DNS SERVER [%s]: %s", label, dns_ip);
    return resolve_host(label, dns_ip, "53", nullptr, 0);
}

static bool debug_dns_resolution(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "DEBUG NETWORK START");

    char google_ip[INET_ADDRSTRLEN] = {};
    char gemini_ip[INET_ADDRSTRLEN] = {};

    bool google_ok = resolve_host("google.com", "google.com", "443", google_ip, sizeof(google_ip));
    bool gemini_ok = resolve_host("Gemini", "generativelanguage.googleapis.com", "443", gemini_ip, sizeof(gemini_ip));

    ESP_LOGI(TAG, "DNS SUMMARY: google.com=%s Gemini=%s", google_ok ? "OK" : "FAILED", gemini_ok ? "OK" : "FAILED");
    if (!google_ok || !gemini_ok) {
        ESP_LOGI(TAG, "========================================");
        return false;
    }
    ESP_LOGI(TAG, "DNS RESULT: OK");
    ESP_LOGI(TAG, "Gemini resolved IP: %s", gemini_ip);
    ESP_LOGI(TAG, "========================================");
    return true;
}

static bool debug_tcp_connection(void)
{
    char gemini_ip[INET6_ADDRSTRLEN] = {};
    if (!resolve_host("Gemini-TCP", "generativelanguage.googleapis.com", "443", gemini_ip, sizeof(gemini_ip))) {
        ESP_LOGE(TAG, "TCP test dihentikan: DNS Gemini gagal");
        return false;
    }

    ESP_LOGI(TAG, "TCP test: generativelanguage.googleapis.com:443 -> %s:443", gemini_ip);
    struct sockaddr_in target = {};
    target.sin_family = AF_INET;
    target.sin_port = htons(443);
    if (inet_pton(AF_INET, gemini_ip, &target.sin_addr) != 1) {
        ESP_LOGE(TAG, "TCP target IP tidak valid: %s", gemini_ip);
        return false;
    }

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "TCP socket() FAILED errno=%d", errno);
        return false;
    }
    struct timeval timeout = {};
    timeout.tv_sec = 5;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    ESP_LOGI(TAG, "TCP connect() ke %s:443...", gemini_ip);
    int64_t start_us = esp_timer_get_time();
    int ret = connect(sock, (struct sockaddr *)&target, sizeof(target));
    int saved_errno = errno;
    int64_t elapsed_us = esp_timer_get_time() - start_us;
    if (ret == 0) {
        ESP_LOGI(TAG, "TCP CONNECT OK elapsed=%lld ms", (long long)(elapsed_us / 1000));
        close(sock);
        ESP_LOGI(TAG, "TCP RESULT: OK");
        return true;
    }
    ESP_LOGE(TAG, "TCP CONNECT FAILED errno=%d elapsed=%lld ms", saved_errno, (long long)(elapsed_us / 1000));
    close(sock);
    ESP_LOGE(TAG, "TCP RESULT: GAGAL");
    return false;
}

static void debug_network_path(void)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "NETWORK DIAGNOSTIC");
    ESP_LOGI(TAG, "Target: generativelanguage.googleapis.com:443");
    ESP_LOGI(TAG, "========================================");

    ESP_LOGI(TAG, "STEP 1: Network interface state sudah dilog oleh WIFI_MGR saat GOT_IP");
    ESP_LOGI(TAG, "STEP 2: DNS server reachability akan diuji melalui resolver");
    ESP_LOGI(TAG, "STEP 3: DNS google.com");
    ESP_LOGI(TAG, "STEP 4: DNS generativelanguage.googleapis.com");
    ESP_LOGI(TAG, "STEP 5: TCP 443 ke IP Gemini hasil DNS");

    if (!debug_dns_resolution()) {
        ESP_LOGE(TAG, "NETWORK STOP: DNS");
        ESP_LOGI(TAG, "========================================");
        return;
    }
    if (!debug_tcp_connection()) {
        ESP_LOGE(TAG, "NETWORK STOP: TCP");
        ESP_LOGI(TAG, "DNS = OK");
        ESP_LOGI(TAG, "TCP = FAILED");
        ESP_LOGI(TAG, "TLS = BELUM DITES");
        ESP_LOGI(TAG, "========================================");
        return;
    }
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "NETWORK BASIC TEST = OK");
    ESP_LOGI(TAG, "DNS = OK");
    ESP_LOGI(TAG, "TCP 443 = OK");
    ESP_LOGI(TAG, "NEXT = WebSocket/TLS");
    ESP_LOGI(TAG, "========================================");
}

static bool sync_sntp_time(void)
{
    setenv("TZ", "WITA-8", 1);  // WITA = UTC+8, tanpa DST
    tzset();
    ESP_LOGI(TAG, "Mencari server NTP..."); display_status("Sync Jam Network..");
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL); esp_sntp_setservername(0, "time.google.com"); esp_sntp_setservername(1, "id.pool.ntp.org"); esp_sntp_setservername(2, "pool.ntp.org"); esp_sntp_init();
    int retry = 0; const int max_retries = 10; time_t now = 0; struct tm timeinfo = {};
    while (retry < max_retries) { time(&now); localtime_r(&now, &timeinfo); if (timeinfo.tm_year >= (2024 - 1900)) { ESP_LOGI(TAG, "Waktu cocok! Tahun: %d", timeinfo.tm_year + 1900); display_status("Jam Cocok!"); vTaskDelay(pdMS_TO_TICKS(1000)); return true; } vTaskDelay(pdMS_TO_TICKS(500)); retry++; }
    ESP_LOGW(TAG, "NTP gagal. Menggunakan waktu fallback."); struct timeval tv = { .tv_sec = NTP_FALLBACK_TIMESTAMP, .tv_usec = 0 }; settimeofday(&tv, NULL); display_status("Jam Set Fallback"); vTaskDelay(pdMS_TO_TICKS(1000)); return false;
}

static volatile bool assistant_active = false;
static volatile bool led_boot_phase = true;
static volatile bool led_wifi_config_mode = false;
static volatile bool mic_voice_detected = false;
static int64_t disconnect_wait_start_us = 0;
static bool alarm_prompt_pending = false;
static alarm_info_t alarm_prompt_info = {};
static bool alarm_prompt_standby_after = true;   // kembali standby setelah alarm bicara (kecuali sedang ngobrol)

/* Setelah radio streaming selesai, Gemini diberi tahu bahwa tadi baru memutar radio. */
static char radio_note_name[48] = {};
static int radio_note_reason = 0;
static bool radio_note_pending = false;

/* Ring 1,5 detik audio mentah untuk Speaker ID. MFCC+DTW harus berjalan
 * di worker terpisah agar audio_task tetap menguras antrean mikrofon. */
constexpr size_t WAKE_RING_SAMPLES = 24000;
static char speaker_pending_name[SPEAKER_ID_NAME_MAX + 1] = {};
static bool speaker_context_pending = false;
static portMUX_TYPE speaker_context_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t speaker_session_seq = 0;  // naik tiap sesi berakhir
static char speaker_current_name[SPEAKER_ID_NAME_MAX + 1] = {};  // nama yang sudah diberitahukan ke Gemini
static char speaker_change_name[SPEAKER_ID_NAME_MAX + 1] = {};
static char speaker_candidate_name[SPEAKER_ID_NAME_MAX + 1] = {};  // calon pembicara baru (perlu 2x cocok berturut-turut)
static int speaker_candidate_count = 0;
static bool speaker_change_pending = false;

typedef struct {
    size_t samples;
    int16_t *pcm;
    uint32_t seq;
    uint8_t kind;   // 0 = Hi ESP (MFCC+DTW), 1 = warna suara saat ngobrol
} speaker_id_job_t;

static QueueHandle_t speaker_id_queue = nullptr;

static void speaker_id_worker_task(void *arg)
{
    (void)arg;
    for (;;) {
        speaker_id_job_t *job = nullptr;
        if (xQueueReceive(speaker_id_queue, &job, portMAX_DELAY) != pdTRUE || !job) continue;

        if (job->kind == 1) {
            speaker_vp_result_t vr = {};
            const speaker_id_status_t vst = speaker_vp_identify(job->pcm, job->samples, &vr);
            if (vst == SPEAKER_ID_OK && vr.known) {
                bool changed = false;
                portENTER_CRITICAL(&speaker_context_mux);
                if (job->seq == speaker_session_seq) {
                    if (strcasecmp(vr.name, speaker_current_name) == 0) {
                        speaker_candidate_count = 0;          // sama dengan yang sudah diberitahukan
                        speaker_candidate_name[0] = '\0';
                    } else {
                        if (strcasecmp(vr.name, speaker_candidate_name) == 0) {
                            ++speaker_candidate_count;
                        } else {
                            strlcpy(speaker_candidate_name, vr.name, sizeof(speaker_candidate_name));
                            speaker_candidate_count = 1;
                        }
                        if (speaker_candidate_count >= 2) {   // 2 ucapan berturut-turut -> yakin
                            strlcpy(speaker_change_name, vr.name, sizeof(speaker_change_name));
                            speaker_change_pending = true;
                            speaker_candidate_count = 0;
                            speaker_candidate_name[0] = '\0';
                            changed = true;
                        }
                    }
                }
                portEXIT_CRITICAL(&speaker_context_mux);
                if (changed) ESP_LOGI(TAG, "Warna suara mengenali pembicara: %s", vr.name);
            } else if (vst != SPEAKER_ID_OK) {
                ESP_LOGD(TAG, "Warna suara dilewati: %s", speaker_id_status_text(vst));
            }
            heap_caps_free(job->pcm);
            heap_caps_free(job);
            continue;
        }

        speaker_id_result_t result = {};
        const speaker_id_status_t st = speaker_id_identify(job->pcm, job->samples, &result);
        if (st == SPEAKER_ID_OK && result.known) {
            bool accepted = false;
            portENTER_CRITICAL(&speaker_context_mux);
            if (job->seq == speaker_session_seq) {
                strlcpy(speaker_pending_name, result.name, sizeof(speaker_pending_name));
                speaker_context_pending = true;
                accepted = true;
            }
            portEXIT_CRITICAL(&speaker_context_mux);
            if (accepted) ESP_LOGI(TAG, "Pembicara dikenal: %s", result.name);
            else ESP_LOGW(TAG, "Hasil Speaker ID dibuang (sesi sudah berakhir): %s", result.name);
        } else if (st == SPEAKER_ID_OK) {
            ESP_LOGI(TAG, "Pembicara tidak dikenal; Gemini tidak diberi nama (tidak disapa)");
        } else {
            ESP_LOGI(TAG, "Pengenal suara dilewati: %s", speaker_id_status_text(st));
        }

        heap_caps_free(job->pcm);
        heap_caps_free(job);
    }
}

/* Salin snapshot ring secara singkat; pengenalan berat dilakukan worker. */
static void queue_speaker_identification(const int16_t *ring, size_t ring_pos, size_t ring_filled)
{
    if (!ring || !speaker_id_has_profiles() || ring_filled < 8000 || !speaker_id_queue) return;

    speaker_id_job_t *job = (speaker_id_job_t *)heap_caps_malloc(
        sizeof(speaker_id_job_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!job) {
        ESP_LOGW(TAG, "Speaker ID dilewati: alokasi job gagal");
        return;
    }
    job->pcm = (int16_t *)heap_caps_malloc(
        ring_filled * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!job->pcm) {
        heap_caps_free(job);
        ESP_LOGW(TAG, "Speaker ID dilewati: alokasi snapshot PSRAM gagal");
        return;
    }

    const size_t start = (ring_filled < WAKE_RING_SAMPLES) ? 0 : ring_pos;
    for (size_t i = 0; i < ring_filled; ++i)
        job->pcm[i] = ring[(start + i) % WAKE_RING_SAMPLES];
    job->samples = ring_filled;
    job->seq = speaker_session_seq;
    job->kind = 0;

    if (xQueueSend(speaker_id_queue, &job, 0) != pdTRUE) {
        heap_caps_free(job->pcm);
        heap_caps_free(job);
        ESP_LOGW(TAG, "Speaker ID dilewati: worker masih sibuk");
    } else {
        ESP_LOGI(TAG, "Snapshot Speaker ID dikirim ke worker: %u sampel",
                 (unsigned)ring_filled);
    }
}

/* ---- Warna suara: potong ucapan pengguna saat ngobrol, kirim ke worker ---- */
constexpr size_t UTT_RING_SAMPLES = 48000;        // 3 detik
constexpr size_t UTT_END_SILENCE_SAMPLES = 6400;  // 0,4 detik diam = ucapan selesai
constexpr size_t UTT_MIN_VOICED_SAMPLES = 6400;   // minimal 0,4 detik suara
static int16_t *utt_ring = nullptr;
static size_t utt_pos = 0;
static uint64_t utt_total = 0;
static uint64_t utt_begin = 0;
static bool utt_active = false;
static size_t utt_voiced = 0;
static size_t utt_silence = 0;

static void utt_reset(void)
{
    utt_pos = 0;
    utt_total = 0;
    utt_active = false;
    utt_voiced = 0;
    utt_silence = 0;
}

static void queue_word_identification(size_t len)
{
    if (!utt_ring || !speaker_id_queue || len < 6400) return;
    speaker_id_job_t *job = (speaker_id_job_t *)heap_caps_malloc(
        sizeof(speaker_id_job_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!job) return;
    job->pcm = (int16_t *)heap_caps_malloc(len * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!job->pcm) { heap_caps_free(job); return; }
    const size_t start = (utt_pos + UTT_RING_SAMPLES - len) % UTT_RING_SAMPLES;
    for (size_t i = 0; i < len; ++i) job->pcm[i] = utt_ring[(start + i) % UTT_RING_SAMPLES];
    job->samples = len;
    job->seq = speaker_session_seq;
    job->kind = 1;
    if (xQueueSend(speaker_id_queue, &job, 0) != pdTRUE) {
        heap_caps_free(job->pcm);
        heap_caps_free(job);   // worker masih sibuk: lewati ucapan ini
    }
}

static void utterance_feed(const int16_t *s, size_t n, bool voice)
{
    if (!speaker_vp_has_profiles() || !speaker_id_queue || n == 0) return;
    if (!utt_ring) {
        utt_ring = (int16_t *)heap_caps_calloc(UTT_RING_SAMPLES, sizeof(int16_t),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!utt_ring) return;
    }
    for (size_t i = 0; i < n; ++i) utt_ring[(utt_pos + i) % UTT_RING_SAMPLES] = s[i];
    utt_pos = (utt_pos + n) % UTT_RING_SAMPLES;
    utt_total += n;

    if (voice) {
        if (!utt_active) {
            utt_active = true;
            utt_begin = utt_total - n;
            utt_voiced = 0;
        }
        utt_voiced += n;
        utt_silence = 0;
    } else if (utt_active) {
        utt_silence += n;
    }
    if (!utt_active) return;

    const uint64_t length = utt_total - utt_begin;
    if (utt_silence >= UTT_END_SILENCE_SAMPLES || length >= UTT_RING_SAMPLES) {
        if (utt_voiced >= UTT_MIN_VOICED_SAMPLES) {
            size_t len = (size_t)length + 3200;               // sedikit awalan
            if (len > UTT_RING_SAMPLES) len = UTT_RING_SAMPLES;
            if (len > utt_total) len = (size_t)utt_total;
            queue_word_identification(len);
        }
        utt_active = false;
        utt_voiced = 0;
        utt_silence = 0;
    }
}

/* Tombol BOOT (GPIO0) ditekan 3x cepat: buka / tutup mode konfigurasi web.
 * Dipanggil tiap ~10 ms. Mengembalikan true sekali saat tekan ke-3 selesai.
 * Tiap tekan harus 30 ms - 1 detik; jeda antar tekan maksimal 1,5 detik. */
static bool boot_triple_press_poll(void)
{
    static bool was_down = false;
    static int64_t down_us = 0;
    static int64_t last_release_us = 0;
    static int count = 0;
    const int64_t now = esp_timer_get_time();
    const bool down = gpio_get_level(BOOT_BUTTON_GPIO) == 0;
    if (down && !was_down) {
        was_down = true;
        down_us = now;
    } else if (!down && was_down) {
        was_down = false;
        const int64_t held = now - down_us;
        if (held >= 30000 && held <= 1000000) {
            if (count > 0 && now - last_release_us > 1500000) count = 0;
            count++;
            last_release_us = now;
            ESP_LOGI(TAG, "Tombol BOOT: tekan ke-%d dari 3", count);
            if (count >= 3) {
                count = 0;
                return true;
            }
        } else {
            count = 0;   // ditahan terlalu lama / terlalu singkat
        }
    }
    if (!down && count > 0 && now - last_release_us > 1500000) count = 0;
    return false;
}

static void boot_button_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (boot_triple_press_poll()) {
            ESP_LOGW(TAG, "Tombol BOOT 3x -> masuk Config Mode (web config)");
            display_status("Masuk Config Mode");
            vTaskDelay(pdMS_TO_TICKS(800));
            web_config_force_reset();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void led_status_task(void *arg)
{
    (void)arg;
    bool blink_on = false;
    int64_t last_blink_us = 0;
    for (;;) {
        const int64_t now_us = esp_timer_get_time();
        led_rgb_state_t state = LED_RGB_OFF;

        if (led_wifi_config_mode || led_boot_phase) {
            const int64_t interval_us = led_wifi_config_mode ? 500000LL : 100000LL;
            if (now_us - last_blink_us >= interval_us) {
                blink_on = !blink_on;
                last_blink_us = now_us;
            }
            state = blink_on
                ? (led_wifi_config_mode ? LED_RGB_WIFI_CONFIG : LED_RGB_BOOT_BLINK)
                : LED_RGB_OFF;
        } else if (assistant_active) {
            if (!websocket_is_connected()) {
                state = LED_RGB_CONNECTING;
            } else if (face_get_state() == FACE_SPEAKING) {
                state = LED_RGB_SPEAKING;
            } else if (mic_voice_detected) {
                state = LED_RGB_VOICE_DETECTED;
            } else {
                state = LED_RGB_LISTENING;
            }
        }
        led_rgb_set_state(state);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void audio_task(void *arg)
{
    (void)arg;
    /* Penyangga suara mic: ~6 detik PCM 16 kHz. Setelah "Hi ESP" sambungan ke Gemini butuh ~3 detik;
     * selama itu ucapan pengguna disimpan di sini lalu dikirim begitu sesi siap (tidak hilang). */
    constexpr size_t AUDIO_BUFFER_SIZE = 196608;
    uint8_t *audio_buffer = (uint8_t *)heap_caps_malloc(
        AUDIO_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const char *audio_buffer_location = "PSRAM";
    if (audio_buffer == nullptr) {
        audio_buffer = (uint8_t *)heap_caps_malloc(
            AUDIO_BUFFER_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        audio_buffer_location = "internal RAM fallback";
    }
    if (audio_buffer == nullptr) {
        ESP_LOGE(TAG, "Gagal alokasi buffer mikrofon %u byte; audio_task dihentikan",
                 (unsigned)AUDIO_BUFFER_SIZE);
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(TAG, "Microphone staging buffer: %u bytes in %s; free_internal=%u largest_internal=%u free_psram=%u largest_psram=%u",
             (unsigned)AUDIO_BUFFER_SIZE, audio_buffer_location,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    /* Worker MFCC+DTW terpisah supaya audio_task tidak tertahan saat identifikasi. */
    if (speaker_id_is_ready()) {
        speaker_id_queue = xQueueCreate(1, sizeof(speaker_id_job_t *));
        if (speaker_id_queue) {
            BaseType_t worker_result = xTaskCreatePinnedToCore(
                speaker_id_worker_task, "speaker_id", 6144, nullptr, 2, nullptr, 1);
            if (worker_result != pdPASS) {
                vQueueDelete(speaker_id_queue);
                speaker_id_queue = nullptr;
                ESP_LOGW(TAG, "Speaker ID worker gagal dibuat; identifikasi nonaktif");
            }
        } else {
            ESP_LOGW(TAG, "Speaker ID queue gagal dibuat; identifikasi nonaktif");
        }
    }

    /* Ring buffer 1,5 detik audio mentah untuk snapshot Speaker ID. */
    int16_t *wake_ring = (int16_t *)heap_caps_calloc(
        WAKE_RING_SAMPLES, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!wake_ring) {
        ESP_LOGW(TAG, "Ring buffer pengenal suara gagal dialokasikan; sapaan nama dimatikan");
    }
    size_t wake_ring_pos = 0;
    size_t wake_ring_filled = 0;
    size_t buffer_pos = 0;
    size_t buffer_high_water = 0;
    uint64_t mic_buffer_lost_bytes = 0;
    uint32_t consecutive_tx_enqueue_failures = 0;
    int64_t tx_backpressure_start_us = 0;
    constexpr int64_t TX_BACKPRESSURE_ABORT_US = 4000000LL;   // 4 detik (sebelumnya 2): cukup untuk melepas tumpukan awal sesi
    int64_t last_tx_diag_us = 0;
    int64_t last_activity_debug_us = 0;
    static int last_wake_result = 0;
    static int detect_calls = 0;

    bool standby_pending = false;
    int64_t standby_requested_us = 0;
    /* Alarm voice responses can be longer than a normal acknowledgement. Do not
     * disconnect while Gemini is still generating/playing the wake-up message. */
    constexpr int64_t STANDBY_PLAYBACK_DRAIN_TIMEOUT_US = 60000000LL;

    while (1) {
        /* Satu alarm per kali: jangan memicu alarm berikutnya sebelum prompt sebelumnya terkirim. */
        if (!alarm_prompt_pending && alarm_poll_due_internal()) {
            alarm_info_t fired = {};
            if (alarm_take_due(&fired)) {
                if (radio_is_active()) radio_stop();   // alarm lebih penting daripada radio
                alarm_prompt_info = fired;
                alarm_prompt_standby_after = !assistant_active;   // sedang ngobrol: jangan putuskan sesi setelah pengingat
                alarm_prompt_pending = true;
                ESP_LOGW(TAG, "ALARM #%d (%s): waktu %02d:%02d WITA tiba; memulai Gemini otomatis. Keterangan=%s",
                         fired.id, fired.kind == ALARM_KIND_WAKE ? "bangun" : "pengingat",
                         fired.hour, fired.minute, fired.label);
                display_status("Alarm: memulai Gemini");
                if (!assistant_active && !websocket_lifecycle_busy()) {
                    assistant_active = true;
                    disconnect_wait_start_us = 0;
                    face_set_state(FACE_THINKING);
                    websocket_app_start();
                }
            }
        }
        if (alarm_prompt_pending && !assistant_active && !websocket_lifecycle_busy()) {
            assistant_active = true;
            disconnect_wait_start_us = 0;
            face_set_state(FACE_THINKING);
            websocket_app_start();
        }
        if (websocket_take_standby_request()) {
            standby_pending = true;
            standby_requested_us = esp_timer_get_time();
            ESP_LOGI(TAG, "STANDBY REQUESTED -> menunggu playback selesai (batas pengaman 60 detik)");
        }
        bool standby_audio_done = standby_pending && websocket_take_audio_playback_complete();
        bool standby_drain_timeout = standby_pending && standby_requested_us > 0 &&
            (esp_timer_get_time() - standby_requested_us >= STANDBY_PLAYBACK_DRAIN_TIMEOUT_US);
        if (standby_pending && (standby_audio_done || standby_drain_timeout)) {
            if (standby_drain_timeout && !standby_audio_done)
                ESP_LOGW(TAG, "STANDBY: timeout pengaman 60 detik; playback belum selesai, sesi diakhiri");
            else
                ESP_LOGI(TAG, "STANDBY: playback selesai -> kembali menunggu wake word");
            websocket_request_fresh_session_reset();
            websocket_disconnect();
            assistant_active = false;
            standby_pending = false;
            standby_requested_us = 0;
            buffer_pos = 0;
            // Standby: no conversation is active; show sleep face while WakeNet keeps listening.
            face_set_state(FACE_SLEEP);
            display_status("Menunggu wakeword");
            continue;
        }
        // ---- Radio streaming ----
        // 1) Gemini sudah menjawab tool putar_radio dan selesai mengucapkan konfirmasi:
        //    akhiri sesi Gemini lalu mulai radio.
        if (radio_has_pending() && !radio_is_active()) {
            const int64_t age_us = radio_pending_age_us();
            const bool spoke_done = age_us > 1200000LL && websocket_take_audio_playback_complete();
            const bool no_speech = age_us > 6000000LL && !audio_turn_active && !audio_turn_complete_pending;
            if (spoke_done || no_speech || age_us > 20000000LL) {
                ESP_LOGI(TAG, "RADIO: konfirmasi Gemini selesai; sesi dihentikan, radio dimulai");
                websocket_request_fresh_session_reset();
                websocket_disconnect();
                assistant_active = false;
                buffer_pos = 0;
                for (int i = 0; i < 60 && websocket_lifecycle_busy(); ++i) vTaskDelay(pdMS_TO_TICKS(50));
                websocket_set_mic_uplink_paused(false);
                audio_hal_set_mic_enabled(false);
                if (!radio_start_pending()) ESP_LOGW(TAG, "RADIO: gagal dimulai; kembali ke obrolan");
                continue;
            }
        }
        // 2) Radio sedang main: mic mati, wake word dan Gemini tidak aktif. Sentuh sensor = berhenti.
        if (radio_is_active()) {
            audio_hal_set_mic_enabled(false);   // websocket bisa menyalakannya lagi saat membersihkan sesi
            buffer_pos = 0;
            mic_voice_detected = false;
            tx_backpressure_start_us = 0;
            consecutive_tx_enqueue_failures = 0;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        // 3) Radio selesai: kembali ke obrolan dan beri tahu Gemini.
        if (radio_has_ended()) {
            char radio_name[48] = {};
            int radio_reason = 0;
            if (assistant_active || alarm_prompt_pending) {
                (void)radio_take_ended(radio_name, sizeof(radio_name), &radio_reason);   // sesi lain sudah jalan
            } else if (!websocket_lifecycle_busy() && radio_take_ended(radio_name, sizeof(radio_name), &radio_reason)) {
                audio_hal_set_mic_enabled(true);
                strlcpy(radio_note_name, radio_name, sizeof(radio_note_name));
                radio_note_reason = radio_reason;
                radio_note_pending = true;
                assistant_active = true;
                disconnect_wait_start_us = 0;
                face_set_state(FACE_THINKING);
                ESP_LOGI(TAG, "RADIO selesai (alasan=%d); memulai sesi Gemini baru", radio_reason);
                websocket_app_start();
                continue;
            }
        }
        // During web search/TX congestion, keep draining the capture queue.
        // The I2S capture task stays active; not reading here fills its queue and
        // causes avoidable MIC capture drops. Drain raw PCM only: never send or
        // replay audio captured while uplink is paused/back-pressured.
        // Hanya saat uplink dijeda (pencarian web) suara dibuang. Saat antrean kirim penuh (jaringan
        // sedang lambat), suara TETAP dibaca dan ditahan di penyangga 6 detik lalu dikirim menyusul;
        // dulu suara dibuang di sini sehingga ucapan hilang.
        if (websocket_mic_uplink_paused) {
            static uint8_t discard_pcm[1024];
            if (audio_hal_is_mic_enabled())
                (void)audio_read_mic_raw(discard_pcm, sizeof(discard_pcm));
            buffer_pos = 0;
            tx_backpressure_start_us = 0;
            consecutive_tx_enqueue_failures = 0;
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        // WakeNet gets the same raw I2S PCM path as Repo5. Keep NSNet2 and
        // conversation gain exclusively on the active Gemini audio path.
        // Sesi Gemini belum siap dan penyangga hampir penuh: buang suara TERTUA (2 detik),
        // supaya yang tersimpan selalu ucapan terbaru, bukan yang paling lama.
        if (assistant_active && !websocket_is_connected() &&
            buffer_pos + 4096 > AUDIO_BUFFER_SIZE) {
            constexpr size_t DROP_OLD_BYTES = 65536;
            const size_t drop = buffer_pos < DROP_OLD_BYTES ? buffer_pos : DROP_OLD_BYTES;
            memmove(audio_buffer, audio_buffer + drop, buffer_pos - drop);
            buffer_pos -= drop;
            mic_buffer_lost_bytes += drop;
        }
        const bool mic_buffer_full = buffer_pos >= AUDIO_BUFFER_SIZE;
        // When TX is back-pressured, do not consume more PCM just to discard it.
        // Keep the buffered audio intact and let the capture queue's drop counter
        // reveal sustained overload instead of silently throwing away overflow reads.
        size_t bytes_read = 0;
        if (!mic_buffer_full) {
            const size_t mic_read_capacity = AUDIO_BUFFER_SIZE - buffer_pos;
            uint8_t *mic_read_dest = audio_buffer + buffer_pos;
            speaker_id_mic_read_lock();
            bytes_read = assistant_active
                ? audio_read_mic(mic_read_dest, mic_read_capacity)
                : audio_read_mic_raw(mic_read_dest, mic_read_capacity);
            speaker_id_mic_read_unlock();
        }
        if (!audio_hal_is_mic_enabled()) {
            buffer_pos = 0;
            tx_backpressure_start_us = 0;
            consecutive_tx_enqueue_failures = 0;
            vTaskDelay(1);
            continue;
        }
        if (websocket_mic_uplink_paused) {
            if (bytes_read > 0) mic_buffer_lost_bytes += bytes_read;
            buffer_pos = 0; // Never buffer/replay speech captured during a web search.
            tx_backpressure_start_us = 0;
            consecutive_tx_enqueue_failures = 0;
        } else if (mic_buffer_full) {
            if (bytes_read > 0) mic_buffer_lost_bytes += bytes_read;
            int64_t now_us = esp_timer_get_time();
            if (now_us - last_tx_diag_us >= 1000000) {
                last_tx_diag_us = now_us;
                ESP_LOGW("MIC_TX_DIAG", "MIC buffer FULL: buffer_pos=%u lost_bytes_total=%llu consecutive_enqueue_failures=%lu read_discarded=%u",
                         (unsigned)buffer_pos, (unsigned long long)mic_buffer_lost_bytes,
                         (unsigned long)consecutive_tx_enqueue_failures, (unsigned)bytes_read);
            }
        } else if (bytes_read > 0) {
            buffer_pos += bytes_read;
            if (buffer_pos > buffer_high_water) buffer_high_water = buffer_pos;

            // Peak/RMS-like energy gate for the brighter red "voice detected" state.
            // This is only a visual indicator; Gemini still decides speech boundaries.
            if (assistant_active && bytes_read >= sizeof(int16_t)) {
                const int16_t *samples = reinterpret_cast<const int16_t *>(audio_buffer + buffer_pos - bytes_read);
                const size_t sample_count = bytes_read / sizeof(int16_t);
                uint64_t energy_sum = 0;
                for (size_t i = 0; i < sample_count; ++i) {
                    const int32_t sample = samples[i];
                    energy_sum += (uint64_t)(sample * sample);
                }
                const uint32_t mean_energy = (uint32_t)(energy_sum / sample_count);
                mic_voice_detected = mean_energy > AUDIO_VOICE_ENERGY_THRESHOLD_SQ;
                utterance_feed(samples, sample_count, mean_energy > AUDIO_VOICE_ENERGY_THRESHOLD_SQ / 4);
            } else {
                mic_voice_detected = false;
            }
        }

        if (!assistant_active) {
            portENTER_CRITICAL(&speaker_context_mux);
            radio_note_pending = false;
            speaker_context_pending = false;  // sesi lama selesai; jangan bawa nama ke sesi berikutnya
            speaker_session_seq = speaker_session_seq + 1; // hasil worker yang terlambat jadi basi
            speaker_change_pending = false;
            speaker_current_name[0] = '\0';
            speaker_candidate_name[0] = '\0';
            speaker_candidate_count = 0;
            utt_reset();
            portEXIT_CRITICAL(&speaker_context_mux);
            // Wait for the previous WebSocket client to be destroyed before
            // detecting another wake word and starting a fresh Gemini session.
            static int64_t lifecycle_busy_since_us = 0;
            if (websocket_lifecycle_busy()) {
                /* Pengaman: jika pembersihan koneksi macet > 30 detik, robot tidak akan pernah
                 * mendengar wake word lagi. Restart supaya pulih sendiri. */
                const int64_t busy_now_us = esp_timer_get_time();
                if (lifecycle_busy_since_us == 0) {
                    lifecycle_busy_since_us = busy_now_us;
                } else if (busy_now_us - lifecycle_busy_since_us > 30 * 1000000LL) {
                    ESP_LOGE(TAG, "Koneksi Gemini macet > 30 detik saat menunggu wake word; restart untuk pulih");
                    robot_stats_event("restart_macet");
                    vTaskDelay(pdMS_TO_TICKS(300));
                    esp_restart();
                }
                buffer_pos = 0;
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            lifecycle_busy_since_us = 0;
            int64_t now_debug_us = esp_timer_get_time();
            if (now_debug_us - last_activity_debug_us >= 1000000) {
                last_activity_debug_us = now_debug_us;
                int32_t max_abs = 0;
                size_t wake_bytes = (size_t)wakeword_get_chunk_samples() * sizeof(int16_t);
                if (wakeword_get_chunk_samples() > 0 && buffer_pos >= wake_bytes) {
                    int16_t *pcm = reinterpret_cast<int16_t *>(audio_buffer);
                    for (int i = 0; i < wakeword_get_chunk_samples(); ++i) {
                        int32_t val = pcm[i];
                        int32_t magnitude = val < 0 ? -val : val;
                        if (magnitude > max_abs) max_abs = magnitude;
                    }
                }
                ESP_LOGI("WAKE_DEBUG", "buffer_pos=%u max_abs=%ld detect_calls=%d last_result=%d chunk_samples=%d bytes_read=%u",
                         (unsigned)buffer_pos, (long)max_abs, detect_calls, last_wake_result,
                         wakeword_get_chunk_samples(), (unsigned)bytes_read);
            }

            while (wakeword_is_ready() && wakeword_get_chunk_samples() > 0 &&
                   buffer_pos >= (size_t)wakeword_get_chunk_samples() * sizeof(int16_t)) {
                size_t wake_bytes = (size_t)wakeword_get_chunk_samples() * sizeof(int16_t);
                int16_t *wake_pcm = reinterpret_cast<int16_t *>(audio_buffer);
                if (wake_ring) {
                    const size_t chunk = (size_t)wakeword_get_chunk_samples();
                    for (size_t i = 0; i < chunk; ++i)
                        wake_ring[(wake_ring_pos + i) % WAKE_RING_SAMPLES] = wake_pcm[i];
                    wake_ring_pos = (wake_ring_pos + chunk) % WAKE_RING_SAMPLES;
                    wake_ring_filled += chunk;
                    if (wake_ring_filled > WAKE_RING_SAMPLES) wake_ring_filled = WAKE_RING_SAMPLES;
                }
                detect_calls++;
                int wake_result = wakeword_detect(wake_pcm);
                last_wake_result = wake_result;

                if (wake_result > 0) {
                    ESP_LOGW(TAG, ">>> WAKE WORD TERDETEKSI: HI, ESP (id=%d)", wake_result);
                    display_status("Wakeword terdeteksi");
                    assistant_active = true;
                    disconnect_wait_start_us = 0;
                    face_set_state(FACE_THINKING);
                    websocket_app_start();
                    /* Koneksi Gemini berjalan di task lain; sambil menunggu, kenali suara. */
                    queue_speaker_identification(wake_ring, wake_ring_pos, wake_ring_filled);
                    wake_ring_filled = 0;
                    buffer_pos = 0;
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }

                size_t remainder = buffer_pos - wake_bytes;
                if (remainder > 0) memmove(audio_buffer, audio_buffer + wake_bytes, remainder);
                buffer_pos = remainder;
            }

            if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
                vTaskDelay(pdMS_TO_TICKS(50));
                if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
                    while (gpio_get_level(BOOT_BUTTON_GPIO) == 0) vTaskDelay(pdMS_TO_TICKS(10));
                    ESP_LOGI(TAG, "Tombol ditekan! Memulai sesi...");
                    assistant_active = true;
                    face_set_state(FACE_THINKING);
                    websocket_app_start();
                }
            }

            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        if (!websocket_is_connected()) {
            tx_backpressure_start_us = 0;
            consecutive_tx_enqueue_failures = 0;

            /*
             * Give the reconnect manager a full 15 seconds after each loss of
             * connectivity. Measuring from the original session start causes a
             * healthy long-running session to abandon reconnect immediately
             * when its transport resets.
             */
            const int64_t now_us = esp_timer_get_time();
            if (disconnect_wait_start_us == 0) disconnect_wait_start_us = now_us;
            if (now_us - disconnect_wait_start_us > 25 * 1000000LL) {
                ESP_LOGW(TAG, "WebSocket tidak pulih dalam 25 detik setelah terputus; akhiri sesi dan kembali ke wake word.");
                display_status("Menunggu wakeword");
                websocket_request_fresh_session_reset();
                websocket_disconnect();
                assistant_active = false;
                face_set_state(FACE_SLEEP);
                buffer_pos = 0;
                disconnect_wait_start_us = 0;
                continue;
            }
            buffer_pos = 0;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        disconnect_wait_start_us = 0;

        // Suara dikenal: beri tahu Gemini namanya sekali setelah sesi siap.
        // Suara tidak dikenal: tidak ada yang dikirim, jadi Gemini tidak menyapa.
        char speaker_name_to_send[SPEAKER_ID_NAME_MAX + 1] = {};
        bool has_speaker_name = false;
        portENTER_CRITICAL(&speaker_context_mux);
        has_speaker_name = speaker_context_pending;
        if (has_speaker_name)
            strlcpy(speaker_name_to_send, speaker_pending_name, sizeof(speaker_name_to_send));
        portEXIT_CRITICAL(&speaker_context_mux);
        if (has_speaker_name && websocket_send_speaker_context(speaker_name_to_send)) {
            portENTER_CRITICAL(&speaker_context_mux);
            if (speaker_context_pending &&
                strcmp(speaker_pending_name, speaker_name_to_send) == 0) {
                speaker_context_pending = false;
                strlcpy(speaker_current_name, speaker_name_to_send, sizeof(speaker_current_name));
            }
            portEXIT_CRITICAL(&speaker_context_mux);
        }
        // Pembicara baru dikenali di tengah obrolan (warna suara): beri tahu Gemini tanpa menyapa ulang.
        if (!has_speaker_name) {
            char change_name[SPEAKER_ID_NAME_MAX + 1] = {};
            bool has_change = false;
            portENTER_CRITICAL(&speaker_context_mux);
            has_change = speaker_change_pending;
            if (has_change && strcasecmp(speaker_change_name, speaker_current_name) == 0) {
                speaker_change_pending = false;   // sudah diberitahukan (mis. lewat sapaan Hi ESP)
                has_change = false;
            }
            if (has_change) strlcpy(change_name, speaker_change_name, sizeof(change_name));
            portEXIT_CRITICAL(&speaker_context_mux);
            if (has_change && websocket_send_speaker_update(change_name)) {
                portENTER_CRITICAL(&speaker_context_mux);
                if (speaker_change_pending && strcmp(speaker_change_name, change_name) == 0) {
                    speaker_change_pending = false;
                    strlcpy(speaker_current_name, change_name, sizeof(speaker_current_name));
                }
                portEXIT_CRITICAL(&speaker_context_mux);
            }
        }
        if (radio_note_pending && websocket_send_radio_ended(radio_note_name, radio_note_reason))
            radio_note_pending = false;
        if (alarm_prompt_pending && websocket_is_connected()) {
            if (websocket_send_alarm_prompt(alarm_prompt_info.hour, alarm_prompt_info.minute,
                                            alarm_prompt_info.label, alarm_prompt_info.kind)) {
                alarm_prompt_pending = false;
                if (alarm_prompt_standby_after) {
                    standby_pending = true;  // kembali ke standby setelah ucapan alarm selesai diputar
                    standby_requested_us = esp_timer_get_time();
                    ESP_LOGI(TAG, "Prompt alarm terkirim ke Gemini; menunggu audio selesai lalu kembali standby");
                } else {
                    ESP_LOGI(TAG, "Prompt pengingat terkirim ke Gemini; sesi obrolan tetap berjalan");
                }
            }
        }

        // Gemini Live receives microphone audio continuously while the speaker is idle.
        // 1280 bytes = 640 samples = 40 ms at 16 kHz mono PCM16. A 40 ms
        // command halves JSON/Base64/WebSocket call frequency versus 20 ms
        // frames while keeping microphone capture independent of network TX.
        // Gemini handles speech start/end detection server-side.
        constexpr size_t GEMINI_MIC_CHUNK_BYTES = 1280;
        // Kirim dari "send_off" (tanpa memindahkan seluruh penyangga tiap potongan; penyangga besar).
        size_t send_off = 0;
        while (buffer_pos - send_off >= GEMINI_MIC_CHUNK_BYTES) {
            if (!audio_hal_is_mic_enabled()) { buffer_pos = 0; send_off = 0; break; }
            /* Sesi belum siap (menyambung): simpan saja, jangan hitung sebagai macet dan jangan dibuang. */
            if (!websocket_is_connected()) {
                tx_backpressure_start_us = 0;
                consecutive_tx_enqueue_failures = 0;
                break;
            }
            /* Keep PCM buffered until TX confirms enqueue. If the queue is full,
             * retry on the next loop instead of consuming and losing this frame. */
            if (!websocket_send_audio_data(audio_buffer + send_off, GEMINI_MIC_CHUNK_BYTES)) {
                consecutive_tx_enqueue_failures++;
                const int64_t now_us = esp_timer_get_time();
                if (tx_backpressure_start_us == 0) tx_backpressure_start_us = now_us;
                if (now_us - tx_backpressure_start_us >= TX_BACKPRESSURE_ABORT_US) {
                    /* Sustained enqueue backpressure: drop buffered microphone
                     * audio, reset the producer-side buffer, but keep the
                     * current WebSocket and Gemini session alive. */
                    ESP_LOGW(TAG, "TX backpressure > 4 detik; audio tertahan dibuang, sesi Gemini tetap aktif");
                    tx_backpressure_start_us = 0;
                    consecutive_tx_enqueue_failures = 0;
                    mic_buffer_lost_bytes += buffer_pos;
                    buffer_pos = 0;
                    send_off = 0;
                    vTaskDelay(pdMS_TO_TICKS(10));
                    break;
                }
                if (consecutive_tx_enqueue_failures == 1 ||
                    consecutive_tx_enqueue_failures % 25U == 0 ||
                    now_us - last_tx_diag_us >= 1000000) {
                    last_tx_diag_us = now_us;
                    ESP_LOGW("MIC_TX_DIAG", "TX enqueue hold: buffer_pos=%u high_water=%u consecutive_failures=%lu lost_bytes=%llu queue=%u",
                             (unsigned)buffer_pos, (unsigned)buffer_high_water,
                             (unsigned long)consecutive_tx_enqueue_failures,
                             (unsigned long long)mic_buffer_lost_bytes,
                             websocket_tx_queue ? (unsigned)uxQueueMessagesWaiting(websocket_tx_queue) : 0U);
                }
                vTaskDelay(1);
                break;
            }
            consecutive_tx_enqueue_failures = 0;
            tx_backpressure_start_us = 0;

            send_off += GEMINI_MIC_CHUNK_BYTES;
        }
        if (send_off > 0) {
            const size_t remainder = buffer_pos - send_off;
            if (remainder > 0) memmove(audio_buffer, audio_buffer + send_off, remainder);
            buffer_pos = remainder;
        }

        // i2s_channel_read() already blocks until the next RX DMA data is
        // available. A 10 ms delay here throttled the microphone producer and
        // could make the realtime input stream fall behind. Keep only one
        // FreeRTOS tick so the task yields without intentionally inserting
        // another 10 ms of latency between microphone batches.
        vTaskDelay(1);
    }
}

extern "C" void app_main()
{
    ESP_LOGI("MAIN", "Total PSRAM: %d bytes", esp_psram_get_size()); ESP_LOGI("MAIN", "Free Heap: %d bytes", esp_get_free_heap_size()); ESP_LOGI("MAIN", "Free PSRAM: %d bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM)); ESP_LOGI(TAG, "ESP32-S3 Asisten Kamar Dimulai...");
    esp_err_t ret = nvs_flash_init(); if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) { ESP_ERROR_CHECK(nvs_flash_erase()); ret = nvs_flash_init(); } ESP_ERROR_CHECK(ret);

    // RGB status LED is independent from OLED/I2C and must not block AI startup.
    if (led_rgb_init()) {
        BaseType_t led_task_result = xTaskCreate(led_status_task, "led_status", 2048, NULL, 2, NULL);
        if (led_task_result != pdPASS) {
            ESP_LOGE(TAG, "Gagal membuat led_status_task");
            led_rgb_set_state(LED_RGB_OFF);
        }
    } else {
        ESP_LOGW(TAG, "LED RGB tidak tersedia; sistem AI tetap dilanjutkan");
    }

    // Servo init is isolated: failure must not prevent audio/Gemini startup.
    if (!servo_head_init()) ESP_LOGE(TAG, "Servo init gagal; sistem AI tetap dilanjutkan");

    // WebConfig remains an isolated boot-time configuration layer.
    if (web_config_is_needed()) {
        led_wifi_config_mode = true;
        led_boot_phase = false;
        display_status("Config Mode");
        // Halaman Profil Suara (/voice): perlu mic dan penyimpanan.
        // Mic dimatikan dulu; hanya dinyalakan sebentar saat merekam.
        audio_hal_init();
        audio_hal_set_mic_enabled(false);
        if (speaker_id_init()) web_config_set_extra_handlers(speaker_id_web_register);
        else ESP_LOGW(TAG, "speaker_id gagal init; halaman Profil Suara tidak aktif");
        web_config_start();
        ota_update_mark_valid();  // mode konfigurasi = robot hidup; jangan di-rollback
        // Di mode konfigurasi: tekan BOOT 3x lagi = keluar dan boot normal (tanpa menyimpan).
        while (1) {
            if (boot_triple_press_poll()) {
                ESP_LOGW(TAG, "Tombol BOOT 3x -> keluar Config Mode, boot normal");
                display_status("Keluar Config Mode");
                vTaskDelay(pdMS_TO_TICKS(800));
                web_config_exit_and_restart();
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    led_boot_phase = false;

    // Recover/initialize persistent memory before starting OLED animation tasks.
    // SPIFFS formatting may temporarily stall flash/cache execution; keep OLED2
    // animation tasks unstarted until storage initialization has completed.
    if (!ai_memory_init()) ESP_LOGW(TAG, "Memori lokal belum siap; fitur catatan akan gagal sampai penyimpanan pulih");
    // Muat profil suara (PSRAM + SPIFFS). Gagal bukan masalah: robot tetap jalan tanpa sapaan nama.
    if (!speaker_id_init()) ESP_LOGW(TAG, "speaker_id tidak aktif; robot tidak akan menyapa dengan nama");

    oled_init(); face_animation_start(); face_set_state(FACE_SLEEP); display_status("Booting...");
    audio_hal_init(); audio_i2s_test_tone();
    if (!wakeword_init()) { ESP_LOGE(TAG, "WakeNet init gagal. Sistem tetap bisa dimulai dengan tombol BOOT."); display_status("ERROR: WakeNet gagal"); } else { display_status("Menunggu wakeword"); }
    // Initialize AFE before a Gemini session starts so model loading does not stall live audio.
    audio_hal_afe_init();
    // Memory storage was initialized before OLED/audio tasks above.
    gpio_set_direction(BOOT_BUTTON_GPIO, GPIO_MODE_INPUT); gpio_set_pull_mode(BOOT_BUTTON_GPIO, GPIO_PULLUP_ONLY); ESP_LOGI(TAG, "Tombol boot siap di GPIO0");
    if (xTaskCreate(boot_button_task, "boot_btn", 3072, NULL, 3, NULL) != pdPASS) ESP_LOGW(TAG, "boot_button_task gagal dibuat; tekan 3x tidak aktif");
    uart_control_init();
    if (!touch_emotion_start()) ESP_LOGE(TAG, "Sensor sentuh gagal dimulai; fitur sentuh dinonaktifkan");

    display_status("Menghubungkan WiFi...");
    wifi_init_sta();
    if (!wifi_wait_for_connection(15000)) {
        ESP_LOGE(TAG, "Wi-Fi tidak mendapatkan IP. Restart dalam 5 detik untuk mencoba lagi.");
        display_status("WiFi Gagal!");
        face_set_state(FACE_SLEEP);
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }

    esp_wifi_set_ps(WIFI_PS_NONE); ESP_LOGI(TAG, "WiFi power save dimatikan"); ESP_LOGI(TAG, "WIFI READY - lanjut ke NTP"); bool ntp_time_synced = sync_sntp_time(); alarm_set_time_ready(ntp_time_synced); robot_stats_start(); if (!alarm_init()) ESP_LOGW(TAG, "Alarm NVS belum siap; fitur alarm mungkin belum bisa digunakan"); ESP_LOGI(TAG, "Menunggu 1 detik..."); vTaskDelay(pdMS_TO_TICKS(1000));
    debug_network_path(); vTaskDelay(pdMS_TO_TICKS(1000)); face_set_state(FACE_SLEEP); display_status("Sistem siap. Katakan Hi, ESP...");

    BaseType_t task_result = xTaskCreate(audio_task, "audio_task", 10240, NULL, 5, NULL); if (task_result != pdPASS) ESP_LOGE(TAG, "Gagal membuat audio_task!"); else ESP_LOGI(TAG, "audio_task berhasil dimulai.");
    // OTA: setelah hidup normal 20 detik, tandai program ini sehat (batalkan rollback).
    vTaskDelay(pdMS_TO_TICKS(20000));
    ota_update_mark_valid();
    // Cek diam-diam versi baru di GitHub (hanya dicatat di log). Tunggu sampai robot
    // benar-benar menganggur: koneksi TLS tambahan saat Gemini sedang menyambung
    // menghabiskan RAM internal dan bisa menggagalkan sambungan Gemini.
    for (int i = 0; i < 300; ++i) {
        if (!assistant_active && !websocket_lifecycle_busy() && !radio_is_active()) {
            vTaskDelay(pdMS_TO_TICKS(3000));   // pastikan benar-benar tenang
            if (!assistant_active && !websocket_lifecycle_busy() && !radio_is_active()) {
                (void)ota_update_run_async(false, NULL, NULL);
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}
