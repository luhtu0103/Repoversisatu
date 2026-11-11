#include "oled2_face.h"

#include "display.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include <netdb.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "OLED2_TEXT";
static i2c_master_bus_handle_t bus2 = nullptr;
static esp_lcd_panel_io_handle_t io2 = nullptr;
static esp_lcd_panel_handle_t panel2 = nullptr;
static bool ready2 = false;
static SemaphoreHandle_t draw_mutex = nullptr;
static uint8_t frame[OLED_WIDTH * OLED_HEIGHT / 8];
static char user_text[256] = {};
static char gemini_text[256] = {};
static char network_status[32] = "RSSI:-- PING:--";
static char device_status[64] = "Menunggu wakeword";
static size_t device_status_scroll_offset = 0;
static size_t user_scroll_offset = 0;
static size_t gemini_scroll_offset = 0;
static volatile uint8_t mic_audio_level = 0;
static volatile uint8_t speaker_audio_level = 0;

static const uint8_t letters[26][5] = {
    {14,17,31,17,17},{30,17,30,17,30},{15,16,16,16,15},
    {30,17,17,17,30},{31,16,30,16,31},{31,16,30,16,16},
    {15,16,23,17,15},{17,17,31,17,17},{31,4,4,4,31},
    {7,2,2,18,12},{17,18,28,18,17},{16,16,16,16,31},
    {17,27,21,17,17},{17,25,21,19,17},{14,17,17,17,14},
    {30,17,30,16,16},{14,17,21,19,15},{30,17,30,18,17},
    {15,16,14,1,30},{31,4,4,4,4},{17,17,17,17,14},
    {17,17,17,10,4},{17,17,21,27,17},{17,10,4,10,17},
    {17,10,4,4,4},{31,2,4,8,31}
};
static const uint8_t digits[10][5] = {
    {14,17,19,21,14},{4,12,4,4,14},{14,1,6,8,31},
    {30,1,6,1,30},{18,18,31,2,2},{31,16,30,1,30},
    {14,16,30,17,14},{31,1,2,4,4},{14,17,14,17,14},
    {14,17,15,1,14}
};

static void pixel(int x, int y, bool on = true) {
    if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) return;
    uint8_t &b = frame[x + (y >> 3) * OLED_WIDTH];
    const uint8_t mask = static_cast<uint8_t>(1U << (y & 7));
    if (on) b |= mask;
    else b &= static_cast<uint8_t>(~mask);
}

static uint8_t glyph_row(char c, int row) {
    c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    if (c >= 'A' && c <= 'Z') return letters[c - 'A'][row];
    if (c >= '0' && c <= '9') return digits[c - '0'][row];
    switch (c) {
        case '-': return row == 2 ? 14 : 0;
        case '.': return row == 4 ? 4 : 0;
        case ',': return row == 4 ? 6 : 0;
        case '!': return row < 4 ? 4 : 0;
        case ':': return (row == 1 || row == 3) ? 4 : 0;
        case '?': return row == 0 ? 14 : row == 1 ? 1 : row == 2 ? 6 : row == 4 ? 4 : 0;
        case '/': return static_cast<uint8_t>(1U << (4 - row));
        case '(': return row == 0 || row == 4 ? 2 : 4;
        case ')': return row == 0 || row == 4 ? 8 : 4;
        case ' ': return 0;
        default: return 0;
    }
}

static void draw_char(int x, int y, char c) {
    for (int row = 0; row < 5; ++row) {
        const uint8_t bits = glyph_row(c, row);
        for (int col = 0; col < 5; ++col) {
            if (bits & (1U << (4 - col))) pixel(x + col, y + row);
        }
    }
}

static void draw_line(int x, int y, const char *text, size_t len) {
    for (size_t i = 0; i < len; ++i) draw_char(x + static_cast<int>(i) * 6, y, text[i]);
}

static void draw_scrolling_text(const char *text, int x, int y, size_t offset, size_t visible_chars) {
    // Keep the label visible; show no placeholder when the transcript is empty.
    if (!text || !text[0]) return;

    const size_t len = strlen(text);
    if (len <= visible_chars) {
        draw_line(x, y, text, len);
        return;
    }

    const size_t cycle_len = len + 3;
    char window[22] = {};
    if (visible_chars >= sizeof(window)) visible_chars = sizeof(window) - 1;
    for (size_t i = 0; i < visible_chars; ++i) {
        const size_t pos = (offset + i) % cycle_len;
        window[i] = pos < len ? text[pos] : ' ';
    }
    draw_line(x, y, window, visible_chars);
}

static void draw_audio_equalizer() {
    // 20 vertical bars in the 14-pixel area below the Gemini transcript.
    // Level is sourced from live PCM, not a random/timer animation.
    const uint8_t mic = mic_audio_level;
    const uint8_t speaker = speaker_audio_level;
    uint8_t level = mic > speaker ? mic : speaker;
    for (int i = 0; i < 20; ++i) {
        // Small deterministic variation gives the bars an equalizer shape while
        // keeping every bar tied to the measured audio amplitude.
        int shaped = (int)level * (65 + ((i * 17 + 11) % 36)) / 100;
        int height = shaped * 12 / 255;
        if (height < 1 && level > 5) height = 1;
        if (height > 12) height = 12;
        const int x = 4 + i * 6;
        for (int y = 63 - height + 1; y <= 63; ++y) {
            for (int w = 0; w < 4; ++w) pixel(x + w, y);
        }
    }
}

static void draw_wita_clock() {
    // Use UTC + 8 directly so this OLED clock does not change the global TZ
    // used by other parts of the firmware.
    const time_t now = time(nullptr);
    if (now < 1704067200) { // 2024-01-01: clock likely has not been synchronized
        draw_line(4, 44, "WAKTU BELUM SYNC", 16);
        return;
    }

    const time_t wita_time = now + (8 * 60 * 60);
    struct tm wita = {};
    if (!gmtime_r(&wita_time, &wita)) {
        draw_line(4, 44, "WAKTU BELUM SYNC", 16);
        return;
    }

    static const char *day_names[] = {"MIN", "SEN", "SEL", "RAB", "KAM", "JUM", "SAB"};
    char clock_text[40] = {};
    snprintf(clock_text, sizeof(clock_text), "%s %02d-%02d-%04d %02d:%02d",
             day_names[wita.tm_wday], wita.tm_mday, wita.tm_mon + 1,
             wita.tm_year + 1900, wita.tm_hour, wita.tm_min);
    draw_line(4, 44, clock_text, strlen(clock_text));
}

static void render_text_locked() {
    memset(frame, 0, sizeof(frame));

    // Top row: RSSI left, PING right.
    const char *ping = strstr(network_status, "PING:");
    if (ping) {
        char rssi_text[16] = {};
        size_t rssi_len = static_cast<size_t>(ping - network_status);
        while (rssi_len > 0 && network_status[rssi_len - 1] == ' ') --rssi_len;
        if (rssi_len >= sizeof(rssi_text)) rssi_len = sizeof(rssi_text) - 1;
        memcpy(rssi_text, network_status, rssi_len);
        draw_line(2, 0, rssi_text, strlen(rssi_text));
        const size_t ping_len = strlen(ping);
        const int ping_x = OLED_WIDTH - 2 - static_cast<int>(ping_len * 6);
        draw_line(ping_x, 0, ping, ping_len);
    } else {
        draw_line(2, 0, network_status, strlen(network_status));
    }

    // Compact transcript rows. Empty transcripts remain blank after their labels.
    draw_line(2, 10, "USER:", 5);
    draw_scrolling_text(user_text, 38, 10, user_scroll_offset, 14);
    draw_line(2, 23, "AI:", 3);
    draw_scrolling_text(gemini_text, 26, 23, gemini_scroll_offset, 16);

    // Device state is displayed below the AI transcript; long states scroll horizontally.
    draw_line(2, 36, "STATUS:", 7);
    draw_scrolling_text(device_status, 50, 36, device_status_scroll_offset, 13);

    // WITA clock fits between STATUS (y=36) and the equalizer (y=52..63).
    draw_wita_clock();

    // Keep the live audio equalizer at the bottom of the display.
    draw_audio_equalizer();
    if (panel2) {
        const esp_err_t err = esp_lcd_panel_draw_bitmap(panel2, 0, 0, OLED_WIDTH, OLED_HEIGHT, frame);
        if (err != ESP_OK) ESP_LOGW(TAG, "OLED2 text draw gagal: %s", esp_err_to_name(err));
    }
}

static void render_text() {
    if (!ready2 || !draw_mutex) return;
    if (xSemaphoreTake(draw_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return;
    render_text_locked();
    xSemaphoreGive(draw_mutex);
}

static void copy_text(char *dst, size_t size, const char *src) {
    if (!src) src = "";
    size_t out = 0;
    for (size_t i = 0; src[i] && out + 1 < size; ++i) {
        unsigned char c = static_cast<unsigned char>(src[i]);
        if (c >= 0x20 && c <= 0x7E) dst[out++] = static_cast<char>(c);
    }
    dst[out] = '\0';
}

static void append_text(char *dst, size_t size, const char *src) {
    if (!src || !src[0] || size < 2) return;
    char incoming[256] = {};
    copy_text(incoming, sizeof(incoming), src);
    size_t add_len = strlen(incoming);
    if (add_len == 0) return;
    const size_t cap = size - 1;
    size_t old_len = strlen(dst);
    size_t sep = old_len ? 1 : 0;

    // Keep a rolling tail instead of freezing when the transcript buffer fills.
    while (old_len + sep + add_len > cap && old_len > 0) {
        for (size_t i = 0; i < old_len; ++i) dst[i] = dst[i + 1];
        --old_len;
        if (old_len > 0 && dst[0] == ' ') {
            for (size_t i = 0; i < old_len; ++i) dst[i] = dst[i + 1];
            --old_len;
        }
        dst[old_len] = '\0';
        sep = old_len ? 1 : 0;
    }
    if (add_len > cap - old_len - sep) {
        size_t skip = add_len - (cap - old_len - sep);
        for (size_t i = 0; i + skip <= add_len; ++i) incoming[i] = incoming[i + skip];
        add_len -= skip;
        incoming[add_len] = '\0';
    }
    if (sep) dst[old_len++] = ' ';
    for (size_t i = 0; i < add_len; ++i) dst[old_len + i] = incoming[i];
    dst[old_len + add_len] = '\0';
}

static void update_text_safely(char *dst, size_t size, const char *text, bool append) {
    // Keep transcript writes and OLED rendering under the same mutex. The
    // network-status task redraws the screen periodically, so unsynchronized
    // writes here could race with a frame render and leave stale/partial text.
    if (draw_mutex && ready2) {
        if (xSemaphoreTake(draw_mutex, pdMS_TO_TICKS(250)) != pdTRUE) {
            ESP_LOGW(TAG, "Mutex timeout saat memperbarui teks OLED2");
            return;
        }
        if (append) {
            append_text(dst, size, text);
        } else {
            copy_text(dst, size, text);
            // A replaced/interim transcript starts at its beginning, not at
            // an offset left over from the previous utterance.
            if (dst == user_text) user_scroll_offset = 0;
            else if (dst == gemini_text) gemini_scroll_offset = 0;
        }
        render_text_locked();
        xSemaphoreGive(draw_mutex);
        ESP_LOGD(TAG, "Teks OLED2 diperbarui: %s", dst == user_text ? "USER" : "GEMINI");
        return;
    }

    // Preserve text received before panel initialization; init renders it.
    if (append) {
        append_text(dst, size, text);
    } else {
        copy_text(dst, size, text);
        if (dst == user_text) user_scroll_offset = 0;
        else if (dst == gemini_text) gemini_scroll_offset = 0;
    }
}

extern "C" void oled2_face_set_user_text(const char *text) {
    update_text_safely(user_text, sizeof(user_text), text, false);
}

extern "C" void oled2_face_set_gemini_text(const char *text) {
    update_text_safely(gemini_text, sizeof(gemini_text), text, false);
}

extern "C" void oled2_face_append_user_text(const char *text) {
    update_text_safely(user_text, sizeof(user_text), text, true);
}

extern "C" void oled2_face_append_gemini_text(const char *text) {
    update_text_safely(gemini_text, sizeof(gemini_text), text, true);
}

extern "C" void oled2_face_set_status(const char *status) {
    if (!status || !status[0]) status = "Menunggu wakeword";
    if (draw_mutex && ready2) {
        if (xSemaphoreTake(draw_mutex, pdMS_TO_TICKS(250)) != pdTRUE) return;
        copy_text(device_status, sizeof(device_status), status);
        device_status_scroll_offset = 0;
        render_text_locked();
        xSemaphoreGive(draw_mutex);
    } else {
        copy_text(device_status, sizeof(device_status), status);
        device_status_scroll_offset = 0;
    }
}

extern "C" void oled2_face_set_mic_level(uint8_t level) {
    mic_audio_level = level;
}

extern "C" void oled2_face_set_speaker_level(uint8_t level) {
    speaker_audio_level = level;
}


static int measure_gemini_tcp_ping_ms() {
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *result = nullptr;
    if (getaddrinfo("generativelanguage.googleapis.com", "443", &hints, &result) != 0 || !result)
        return -1;

    int elapsed_ms = -1;
    for (struct addrinfo *it = result; it; it = it->ai_next) {
        int sock = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (sock < 0) continue;
        timeval timeout = {};
        timeout.tv_sec = 2;
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        const int64_t start = esp_timer_get_time();
        if (connect(sock, it->ai_addr, it->ai_addrlen) == 0) {
            elapsed_ms = static_cast<int>((esp_timer_get_time() - start) / 1000);
            close(sock);
            break;
        }
        close(sock);
    }
    freeaddrinfo(result);
    return elapsed_ms;
}

static void transcript_scroll_task(void *arg) {
    (void)arg;
    unsigned refresh_count = 0;
    unsigned gemini_scroll_ms = 0;
    while (true) {
        if (draw_mutex && ready2 &&
            xSemaphoreTake(draw_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            // The previous 300 ms/character pace made the transcript lag behind
            // synthesized speech. Advance roughly every 120 ms while speaker PCM
            // is active, with a lower gate so quiet syllables still move the text.
            if ((refresh_count % 2U) == 0U) {
                if (strlen(user_text) > 14)
                    user_scroll_offset = (user_scroll_offset + 1) % (strlen(user_text) + 3);
                else
                    user_scroll_offset = 0;

                if (speaker_audio_level > 3) {
                    gemini_scroll_ms += 180;
                    if (gemini_scroll_ms >= 120) {
                        gemini_scroll_ms = 0;
                        if (strlen(gemini_text) > 16)
                            gemini_scroll_offset = (gemini_scroll_offset + 1) % (strlen(gemini_text) + 3);
                        else
                            gemini_scroll_offset = 0;
                    }
                } else {
                    // Avoid accumulating a stale delay during silence.
                    gemini_scroll_ms = 0;
                }

                if (strlen(device_status) > 13)
                    device_status_scroll_offset = (device_status_scroll_offset + 1) % (strlen(device_status) + 3);
                else
                    device_status_scroll_offset = 0;
            }
            render_text_locked();
            xSemaphoreGive(draw_mutex);
        }
        ++refresh_count;
        vTaskDelay(pdMS_TO_TICKS(90));
    }
}

static void network_status_task(void *arg) {
    (void)arg;
    int ping_ms = -1;
    TickType_t last_ping = 0;
    const TickType_t ping_period = pdMS_TO_TICKS(15000);
    while (true) {
        wifi_ap_record_t ap = {};
        const bool wifi_ok = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
        if (xTaskGetTickCount() - last_ping >= ping_period) {
            ping_ms = wifi_ok ? measure_gemini_tcp_ping_ms() : -1;
            last_ping = xTaskGetTickCount();
        }

        char status[32] = {};
        if (!wifi_ok) {
            snprintf(status, sizeof(status), "RSSI:-- PING:--");
        } else if (ping_ms < 0) {
            snprintf(status, sizeof(status), "RSSI:%d PING:FAIL", (int)ap.rssi);
        } else {
            snprintf(status, sizeof(status), "RSSI:%d PING:%dMS", (int)ap.rssi, ping_ms);
        }

        if (draw_mutex && xSemaphoreTake(draw_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            copy_text(network_status, sizeof(network_status), status);
            render_text_locked();
            xSemaphoreGive(draw_mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

extern "C" void oled2_face_init(void) {
    if (ready2) return;

    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = GPIO_NUM_41,
        .scl_io_num = GPIO_NUM_42,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {.enable_internal_pullup = 1, .allow_pd = 0},
    };
    esp_err_t err = i2c_new_master_bus(&bus_config, &bus2);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C1 OLED2 GPIO41/42 gagal: %s", esp_err_to_name(err));
        return;
    }

    esp_lcd_panel_io_i2c_config_t io_config = {
        .dev_addr = OLED_I2C_ADDR,
        .scl_speed_hz = 400 * 1000,
        .control_phase_bytes = 1,
        .dc_bit_offset = 6,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .on_color_trans_done = nullptr,
        .user_ctx = nullptr,
        .flags = {.dc_low_on_data = 0, .disable_control_phase = 0},
    };
    err = esp_lcd_new_panel_io_i2c(bus2, &io_config, &io2);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OLED2 panel IO gagal: %s", esp_err_to_name(err));
        return;
    }

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = GPIO_NUM_NC;
    panel_config.bits_per_pixel = 1;
    esp_lcd_panel_ssd1306_config_t ssd1306_config = {.height = OLED_HEIGHT};
    panel_config.vendor_config = &ssd1306_config;
    err = esp_lcd_new_panel_ssd1306(io2, &panel_config, &panel2);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Driver SSD1306 OLED2 gagal: %s", esp_err_to_name(err));
        return;
    }
    if ((err = esp_lcd_panel_reset(panel2)) != ESP_OK ||
        (err = esp_lcd_panel_init(panel2)) != ESP_OK) {
        ESP_LOGE(TAG, "Inisialisasi panel OLED2 gagal: %s", esp_err_to_name(err));
        return;
    }
    if ((err = esp_lcd_panel_invert_color(panel2, false)) != ESP_OK ||
        (err = esp_lcd_panel_mirror(panel2, true, true)) != ESP_OK ||
        (err = esp_lcd_panel_disp_on_off(panel2, true)) != ESP_OK) {
        ESP_LOGE(TAG, "Konfigurasi panel OLED2 gagal: %s", esp_err_to_name(err));
        return;
    }

    draw_mutex = xSemaphoreCreateMutex();
    if (!draw_mutex) {
        ESP_LOGE(TAG, "Gagal membuat mutex OLED2");
        return;
    }
    ready2 = true;
    render_text();
    if (xTaskCreate(transcript_scroll_task, "oled2_text_scroll", 3072, nullptr, 2, nullptr) != pdPASS) {
        ESP_LOGW(TAG, "Task scroll teks OLED2 gagal dibuat");
    }
    if (xTaskCreate(network_status_task, "oled2_net_status", 4096, nullptr, 2, nullptr) != pdPASS) {
        ESP_LOGW(TAG, "Task status jaringan OLED2 gagal dibuat");
    }
    ESP_LOGI(TAG, "OLED2 layout: RSSI/PING atas, USER dan AI sejajar teks, EQ bawah; I2C1 SDA=41 SCL=42 addr=0x%02X", OLED_I2C_ADDR);
}

