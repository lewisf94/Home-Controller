/*
 * Step 1 — ILI9341 colour cycle
 *
 * To activate:
 *   1. Update idf_component.yml to add espressif/esp_lcd_ili9341: "^2.0.0"
 *   2. Run: idf.py reconfigure
 *   3. Replace main.c with this file (or rename and update main/CMakeLists.txt)
 *   4. idf.py build && idf.py flash monitor
 *
 * Done when: screen cycles full-screen red -> green -> blue at 1 Hz,
 * no tearing, no garbage, serial shows "Music Controller IDF step 1" once.
 *
 * Mirror note: swap_xy(true) + mirror(true, false) is the standard CYD
 * landscape orientation. If the image is upside-down or mirrored, toggle
 * the second argument of esp_lcd_panel_mirror(). Document the result in
 * docs/PORT-NOTES.md.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "esp_log.h"

#define GPIO_BL     21
#define LCD_HOST    SPI2_HOST
#define GPIO_MOSI   13
#define GPIO_MISO   12
#define GPIO_SCLK   14
#define GPIO_CS     15
#define GPIO_DC      2
#define LCD_H       320
#define LCD_V       240
#define LCD_PIX_CLK (40 * 1000 * 1000)

static const char *TAG = "main";

static void fill(esp_lcd_panel_handle_t panel, uint16_t colour)
{
    uint16_t *line = heap_caps_malloc(LCD_H * sizeof(uint16_t), MALLOC_CAP_DMA);
    for (int x = 0; x < LCD_H; x++) line[x] = colour;
    for (int y = 0; y < LCD_V; y++) {
        esp_lcd_panel_draw_bitmap(panel, 0, y, LCD_H, y + 1, line);
    }
    heap_caps_free(line);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Music Controller IDF step 1");

    gpio_config_t bl = {
        .pin_bit_mask = (1ULL << GPIO_BL),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&bl);
    gpio_set_level(GPIO_BL, 1);

    spi_bus_config_t bus = {
        .mosi_io_num  = GPIO_MOSI,
        .miso_io_num  = GPIO_MISO,
        .sclk_io_num  = GPIO_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H * 40 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num      = GPIO_DC,
        .cs_gpio_num      = GPIO_CS,
        .pclk_hz          = LCD_PIX_CLK,
        .lcd_cmd_bits     = 8,
        .lcd_param_bits   = 8,
        .spi_mode         = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io));

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,
        .rgb_endian     = LCD_RGB_ENDIAN_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io, &panel_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    const uint16_t colours[] = { 0xF800, 0x07E0, 0x001F };
    int i = 0;
    while (1) {
        fill(panel, colours[i]);
        i = (i + 1) % 3;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
