#include "display.h"
#include "oled2_face.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include "esp_attr.h"
#include "esp_timer.h"

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

static portMUX_TYPE state_mux = portMUX_INITIALIZER_UNLOCKED;
// Frame terakhir yang dikirim ke OLED1: frame sama tidak dikirim ulang (hemat I2C).
static uint8_t last_sent[OLED_WIDTH * OLED_HEIGHT / 8];
static bool last_sent_valid = false;

static void draw_buffer_locked(const uint8_t *buffer)
{
    if (!panel || !buffer) return;
    if (last_sent_valid && memcmp(last_sent, buffer, sizeof(last_sent)) == 0) return;
    esp_err_t err = esp_lcd_panel_draw_bitmap(panel, 0, 0, OLED_WIDTH, OLED_HEIGHT, buffer);
    if (err != ESP_OK) {
        last_sent_valid = false;
        ESP_LOGW(TAG, "OLED draw gagal: %s", esp_err_to_name(err));
        return;
    }
    memcpy(last_sent, buffer, sizeof(last_sent));
    last_sent_valid = true;
}

void oled_init(void)
{
    // Claim initialization atomically so concurrent tasks cannot create the
    // I2C/panel resources twice or initialize OLED2 repeatedly.
    static portMUX_TYPE init_mux = portMUX_INITIALIZER_UNLOCKED;
    static bool initializing = false;
    static int64_t next_retry_us = 0;   // setelah gagal, tunggu 10 detik sebelum coba lagi
    static bool oled2_tried = false;

    portENTER_CRITICAL(&init_mux);
    if (oled_ready || initializing ||
        (next_retry_us != 0 && esp_timer_get_time() < next_retry_us)) {
        portEXIT_CRITICAL(&init_mux);
        return;
    }
    initializing = true;
    portEXIT_CRITICAL(&init_mux);

    if (!oled2_tried) {      // OLED2 cukup dicoba sekali
        oled2_tried = true;
        oled2_face_init();
    }

    bool success = false;
    do {
        esp_err_t err = ESP_OK;
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

        if (i2c_bus == NULL) {
            err = i2c_new_master_bus(&bus_config, &i2c_bus);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Gagal membuat I2C OLED (SDA=%d SCL=%d): %s",
                         OLED_SDA_PIN, OLED_SCL_PIN, esp_err_to_name(err));
                break;
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
                break;
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
                break;
            }
        }

        err = esp_lcd_panel_reset(panel);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OLED reset gagal: %s", esp_err_to_name(err));
            break;
        }
        err = esp_lcd_panel_init(panel);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OLED init gagal: %s", esp_err_to_name(err));
            break;
        }

        // Jangan restart robot kalau OLED bermasalah: cukup catat dan berhenti.
        if ((err = esp_lcd_panel_invert_color(panel, false)) != ESP_OK ||
            (err = esp_lcd_panel_mirror(panel, true, true)) != ESP_OK ||
            (err = esp_lcd_panel_disp_on_off(panel, true)) != ESP_OK) {
            ESP_LOGE(TAG, "Konfigurasi OLED gagal: %s", esp_err_to_name(err));
            break;
        }

        if (!oled_mutex) oled_mutex = xSemaphoreCreateMutex();
        if (!oled_mutex) {
            ESP_LOGE(TAG, "Gagal membuat mutex OLED");
            break;
        }

        memset(face_buffer, 0, sizeof(face_buffer));
        last_sent_valid = false;
        success = true;
    } while (false);

    portENTER_CRITICAL(&init_mux);
    oled_ready = success;
    initializing = false;
    next_retry_us = success ? 0 : esp_timer_get_time() + 10000000LL;
    portEXIT_CRITICAL(&init_mux);

    if (!success) return;

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

void display_set_user_text(const char *text)
{
    oled2_face_set_user_text(text);
}

void display_set_gemini_text(const char *text)
{
    oled2_face_set_gemini_text(text);
}

void display_append_user_text(const char *text)
{
    oled2_face_append_user_text(text);
}

void display_append_gemini_text(const char *text)
{
    oled2_face_append_gemini_text(text);
}

void face_set_speech_level(uint8_t level)
{
    // Smooth sudden PCM peaks to avoid a jittery mouth while retaining syllables.
    portENTER_CRITICAL(&speech_level_mux);
    // Level 0 (jeda/selesai bicara) langsung menutup mulut, tidak menunggu penghalusan.
    speech_level_smoothed = (level == 0) ? 0 :
        (uint8_t)(((unsigned)speech_level_smoothed * 3U + level) / 4U);
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
    portENTER_CRITICAL(&state_mux);
    current_face_state = state;
    portEXIT_CRITICAL(&state_mux);
}

face_state_t face_get_state(void)
{
    portENTER_CRITICAL(&state_mux);
    face_state_t state = current_face_state;
    portEXIT_CRITICAL(&state_mux);
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
