/*
 * Step 2 — LVGL init + centred "Hello CYD" label
 *
 * Done when: screen shows white "Hello CYD" text centred on black,
 * stable for at least 30 s, serial shows "Music Controller IDF step 2".
 *
 * Adds esp_lvgl_port on top of the Step 1 ILI9341 init. LVGL runs in
 * its own FreeRTOS task; UI mutations must be wrapped in
 * lvgl_port_lock() / lvgl_port_unlock(). The swap_bytes flag handles
 * the ILI9341 big-endian RGB565 quirk (replaces the manual setSwapBytes
 * pattern from the Arduino build).
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
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

void app_main(void)
{
    ESP_LOGI(TAG, "Music Controller IDF step 2");

    gpio_config_t bl = {
        .pin_bit_mask = (1ULL << GPIO_BL),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&bl);
    gpio_set_level(GPIO_BL, 1);

    spi_bus_config_t bus = {
        .mosi_io_num   = GPIO_MOSI,
        .miso_io_num   = GPIO_MISO,
        .sclk_io_num   = GPIO_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H * 40 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num       = GPIO_DC,
        .cs_gpio_num       = GPIO_CS,
        .pclk_hz           = LCD_PIX_CLK,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
        .spi_mode          = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io));

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io, &panel_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = io,
        .panel_handle  = panel,
        .buffer_size   = LCD_H * 20,
        .double_buffer = true,
        .hres          = LCD_H,
        .vres          = LCD_V,
        .monochrome    = false,
        .rotation = {
            .swap_xy  = true,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma   = true,
            .swap_bytes = true,
        },
    };
    lv_disp_t *disp = lvgl_port_add_disp(&disp_cfg);
    (void)disp;

    lvgl_port_lock(0);
    lv_obj_t *label = lv_label_create(lv_screen_active());
    lv_label_set_text(label, "Hello CYD");
    lv_obj_center(label);
    lvgl_port_unlock();
}
