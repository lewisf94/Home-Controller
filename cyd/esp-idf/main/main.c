/*
 * Step 3 — XPT2046 touch as LVGL input device
 *
 * Done when: a red 50x50 square follows the finger anywhere on the
 * panel. Serial shows "Music Controller IDF step 3".
 *
 * Touch lives on its own SPI bus (SPI3_HOST / HSPI) to keep the
 * display bus free. The touch panel reports raw 12-bit ADC values;
 * the esp_lcd_touch driver maps them to screen coordinates using
 * x_max/y_max and the swap/mirror flags.
 *
 * Orientation (after the 180-deg landscape flip applied in Step 3):
 *   - Long edge horizontal, USB connector on the LEFT.
 *   - Screen origin (0,0) is TOP-LEFT corner.
 *   - X increases RIGHTWARD (0 -> 319).
 *   - Y increases DOWNWARD (0 -> 239).
 *   - Display flags: swap_xy=true, mirror_x=true, mirror_y=true.
 *   - Touch flags:   swap_xy=1,    mirror_x=1,    mirror_y=1.
 * If the orientation needs to change later, flip BOTH the display
 * rotation and the touch flags together so they stay in sync.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_xpt2046.h"
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

#define TOUCH_HOST     SPI3_HOST
#define GPIO_TOUCH_IRQ  36
#define GPIO_TOUCH_MOSI 32
#define GPIO_TOUCH_MISO 39
#define GPIO_TOUCH_CLK  25
#define GPIO_TOUCH_CS   33

#define SQUARE_SIZE 50

/*
 * XPT2046 calibration measured at the four corners on this panel
 * (180-deg landscape orientation). The atanisoft driver pre-scales
 * raw ADC into 0..x_max / 0..y_max, but the usable range is squashed
 * to roughly [31..202] on raw_x and [25..261] on raw_y. The
 * process_coordinates callback below maps that observed range
 * straight to the final screen coords (so we set the touch
 * swap_xy / mirror_x / mirror_y flags to 0 and do the rotation here).
 *
 * Direction (after the 180-deg flip):
 *   raw_y HIGH  -> screen LEFT      raw_y LOW  -> screen RIGHT
 *   raw_x HIGH  -> screen TOP       raw_x LOW  -> screen BOTTOM
 */
#define TOUCH_RAW_X_MIN  31
#define TOUCH_RAW_X_MAX 202
#define TOUCH_RAW_Y_MIN  25
#define TOUCH_RAW_Y_MAX 261

static const char *TAG = "main";

static lv_obj_t *coord_label = NULL;
static lv_obj_t *square      = NULL;

static void touch_calibrate(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y,
                            uint16_t *strength, uint8_t *point_num, uint8_t max_point_num)
{
    (void)tp;
    (void)strength;
    (void)max_point_num;
    for (uint8_t i = 0; i < *point_num; i++) {
        uint16_t raw_x = x[i];
        uint16_t raw_y = y[i];
        int32_t fx = (int32_t)(TOUCH_RAW_Y_MAX - raw_y) * (LCD_H - 1)
                   / (TOUCH_RAW_Y_MAX - TOUCH_RAW_Y_MIN);
        int32_t fy = (int32_t)(TOUCH_RAW_X_MAX - raw_x) * (LCD_V - 1)
                   / (TOUCH_RAW_X_MAX - TOUCH_RAW_X_MIN);
        if (fx < 0) fx = 0;
        if (fx > LCD_H - 1) fx = LCD_H - 1;
        if (fy < 0) fy = 0;
        if (fy > LCD_V - 1) fy = LCD_V - 1;
        x[i] = (uint16_t)fx;
        y[i] = (uint16_t)fy;
    }
}

static void on_press(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    if (square) {
        lv_obj_set_pos(square, p.x - SQUARE_SIZE / 2, p.y - SQUARE_SIZE / 2);
    }
    if (coord_label) {
        lv_label_set_text_fmt(coord_label, "x=%d y=%d", (int)p.x, (int)p.y);
    }
    ESP_LOGI(TAG, "touch x=%d y=%d", (int)p.x, (int)p.y);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Music Controller IDF step 3 (calibrated build)");

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

    spi_bus_config_t touch_bus = {
        .mosi_io_num   = GPIO_TOUCH_MOSI,
        .miso_io_num   = GPIO_TOUCH_MISO,
        .sclk_io_num   = GPIO_TOUCH_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 64,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(TOUCH_HOST, &touch_bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_spi_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_SPI_XPT2046_CONFIG(GPIO_TOUCH_CS);
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)TOUCH_HOST, &tp_io_cfg, &tp_io));

    esp_lcd_touch_handle_t tp = NULL;
    esp_lcd_touch_config_t tp_cfg = {
        /*
         * x_max / y_max apply to the PRE-swap axes (the atanisoft
         * driver scales raw_X into 0..x_max and raw_Y into 0..y_max
         * before the common esp_lcd_touch layer applies mirror/swap).
         * With swap_xy=1, the screen-horizontal axis comes from raw_Y,
         * so y_max must be LCD_H (320) and x_max must be LCD_V (240).
         */
        .x_max        = LCD_V,
        .y_max        = LCD_H,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .levels = {
            .reset     = 0,
            .interrupt = 0,
        },
        /*
         * Mirror is applied BEFORE swap in esp_lcd_touch, so mirror_x
         * flips what becomes screen Y after swap, and mirror_y flips
         * what becomes screen X. Both set to 1 here to match the
         * 180-degree display rotation (mirror_x=true, mirror_y=true
         * in disp_cfg.rotation).
         */
        /*
         * The rotation + mirror is handled inside touch_calibrate(),
         * so we leave the driver-level transforms off here.
         */
        .flags = {
            .swap_xy  = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
        .process_coordinates = touch_calibrate,
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_spi_xpt2046(tp_io, &tp_cfg, &tp));

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
            .mirror_x = true,
            .mirror_y = true,
        },
        .flags = {
            .buff_dma   = true,
            .swap_bytes = true,
        },
    };
    lv_disp_t *disp = lvgl_port_add_disp(&disp_cfg);

    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp   = disp,
        .handle = tp,
    };
    lvgl_port_add_touch(&touch_cfg);

    lvgl_port_lock(0);
    lv_obj_remove_flag(lv_screen_active(), LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *top_label = lv_label_create(lv_screen_active());
    lv_label_set_text(top_label, "TOP");
    lv_obj_align(top_label, LV_ALIGN_TOP_MID, 0, 2);

    coord_label = lv_label_create(lv_screen_active());
    lv_label_set_text(coord_label, "x=? y=?");
    lv_obj_align(coord_label, LV_ALIGN_BOTTOM_MID, 0, -2);

    square = lv_obj_create(lv_screen_active());
    lv_obj_set_size(square, SQUARE_SIZE, SQUARE_SIZE);
    lv_obj_set_pos(square, (LCD_H - SQUARE_SIZE) / 2, (LCD_V - SQUARE_SIZE) / 2);
    lv_obj_set_style_bg_color(square, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_set_style_border_width(square, 0, 0);
    lv_obj_remove_flag(square, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_event_cb(lv_screen_active(), on_press, LV_EVENT_PRESSING, NULL);
    lvgl_port_unlock();
}
