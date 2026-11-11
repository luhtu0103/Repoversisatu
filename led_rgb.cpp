#include "led_rgb.h"

#include <stdint.h>

#include "driver/gpio.h"
#include "led_strip.h"
#include "esp_log.h"

static const char *TAG = "LED_RGB";
static constexpr gpio_num_t LED_GPIO = GPIO_NUM_48;
static led_strip_handle_t strip = nullptr;
static led_rgb_state_t current_state = LED_RGB_OFF;

static void apply_color(uint8_t red, uint8_t green, uint8_t blue)
{
    if (strip == nullptr) return;
    if (led_strip_set_pixel(strip, 0, red, green, blue) != ESP_OK) {
        ESP_LOGW(TAG, "Gagal mengatur warna LED");
        return;
    }
    if (led_strip_refresh(strip) != ESP_OK) ESP_LOGW(TAG, "Gagal refresh LED");
}

extern "C" bool led_rgb_init(void)
{
    if (strip != nullptr) return true;

    led_strip_config_t strip_config = {};
    strip_config.strip_gpio_num = LED_GPIO;
    strip_config.max_leds = 1;
    strip_config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    strip_config.led_model = LED_MODEL_WS2812;
    strip_config.flags.invert_out = false;

    led_strip_rmt_config_t rmt_config = {};
    rmt_config.clk_src = RMT_CLK_SRC_DEFAULT;
    rmt_config.resolution_hz = 10 * 1000 * 1000;
    rmt_config.mem_block_symbols = 64;
    rmt_config.flags.with_dma = false;

    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Inisialisasi WS2812 GPIO%d gagal: %s",
                 (int)LED_GPIO, esp_err_to_name(err));
        strip = nullptr;
        return false;
    }

    current_state = LED_RGB_OFF;
    (void)led_strip_clear(strip);
    ESP_LOGI(TAG, "WS2812 siap pada GPIO%d (1 LED, format GRB)", (int)LED_GPIO);
    return true;
}

extern "C" void led_rgb_set_state(led_rgb_state_t state)
{
    if (strip == nullptr || state == current_state) return;
    current_state = state;

    switch (state) {
        case LED_RGB_BOOT_BLINK:
        case LED_RGB_WIFI_CONFIG:
        case LED_RGB_CONNECTING:
            apply_color(0, 0, 32);       // Biru.
            break;
        case LED_RGB_LISTENING:
            apply_color(20, 0, 0);       // Merah normal: mendengarkan.
            break;
        case LED_RGB_VOICE_DETECTED:
            apply_color(90, 0, 0);        // Merah terang: suara terdeteksi.
            break;
        case LED_RGB_SPEAKING:
            apply_color(0, 32, 0);        // Hijau: AI berbicara.
            break;
        case LED_RGB_OFF:
        default:
            (void)led_strip_clear(strip);
            break;
    }
}
