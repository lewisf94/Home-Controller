/*
 * Phase 2 Step 4 — WiFi STA connect
 *
 * Cumulative state (steps 0..4 verified on hardware):
 *   Step 0 backlight blink
 *   Step 1 ILI9341 colour cycle
 *   Step 2 LVGL "Hello CYD" label
 *   Step 3 XPT2046 touch as LVGL input (red square follows finger)
 *   Step 4 esp_wifi STA, blocks until IP is logged
 *
 * Orientation (180-deg landscape, USB connector on the LEFT):
 *   Screen (0,0) is top-left; X increases right (0..319); Y down (0..239).
 *
 * Touch pipeline:
 *   The atanisoft XPT2046 driver scales raw ADC into 0..x_max / 0..y_max,
 *   then esp_lcd_touch applies mirror_x / mirror_y / swap_xy. We do the
 *   full rotation inside touch_calibrate() and leave those flags zero so
 *   the math stays in one place.
 *
 * WiFi:
 *   SSID and password live in secrets.h (gitignored). The connect is
 *   synchronous in app_main -- it blocks until WIFI_CONNECTED_BIT is set,
 *   so subsequent steps can assume the network is up. Auto-reconnects on
 *   transient drops via the event handler.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_xpt2046.h"
#include "esp_lvgl_port.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "lvgl.h"
#include "esp_log.h"
#include "secrets.h"

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

#define TOUCH_RAW_X_MIN  31
#define TOUCH_RAW_X_MAX 202
#define TOUCH_RAW_Y_MIN  25
#define TOUCH_RAW_Y_MAX 261

#define WIFI_MAX_RETRY        10
#define WIFI_CONNECTED_BIT    BIT0
#define WIFI_FAIL_BIT         BIT1

static const char *TAG = "main";

static lv_obj_t *coord_label = NULL;
static lv_obj_t *square      = NULL;
static lv_obj_t *wifi_label  = NULL;

static EventGroupHandle_t s_wifi_event_group;
static int                s_wifi_retry_count = 0;

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

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_retry_count < WIFI_MAX_RETRY) {
            s_wifi_retry_count++;
            ESP_LOGW(TAG, "wifi disconnected, retry %d/%d",
                     s_wifi_retry_count, WIFI_MAX_RETRY);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "wifi failed to connect after %d retries", WIFI_MAX_RETRY);
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "wifi connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid     = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi connecting to \"%s\"...", WIFI_SSID);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

    return (bits & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_FAIL;
}

void app_main(void)
{
    ESP_LOGI(TAG, "Music Controller IDF step 4 (wifi build)");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    if (wifi_init_sta() != ESP_OK) {
        ESP_LOGE(TAG, "wifi did not connect -- continuing without network");
    }

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
        .x_max        = LCD_V,
        .y_max        = LCD_H,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .levels = {
            .reset     = 0,
            .interrupt = 0,
        },
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

    wifi_label = lv_label_create(lv_screen_active());
    esp_netif_ip_info_t ip_info = {0};
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        lv_label_set_text_fmt(wifi_label, "IP " IPSTR, IP2STR(&ip_info.ip));
    } else {
        lv_label_set_text(wifi_label, "wifi: offline");
    }
    lv_obj_align(wifi_label, LV_ALIGN_TOP_LEFT, 4, 2);

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
