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

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nvs_flash.h"
#include "esp_sntp.h"

#include <sys/time.h>
#include <time.h>

#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

static const char *TAG = "MAIN";
#define BOOT_BUTTON_GPIO GPIO_NUM_0

// Explicit Gemini enrollment consumes the existing audio_task microphone stream.
static constexpr size_t SPEAKER_CAPTURE_SAMPLES = 38400; // 2.4 s at 16 kHz
static int16_t *speaker_capture_pcm = nullptr;
static volatile bool speaker_capture_active = false;
static volatile bool speaker_busy = false;
static size_t speaker_capture_pos = 0;
static char speaker_capture_name[32] = {};
static bool speaker_capture_verify = false;
static void speaker_process_task(void *arg) {
    (void)arg;
    esp_err_t err;
    float distance = 0.0f;
    if (speaker_capture_verify) {
        err = speaker_id_compare_pcm16(speaker_capture_pcm, SPEAKER_CAPTURE_SAMPLES, &distance);
        if (err == ESP_OK) {
            ESP_LOGI("SPEAKER_ID", "Verifikasi diagnostik selesai: DTW distance=%.5f; belum ada ambang keputusan", (double)distance);
            display_status("Cek suara selesai");
        } else {
            ESP_LOGW("SPEAKER_ID", "Verifikasi gagal: %s", esp_err_to_name(err));
            display_status("Cek suara gagal");
        }
        speaker_id_complete_verification(err, distance);
        speaker_capture_verify = false;
    } else {
        err = speaker_id_enroll_named_pcm16(speaker_capture_pcm, SPEAKER_CAPTURE_SAMPLES, speaker_capture_name);
        if (err == ESP_OK) {
            ESP_LOGI("SPEAKER_ID", "Pendaftaran '%s' berhasil dan tersimpan di NVS", speaker_capture_name);
            display_status("Suara tersimpan");
        } else {
            ESP_LOGW("SPEAKER_ID", "Pendaftaran gagal: %s", esp_err_to_name(err));
            display_status("Daftar suara gagal");
        }
        speaker_id_complete_enrollment(err);
    }
    speaker_busy = false;
    vTaskDelete(nullptr);
}
static void speaker_capture_start(const char *name) {
    if (!speaker_capture_pcm || !name || !name[0] || speaker_busy || speaker_capture_active) {
        speaker_id_complete_enrollment(ESP_ERR_INVALID_STATE);
        return;
    }
    strlcpy(speaker_capture_name, name, sizeof(speaker_capture_name));
    speaker_capture_pos = 0;
    speaker_busy = true;
    speaker_capture_active = true;
    ESP_LOGI("SPEAKER_ID", "Pendaftaran eksplisit '%s': silakan bicara 2,4 detik", speaker_capture_name);
    display_status("Daftar suara: bicara");
}

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

static void sync_sntp_time(void)
{
    ESP_LOGI(TAG, "Mencari server NTP..."); display_status("Sync Jam Network..");
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL); esp_sntp_setservername(0, "time.google.com"); esp_sntp_setservername(1, "id.pool.ntp.org"); esp_sntp_setservername(2, "pool.ntp.org"); esp_sntp_init();
    int retry = 0; const int max_retries = 10; time_t now = 0; struct tm timeinfo = {};
    while (retry < max_retries) { time(&now); localtime_r(&now, &timeinfo); if (timeinfo.tm_year >= (2024 - 1900)) { ESP_LOGI(TAG, "Waktu cocok! Tahun: %d", timeinfo.tm_year + 1900); display_status("Jam Cocok!"); vTaskDelay(pdMS_TO_TICKS(1000)); return; } vTaskDelay(pdMS_TO_TICKS(500)); retry++; }
    ESP_LOGW(TAG, "NTP gagal. Menggunakan waktu fallback."); struct timeval tv = { .tv_sec = 1770000000, .tv_usec = 0 }; settimeofday(&tv, NULL); display_status("Jam Set Fallback"); vTaskDelay(pdMS_TO_TICKS(1000));
}

static volatile bool assistant_active = false;
static volatile bool led_boot_phase = true;
static volatile bool led_wifi_config_mode = false;
static volatile bool mic_voice_detected = false;
static int64_t connect_start_us = 0;

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
    constexpr size_t AUDIO_BUFFER_SIZE = 8192;
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
    size_t buffer_pos = 0;
    size_t buffer_high_water = 0;
    uint64_t mic_buffer_lost_bytes = 0;
    uint32_t consecutive_tx_enqueue_failures = 0;
    int64_t tx_backpressure_start_us = 0;
    constexpr int64_t TX_BACKPRESSURE_ABORT_US = 2000000LL;
    int64_t last_tx_diag_us = 0;
    int64_t last_activity_debug_us = 0;
    static int last_wake_result = 0;
    static int detect_calls = 0;

    static int64_t boot_press_start_us = 0;
    static bool boot_button_was_down = false;

    bool standby_pending = false;
    int64_t standby_requested_us = 0;
    constexpr int64_t STANDBY_PLAYBACK_DRAIN_TIMEOUT_US = 8000000LL;

    while (1) {
        if (websocket_take_standby_request()) {
            standby_pending = true;
            standby_requested_us = esp_timer_get_time();
            ESP_LOGI(TAG, "STANDBY REQUESTED -> menunggu playback selesai (maksimal 8 detik)");
        }
        bool standby_audio_done = standby_pending && websocket_take_audio_playback_complete();
        bool standby_drain_timeout = standby_pending && standby_requested_us > 0 &&
            (esp_timer_get_time() - standby_requested_us >= STANDBY_PLAYBACK_DRAIN_TIMEOUT_US);
        if (standby_pending && (standby_audio_done || standby_drain_timeout)) {
            if (standby_drain_timeout && !standby_audio_done)
                ESP_LOGW(TAG, "STANDBY: timeout menunggu playback; paksa kembali ke wake word");
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
        // WakeNet gets the same raw I2S PCM path as Repo5. Keep NSNet2 and
        // conversation gain exclusively on the active Gemini audio path.
        const bool mic_buffer_full = buffer_pos >= AUDIO_BUFFER_SIZE;
        // When TX is back-pressured, do not consume more PCM just to discard it.
        // Keep the buffered audio intact and let the capture queue's drop counter
        // reveal sustained overload instead of silently throwing away overflow reads.
        size_t bytes_read = 0;
        if (!mic_buffer_full) {
            const size_t mic_read_capacity = AUDIO_BUFFER_SIZE - buffer_pos;
            uint8_t *mic_read_dest = audio_buffer + buffer_pos;
            bytes_read = assistant_active
                ? audio_read_mic(mic_read_dest, mic_read_capacity)
                : audio_read_mic_raw(mic_read_dest, mic_read_capacity);
        }
        if (!audio_hal_is_mic_enabled()) {
            buffer_pos = 0;
            tx_backpressure_start_us = 0;
            consecutive_tx_enqueue_failures = 0;
            vTaskDelay(1);
            continue;
        }

        char enrollment_name[32] = {};
        if (speaker_capture_pcm && !speaker_capture_active && !speaker_busy) {
            if (speaker_id_take_enrollment_request(enrollment_name, sizeof(enrollment_name))) {
                speaker_capture_verify = false;
                speaker_capture_start(enrollment_name);
            } else if (speaker_id_take_verification_request()) {
                speaker_capture_pos = 0;
                speaker_busy = true;
                speaker_capture_verify = true;
                speaker_capture_active = true;
                ESP_LOGI("SPEAKER_ID", "Verifikasi diagnostik: silakan bicara 2,4 detik");
                display_status("Cek suara: bicara");
            }
        }

        // Capture from the same microphone read already feeding Gemini.
        if (!mic_buffer_full && bytes_read > 0 &&
            speaker_capture_active && speaker_capture_pcm) {
            size_t available_samples = bytes_read / sizeof(int16_t);
            size_t remaining_samples = SPEAKER_CAPTURE_SAMPLES - speaker_capture_pos;
            size_t take_samples = available_samples < remaining_samples
                ? available_samples : remaining_samples;
            if (take_samples > 0) {
                const int16_t *src = reinterpret_cast<const int16_t *>(
                    audio_buffer + buffer_pos);
                memcpy(speaker_capture_pcm + speaker_capture_pos,
                       src, take_samples * sizeof(int16_t));
                speaker_capture_pos += take_samples;
            }
            if (speaker_capture_pos >= SPEAKER_CAPTURE_SAMPLES) {
                speaker_capture_active = false;
                BaseType_t result = xTaskCreate(
                    speaker_process_task, "speaker_process", 8192,
                    nullptr,
                    3, nullptr);
                if (result != pdPASS) {
                    speaker_busy = false;
                    if (speaker_capture_verify) {
                        speaker_id_complete_verification(ESP_ERR_NO_MEM, 0.0f);
                        speaker_capture_verify = false;
                    } else {
                        speaker_id_complete_enrollment(ESP_ERR_NO_MEM);
                    }
                    ESP_LOGE("SPEAKER_ID", "Gagal membuat task pemrosesan");
                } else {
                    ESP_LOGI("SPEAKER_ID", "Audio terkumpul; memproses MFCC + DTW");
                }
            }
        }

        if (mic_buffer_full) {
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
                mic_voice_detected = mean_energy > (uint32_t)(1200 * 1200);
            } else {
                mic_voice_detected = false;
            }
        }

        if (!assistant_active) {
            // Wait for the previous WebSocket client to be destroyed before
            // detecting another wake word and starting a fresh Gemini session.
            if (websocket_lifecycle_busy()) {
                buffer_pos = 0;
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
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
                detect_calls++;
                int wake_result = wakeword_detect(wake_pcm);
                last_wake_result = wake_result;

                if (wake_result > 0) {
                    ESP_LOGW(TAG, ">>> WAKE WORD TERDETEKSI: HI, ESP (id=%d)", wake_result);
                    display_status("Wakeword terdeteksi");
                    assistant_active = true;
                    connect_start_us = esp_timer_get_time();
                    face_set_state(FACE_THINKING);
                    websocket_app_start();
                    buffer_pos = 0;
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }

                size_t remainder = buffer_pos - wake_bytes;
                if (remainder > 0) memmove(audio_buffer, audio_buffer + wake_bytes, remainder);
                buffer_pos = remainder;
            }

            const bool boot_button_down = gpio_get_level(BOOT_BUTTON_GPIO) == 0;
            if (boot_button_down && !boot_button_was_down) {
                boot_press_start_us = esp_timer_get_time();
                boot_button_was_down = true;
            } else if (!boot_button_down && boot_button_was_down) {
                const int64_t held_us = esp_timer_get_time() - boot_press_start_us;
                boot_press_start_us = 0;
                boot_button_was_down = false;
                if (held_us >= 50000LL && held_us < 1200000LL) {
                    ESP_LOGI(TAG, "Tombol ditekan! Memulai sesi...");
                    assistant_active = true;
                    connect_start_us = esp_timer_get_time();
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

            /* Match RepoV3: the 15-second timeout is measured from session start,
             * not from the latest disconnect. A disconnect after that window
             * therefore returns to wake-word mode without another 15-second wait. */
            if (esp_timer_get_time() - connect_start_us > 15 * 1000000LL) {
                ESP_LOGW(TAG, "WebSocket tidak pulih dalam 15 detik; akhiri sesi dan kembali ke wake word.");
                display_status("Menunggu wakeword");
                websocket_request_fresh_session_reset();
                websocket_disconnect();
                assistant_active = false;
                face_set_state(FACE_SLEEP);
                buffer_pos = 0;
                continue;
            }
            buffer_pos = 0;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // Gemini Live receives microphone audio continuously while the speaker is idle.
        // 1280 bytes = 640 samples = 40 ms at 16 kHz mono PCM16. A 40 ms
        // command halves JSON/Base64/WebSocket call frequency versus 20 ms
        // frames while keeping microphone capture independent of network TX.
        // Gemini handles speech start/end detection server-side.
        constexpr size_t GEMINI_MIC_CHUNK_BYTES = 1280;
        while (buffer_pos >= GEMINI_MIC_CHUNK_BYTES) {
            if (!audio_hal_is_mic_enabled()) { buffer_pos = 0; break; }
            /* Keep PCM buffered until TX confirms enqueue. If the queue is full,
             * retry on the next loop instead of consuming and losing this frame. */
            if (!websocket_send_audio_data(audio_buffer, GEMINI_MIC_CHUNK_BYTES)) {
                consecutive_tx_enqueue_failures++;
                const int64_t now_us = esp_timer_get_time();
                if (tx_backpressure_start_us == 0) tx_backpressure_start_us = now_us;
                if (now_us - tx_backpressure_start_us >= TX_BACKPRESSURE_ABORT_US) {
                    /* Sustained enqueue backpressure: drop buffered microphone
                     * audio, reset the producer-side buffer, but keep the
                     * current WebSocket and Gemini session alive. */
                    ESP_LOGW(TAG, "TX backpressure > 2 detik; audio tertahan dibuang, sesi Gemini tetap aktif");
                    tx_backpressure_start_us = 0;
                    consecutive_tx_enqueue_failures = 0;
                    mic_buffer_lost_bytes += buffer_pos;
                    buffer_pos = 0;
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

            size_t remainder = buffer_pos - GEMINI_MIC_CHUNK_BYTES;
            if (remainder > 0) memmove(audio_buffer, audio_buffer + GEMINI_MIC_CHUNK_BYTES, remainder);
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

    speaker_id_init();

    // Servo init is isolated: failure must not prevent audio/Gemini startup.
    if (!servo_head_init()) ESP_LOGE(TAG, "Servo init gagal; sistem AI tetap dilanjutkan");

    // WebConfig remains an isolated boot-time configuration layer.
    if (web_config_is_needed()) {
        led_wifi_config_mode = true;
        led_boot_phase = false;
        display_status("Config Mode");
        web_config_start();
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    led_boot_phase = false;

    // Recover/initialize persistent memory before starting OLED animation tasks.
    // SPIFFS formatting may temporarily stall flash/cache execution; keep OLED2
    // animation tasks unstarted until storage initialization has completed.
    if (!ai_memory_init()) ESP_LOGW(TAG, "Memori lokal belum siap; fitur catatan akan gagal sampai penyimpanan pulih");

    oled_init(); face_animation_start(); face_set_state(FACE_SLEEP); display_status("Booting...");
    audio_hal_init();
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0)
        speaker_capture_pcm = (int16_t *)heap_caps_malloc(
            SPEAKER_CAPTURE_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!speaker_capture_pcm)
        speaker_capture_pcm = (int16_t *)heap_caps_malloc(
            SPEAKER_CAPTURE_SAMPLES * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!speaker_capture_pcm)
        ESP_LOGE("SPEAKER_ID", "Tidak dapat mengalokasikan buffer audio; fitur speaker ID nonaktif");
    else
        ESP_LOGI("SPEAKER_ID", "Siap. Pendaftaran hanya melalui perintah eksplisit Gemini.");
    audio_i2s_test_tone();
    if (!wakeword_init()) { ESP_LOGE(TAG, "WakeNet init gagal. Sistem tetap bisa dimulai dengan tombol BOOT."); display_status("ERROR: WakeNet gagal"); } else { display_status("Menunggu wakeword"); }
    // Initialize AFE before a Gemini session starts so model loading does not stall live audio.
    audio_hal_afe_init();
    // Memory storage was initialized before OLED/audio tasks above.
    gpio_set_direction(BOOT_BUTTON_GPIO, GPIO_MODE_INPUT); gpio_set_pull_mode(BOOT_BUTTON_GPIO, GPIO_PULLUP_ONLY); ESP_LOGI(TAG, "Tombol boot siap di GPIO0");
    uart_control_init();
    if (!touch_emotion_start()) ESP_LOGE(TAG, "Sensor sentuh gagal dimulai; fitur sentuh dinonaktifkan");

    display_status("Menghubungkan WiFi...");
    wifi_init_sta();
    if (!wifi_wait_for_connection(15000)) { ESP_LOGE(TAG, "Wi-Fi tidak mendapatkan IP."); display_status("WiFi Gagal!"); face_set_state(FACE_SLEEP); while (1) vTaskDelay(pdMS_TO_TICKS(1000)); }

    esp_wifi_set_ps(WIFI_PS_NONE); ESP_LOGI(TAG, "WiFi power save dimatikan"); ESP_LOGI(TAG, "WIFI READY - lanjut ke NTP"); sync_sntp_time(); ESP_LOGI(TAG, "Menunggu 1 detik..."); vTaskDelay(pdMS_TO_TICKS(1000));
    debug_network_path(); vTaskDelay(pdMS_TO_TICKS(1000)); face_set_state(FACE_SLEEP); display_status("Sistem siap. Katakan Hi, ESP...");

    BaseType_t task_result = xTaskCreate(audio_task, "audio_task", 10240, NULL, 5, NULL); if (task_result != pdPASS) ESP_LOGE(TAG, "Gagal membuat audio_task!"); else ESP_LOGI(TAG, "audio_task berhasil dimulai.");
    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}
