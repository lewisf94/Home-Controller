/*
 * Music Controller -- ESP-IDF main.
 *
 * Wiring only: brings up LCD + touch + LVGL + WiFi + Spotify polling,
 * then hands the UI surface to ui.c which builds the album browser
 * and now-playing screens.
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
 * Spotify polling:
 *   spotify_task wakes every 5 s, ensures the access token is fresh,
 *   GETs /v1/me/player, and pushes the result into the UI. If the
 *   album-art URL changed (track-change), it also downloads and
 *   JPEG-decodes the art into s_art_rgb, then publishes via
 *   ui_art_refresh().
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
#include "esp_heap_caps.h"
#include "album_art.h"
#include "littlefs.h"
#include "secrets.h"
#include "spotify.h"
#include "ui.h"

#include <string.h>
#include <stdlib.h>

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

#define ART_W 160
#define ART_H 160
#define ART_RGB_BYTES (ART_W * ART_H * 2)

#define TOUCH_RAW_X_MIN  31
#define TOUCH_RAW_X_MAX 202
#define TOUCH_RAW_Y_MIN  25
#define TOUCH_RAW_Y_MAX 261

#define WIFI_MAX_RETRY        10
#define WIFI_CONNECTED_BIT    BIT0
#define WIFI_FAIL_BIT         BIT1

static const char *TAG = "main";

static uint8_t        *s_art_rgb = NULL;
static lv_image_dsc_t  s_art_dsc = {0};
static char            s_art_url_loaded[256] = {0};

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

/* LittleFS scratch path where the freshly-downloaded JPEG lives between
 * spotify_download_to_file() and album_art_decode_file(). Overwritten
 * on every track change. */
#define ART_JPEG_PATH "/littlefs/nowplaying.jpg"

/* Decode the JPEG at ART_JPEG_PATH into s_art_rgb and hand the buffer
 * to ui.c, which publishes it under the LVGL lock. Returns true if
 * the new art is now visible. */
static bool decode_and_publish_art(void)
{
    if (!s_art_rgb) return false;

    uint16_t w = 0, h = 0;
    if (!album_art_decode_file(ART_JPEG_PATH,
                               (uint16_t *)s_art_rgb, ART_W * ART_H,
                               &w, &h)) {
        ESP_LOGW(TAG, "jpeg decode failed");
        return false;
    }
    ESP_LOGI(TAG, "decoded %ux%u album art (%u bytes)",
             (unsigned)w, (unsigned)h, (unsigned)(w * h * 2));

    ui_art_refresh(s_art_rgb, w, h);
    return true;
}

static void spotify_task(void *arg)
{
    (void)arg;
    spotify_init(SPOTIFY_CLIENT_ID, SPOTIFY_CLIENT_SECRET, SPOTIFY_REFRESH_TOKEN);

    spotify_track_t info;
    while (1) {
        if (spotify_fetch_player(&info)) {
            ESP_LOGI(TAG, "now playing: %s -- %s [%lu/%lu ms, %s]",
                     info.artist, info.title,
                     (unsigned long)info.progress_ms,
                     (unsigned long)info.duration_ms,
                     info.is_playing ? "playing" : "paused");
            ui_set_track_info(&info);

            if (info.album_art_url[0] &&
                strcmp(info.album_art_url, s_art_url_loaded) != 0 &&
                littlefs_is_mounted()) {
                size_t bytes = 0;
                if (spotify_download_to_file(info.album_art_url, ART_JPEG_PATH, &bytes)) {
                    ESP_LOGI(TAG, "downloaded %u bytes -> %s",
                             (unsigned)bytes, ART_JPEG_PATH);
                    if (decode_and_publish_art()) {
                        strncpy(s_art_url_loaded, info.album_art_url,
                                sizeof(s_art_url_loaded) - 1);
                        s_art_url_loaded[sizeof(s_art_url_loaded) - 1] = '\0';
                    }
                }
            }
        } else {
            /* HTTP 204 (no active playback) or transient error: blank labels
             * so a previous track isn't shown stale. The art widget keeps
             * its last image; it'll refresh when playback resumes. */
            ui_set_track_info(NULL);
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
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
            .password = WIFI_PASSWORD,
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
    ESP_LOGI(TAG, "Music Controller IDF (browser + now-playing UI)");

    s_art_rgb = heap_caps_malloc(ART_RGB_BYTES, MALLOC_CAP_DEFAULT);
    if (!s_art_rgb) {
        ESP_LOGE(TAG, "failed to allocate %d byte art buffer", ART_RGB_BYTES);
    }

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

    /* Mount LittleFS before kicking off the Spotify task so the download
     * path can write the JPEG immediately. */
    if (!littlefs_mount()) {
        ESP_LOGE(TAG, "LittleFS mount failed -- album art disabled");
    }

    ui_init(&s_art_dsc);

    xTaskCreate(spotify_task, "spotify", 8192, NULL, 5, NULL);
}
