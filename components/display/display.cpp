#include "display.h"
#include "oled2_face.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include <string.h>
#include <math.h>
#include "esp_attr.h"
#include "esp_timer.h"
#include <stdio.h>

static const char *TAG = "DISPLAY";
static i2c_master_bus_handle_t i2c_bus = NULL;
static esp_lcd_panel_io_handle_t panel_io = NULL;
static esp_lcd_panel_handle_t panel = NULL;
static SemaphoreHandle_t oled_mutex = NULL;
static bool oled_ready = false;
static face_state_t current_face_state = FACE_SLEEP;
static uint8_t speech_level_smoothed = 0;
static portMUX_TYPE speech_level_mux = portMUX_INITIALIZER_UNLOCKED;
static EXT_RAM_BSS_ATTR uint8_t face_buffer[OLED_WIDTH * OLED_HEIGHT / 8];

static char user_scroll_text[256] = {0};
static char gemini_scroll_text[256] = {0};
static uint16_t user_scroll_offset = 0;
static uint16_t gemini_scroll_offset = 0;
static portMUX_TYPE scroll_text_mux = portMUX_INITIALIZER_UNLOCKED;

static void draw_buffer_locked(const uint8_t *buffer)
{
    if (!panel || !buffer) return;
    esp_err_t err = esp_lcd_panel_draw_bitmap(panel, 0, 0, OLED_WIDTH, OLED_HEIGHT, buffer);
    if (err != ESP_OK) ESP_LOGW(TAG, "OLED draw gagal: %s", esp_err_to_name(err));
}

static void draw_rssi_char(int x, int y, char c)
{
    static const uint8_t glyphs[][5] = {
        {0x00,0x00,0x1F,0x00,0x00},
        {0x1E,0x11,0x11,0x11,0x1E},
        {0x00,0x12,0x1F,0x10,0x00},
        {0x12,0x19,0x15,0x13,0x12},
        {0x11,0x15,0x15,0x15,0x0A},
        {0x07,0x04,0x04,0x1F,0x04},
        {0x17,0x15,0x15,0x15,0x09},
        {0x0E,0x15,0x15,0x15,0x08},
        {0x01,0x01,0x19,0x05,0x03},
        {0x0A,0x15,0x15,0x15,0x0A},
        {0x02,0x15,0x15,0x15,0x0E},
    };

    int index = (c == '-') ? 0 : (c - '0' + 1);
    if (index < 0 || index >= (int)(sizeof(glyphs) / sizeof(glyphs[0]))) return;

    for (int col = 0; col < 5; ++col) {
        uint8_t bits = glyphs[index][col];
        for (int row = 0; row < 5; ++row) {
            if (bits & (1U << row)) {
                int px = x + col;
                int py = y + row;
                if (px >= 0 && px < OLED_WIDTH && py >= 0 && py < OLED_HEIGHT) {
                    uint8_t &b = face_buffer[px + (py >> 3) * OLED_WIDTH];
                    b |= (uint8_t)(1U << (py & 7));
                }
            }
        }
    }
}

static int draw_rssi(void)
{
    wifi_ap_record_t ap_info = {};
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) return 0;

    int rssi = ap_info.rssi;
    if (rssi > 0) rssi = 0;
    if (rssi < -99) rssi = -99;

    char text[5];
    snprintf(text, sizeof(text), "%d", rssi);

    const int char_width = 6;
    int x = 0;
    const int y = OLED_HEIGHT - 5;

    for (size_t i = 0; text[i] != '\0'; ++i) {
        draw_rssi_char(x, y, text[i]);
        x += char_width;
    }

    return x;
}

void oled_init(void)
{
    // OLED2 uses its own I2C controller. Initialize it independently so an
    // OLED1/I2C0 failure does not prevent the scrolling transcript display.
    oled2_face_init();
    if (oled_ready) return;

    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = OLED_SDA_PIN,
        .scl_io_num = OLED_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {.enable_internal_pullup = 1, .allow_pd = 0},
    };

    // oled_init() can be reached repeatedly while initialization is incomplete.
    // Reuse any resources already acquired instead of creating I2C_NUM_0 again.
    esp_err_t err = ESP_OK;
    if (i2c_bus == NULL) {
        err = i2c_new_master_bus(&bus_config, &i2c_bus);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Gagal membuat I2C OLED (SDA=%d SCL=%d): %s",
                     OLED_SDA_PIN, OLED_SCL_PIN, esp_err_to_name(err));
            return;
        }
    }

    esp_lcd_panel_io_i2c_config_t io_config = {
        .dev_addr = OLED_I2C_ADDR,
        .scl_speed_hz = 400 * 1000,
        .control_phase_bytes = 1,
        .dc_bit_offset = 6,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .on_color_trans_done = NULL,
        .user_ctx = NULL,
        .flags = {.dc_low_on_data = 0, .disable_control_phase = 0},
    };

    if (panel_io == NULL) {
        err = esp_lcd_new_panel_io_i2c(i2c_bus, &io_config, &panel_io);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Gagal membuat panel I2C: %s", esp_err_to_name(err));
            return;
        }
    }

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = GPIO_NUM_NC;
    panel_config.bits_per_pixel = 1;
    esp_lcd_panel_ssd1306_config_t ssd1306_config = {.height = OLED_HEIGHT};
    panel_config.vendor_config = &ssd1306_config;

    if (panel == NULL) {
        ESP_LOGI(TAG, "Install SSD1306 driver");
        err = esp_lcd_new_panel_ssd1306(panel_io, &panel_config, &panel);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Gagal install SSD1306: %s", esp_err_to_name(err));
            return;
        }
    }

    if ((err = esp_lcd_panel_reset(panel)) != ESP_OK) {
        ESP_LOGE(TAG, "OLED reset gagal: %s", esp_err_to_name(err));
        return;
    }
    if ((err = esp_lcd_panel_init(panel)) != ESP_OK) {
        ESP_LOGE(TAG, "OLED init gagal: %s", esp_err_to_name(err));
        return;
    }

    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, false));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, true, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    oled_mutex = xSemaphoreCreateMutex();
    if (!oled_mutex) {
        ESP_LOGE(TAG, "Gagal membuat mutex OLED");
        return;
    }

    oled_ready = true;
    memset(face_buffer, 0, sizeof(face_buffer));
    display_render_buffer(face_buffer);
    ESP_LOGI(TAG, "XiaoZhi SSD1306 128x64 siap: SDA=%d SCL=%d ADDR=0x%02X SPEED=400kHz MIRROR=XY",
             OLED_SDA_PIN, OLED_SCL_PIN, OLED_I2C_ADDR);
}

void display_render_buffer(const uint8_t *buffer)
{
    if (!buffer) return;
    if (!oled_ready) oled_init();
    if (!oled_ready || !oled_mutex) return;
    if (xSemaphoreTake(oled_mutex, pdMS_TO_TICKS(250)) != pdTRUE) {
        ESP_LOGW(TAG, "OLED mutex timeout");
        return;
    }
    draw_buffer_locked(buffer);
    xSemaphoreGive(oled_mutex);
}

void display_status(const char *text)
{
    if (!oled_ready) oled_init();
    ESP_LOGI(TAG, "[OLED STATUS]: %s", text ? text : "(null)");

    // Only forward the requested live device states to OLED2; ignore boot/NTP chatter.
    if (!text) return;
    if (strcmp(text, "Menunggu wakeword") == 0 ||
        strcmp(text, "Wakeword terdeteksi") == 0 ||
        strcmp(text, "AI terhubung") == 0 ||
        strcmp(text, "AI terputus") == 0 ||
        strncmp(text, "ERROR:", 6) == 0) {
        oled2_face_set_status(text);
    }
}

static void set_scroll_text(char *dst, size_t dst_size, const char *text, uint16_t &offset)
{
    if (!dst || dst_size == 0) return;

    dst[0] = '\0';
    offset = 0;
    if (!text) return;

    size_t out = 0;
    for (size_t i = 0; text[i] != '\0' && out + 1 < dst_size; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c >= 0x20 && c <= 0x7E)
            dst[out++] = (char)c;
    }
    dst[out] = '\0';
}

static void append_scroll_text(char *dst, size_t dst_size, const char *text)
{
    if (!dst || dst_size == 0 || !text || !text[0]) return;

    size_t out = strlen(dst);
    if (out > 0 && out + 1 < dst_size) {
        dst[out++] = ' ';
        dst[out] = '\0';
    }

    for (size_t i = 0; text[i] != '\0' && out + 1 < dst_size; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c >= 0x20 && c <= 0x7E)
            dst[out++] = (char)c;
    }
    dst[out] = '\0';
}

void display_set_user_text(const char *text)
{
    portENTER_CRITICAL(&scroll_text_mux);
    set_scroll_text(user_scroll_text, sizeof(user_scroll_text), text, user_scroll_offset);
    portEXIT_CRITICAL(&scroll_text_mux);
    oled2_face_set_user_text(text);
}

void display_set_gemini_text(const char *text)
{
    portENTER_CRITICAL(&scroll_text_mux);
    set_scroll_text(gemini_scroll_text, sizeof(gemini_scroll_text), text, gemini_scroll_offset);
    portEXIT_CRITICAL(&scroll_text_mux);
    oled2_face_set_gemini_text(text);
}

void display_append_user_text(const char *text)
{
    portENTER_CRITICAL(&scroll_text_mux);
    append_scroll_text(user_scroll_text, sizeof(user_scroll_text), text);
    portEXIT_CRITICAL(&scroll_text_mux);
    oled2_face_append_user_text(text);
}

void display_append_gemini_text(const char *text)
{
    portENTER_CRITICAL(&scroll_text_mux);
    append_scroll_text(gemini_scroll_text, sizeof(gemini_scroll_text), text);
    portEXIT_CRITICAL(&scroll_text_mux);
    oled2_face_append_gemini_text(text);
}

static uint8_t text_glyph_row(char c, int row)
{
    static const uint8_t letters[26][5] = {
        {14,17,31,17,17}, {30,17,30,17,30}, {15,16,16,16,15},
        {30,17,17,17,30}, {31,16,30,16,31}, {31,16,30,16,16},
        {15,16,23,17,15}, {17,17,31,17,17}, {31,4,4,4,31},
        {7,2,2,18,12}, {17,18,28,18,17}, {16,16,16,16,31},
        {17,27,21,17,17}, {17,25,21,19,17}, {14,17,17,17,14},
        {30,17,30,16,16}, {14,17,21,19,15}, {30,17,30,18,17},
        {15,16,14,1,30}, {31,4,4,4,4}, {17,17,17,17,14},
        {17,17,17,10,4}, {17,17,21,27,17}, {17,10,4,10,17},
        {17,10,4,4,4}, {31,2,4,8,31}
    };
    static const uint8_t digits[10][5] = {
        {14,17,19,21,14}, {4,12,4,4,14}, {14,1,6,8,31},
        {30,1,6,1,30}, {18,18,31,2,2}, {31,16,30,1,30},
        {14,16,30,17,14}, {31,1,2,4,4}, {14,17,14,17,14},
        {14,17,15,1,14}
    };

    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z') return letters[c - 'A'][row];
    if (c >= '0' && c <= '9') return digits[c - '0'][row];

    switch (c) {
        case '-': return row == 2 ? 14 : 0;
        case '.': return row == 4 ? 4 : 0;
        case ',': return row == 4 ? 6 : 0;
        case '!': return row < 4 ? 4 : 0;
        case ':': return (row == 1 || row == 3) ? 4 : 0;
        case '?': return row == 0 ? 14 : row == 1 ? 1 : row == 2 ? 6 : row == 4 ? 4 : 0;
        case '/': return (uint8_t)(1U << (4 - row));
        case ' ': return 0;
        default: return 0;
    }
}

static void pixel(int x, int y, bool on = true)
{
    if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) return;
    uint8_t &b = face_buffer[x + (y >> 3) * OLED_WIDTH];
    uint8_t m = (uint8_t)(1U << (y & 7));
    if (on) b |= m;
    else b &= (uint8_t)~m;
}

static void draw_text_char(int x, int y, char c)
{
    for (int row = 0; row < 5; ++row) {
        uint8_t bits = text_glyph_row(c, row);
        for (int col = 0; col < 5; ++col) {
            if (bits & (1U << (4 - col))) pixel(x + col, y + row, true);
        }
    }
}
static void draw_scrolling_text(const char *text, uint16_t &offset, int text_x)
{
    if (!text || !text[0]) return;

    constexpr int TEXT_Y = OLED_HEIGHT - 5;
    constexpr int CHAR_WIDTH = 6;
    constexpr int TEXT_RIGHT = OLED_WIDTH - 1;
    constexpr int GAP_PX = 12;

    const size_t len = strlen(text);
    const int text_px = (int)len * CHAR_WIDTH;
    const int visible_width = TEXT_RIGHT - text_x + 1;

    if (visible_width <= 5) return;

    // Teks pendek: tampil penuh dan tidak berjalan.
    if (text_px <= visible_width) {
        for (size_t i = 0; i < len; ++i) {
            draw_text_char(
                text_x + (int)i * CHAR_WIDTH,
                TEXT_Y,
                text[i]
            );
        }

        offset = 0;
        return;
    }

    /*
     * Marquee kontinu:
     *
     * 1. Teks masuk dari kanan.
     * 2. Bergerak ke kiri.
     * 3. Keluar sepenuhnya dari kiri.
     * 4. GAP_PX kemudian teks berikutnya masuk.
     */

    const int cycle_px = text_px + GAP_PX;

    // Posisi teks dihitung relatif terhadap batas kanan.
    int pos = TEXT_RIGHT + 1 - (int)offset;

    // Salinan pertama.
    for (size_t i = 0; i < len; ++i) {
        const int x = pos + (int)i * CHAR_WIDTH;

        if (x + 5 >= text_x && x <= TEXT_RIGHT) {
            draw_text_char(x, TEXT_Y, text[i]);
        }
    }

    // Salinan kedua untuk menjaga loop tetap kontinu.
    pos += cycle_px;

    for (size_t i = 0; i < len; ++i) {
        const int x = pos + (int)i * CHAR_WIDTH;

        if (x + 5 >= text_x && x <= TEXT_RIGHT) {
            draw_text_char(x, TEXT_Y, text[i]);
        }
    }

    // Satu pixel setiap frame pemanggilan.
    offset = (uint16_t)((offset + 1) % cycle_px);
}
static void draw_current_scroll_text(int rssi_end_x)
{
    char text[256] = {0};
    uint16_t offset = 0;
    bool enabled = false;

    portENTER_CRITICAL(&scroll_text_mux);
    if (current_face_state == FACE_THINKING) {
        strncpy(text, user_scroll_text, sizeof(text) - 1);
        offset = user_scroll_offset;
        enabled = true;
    } else if (current_face_state == FACE_SPEAKING) {
        strncpy(text, gemini_scroll_text, sizeof(text) - 1);
        offset = gemini_scroll_offset;
        enabled = true;
    }
    portEXIT_CRITICAL(&scroll_text_mux);

    if (!enabled || !text[0]) return;

    const int text_x = rssi_end_x > 0 ? rssi_end_x + 4 : 4;
    draw_scrolling_text(text, offset, text_x);

    portENTER_CRITICAL(&scroll_text_mux);
    if (current_face_state == FACE_THINKING)
        user_scroll_offset = offset;
    else if (current_face_state == FACE_SPEAKING)
        gemini_scroll_offset = offset;
    portEXIT_CRITICAL(&scroll_text_mux);
}

void face_set_speech_level(uint8_t level)
{
    // Smooth sudden PCM peaks to avoid a jittery mouth while retaining syllables.
    portENTER_CRITICAL(&speech_level_mux);
    speech_level_smoothed = (uint8_t)(((unsigned)speech_level_smoothed * 3U + level) / 4U);
    portEXIT_CRITICAL(&speech_level_mux);
}

uint8_t face_get_speech_level(void)
{
    portENTER_CRITICAL(&speech_level_mux);
    const uint8_t level = speech_level_smoothed;
    portEXIT_CRITICAL(&speech_level_mux);
    return level;
}

void face_set_state(face_state_t state)
{
    // The animation/controller and websocket callbacks run in different tasks.
    // Keep the state and its associated scroll-buffer reset in one critical
    // section so readers never observe a partially updated transition.
    portENTER_CRITICAL(&scroll_text_mux);
    if (current_face_state == state) {
        portEXIT_CRITICAL(&scroll_text_mux);
        return;
    }

    current_face_state = state;
    if (state == FACE_THINKING) {
        user_scroll_text[0] = '\0';
        user_scroll_offset = 0;
    } else if (state == FACE_SPEAKING) {
        gemini_scroll_text[0] = '\0';
        gemini_scroll_offset = 0;
    }
    portEXIT_CRITICAL(&scroll_text_mux);
}

face_state_t face_get_state(void)
{
    portENTER_CRITICAL(&scroll_text_mux);
    face_state_t state = current_face_state;
    portEXIT_CRITICAL(&scroll_text_mux);
    return state;
}

// Private bridge used by face_base.cpp. The framebuffer remains owned by the
// display component; only the animation task should draw into it.
uint8_t *display_framebuffer(void)
{
    return face_buffer;
}

void display_clear_framebuffer(void)
{
    memset(face_buffer, 0, sizeof(face_buffer));
}
