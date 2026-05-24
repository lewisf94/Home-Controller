/*
 * Music Controller -- ESP32-P4 (Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3),
 * DIRECT SPOTIFY backend (no Home Assistant).
 *
 * STATUS: checkpoint-2 -- WiFi bring-up. Connects to WiFi via the onboard
 * ESP32-C6 slave (esp_wifi_remote + esp_hosted over SDIO); logs the IP.
 * The esp_wifi_* API is transparent -- identical to the CYD build once
 * esp_wifi_remote is in the manifest.
 *
 * Checkpoint roadmap (see README.md):
 *  1  Display skeleton (hardware-verified)
 *  2  WiFi -- this file                                   <-- HERE
 *  3  Spotify task + scmd_t command queue; log track title
 *  4  UI: port cyd ui.c (lvgl_port_lock -> bsp_display_lock); 800x480 layout
 *  5  Assets: regen album_thumbs.bin; bigger art + fonts
 *  6  Touch controls: prev/play-pause/next + volume -> ui_request_*()
 *  7  Parity: WiFi-strength indicator, volume HUD, progress bar, view toggle
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "secrets.h"

#define WIFI_MAX_RETRY     5
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static const char *TAG = "main";
static EventGroupHandle_t s_wifi_event_group;
static int s_wifi_retry_count = 0;

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
            ESP_LOGE(TAG, "wifi failed after %d retries", WIFI_MAX_RETRY);
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
            .ssid              = WIFI_SSID,
            .password          = WIFI_PASSWORD,
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
    ESP_LOGI(TAG, "Music Controller P4 (direct Spotify) -- checkpoint 2: WiFi");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Display first so we see something while WiFi connects. */
    bsp_display_cfg_t cfg = {
        .lv_adapter_cfg  = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation        = ESP_LV_ADAPTER_ROTATE_90,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL,
        .touch_flags     = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
    };
    bsp_display_start_with_config(&cfg);
    bsp_display_backlight_on();

    bsp_display_lock(-1);
    lv_obj_t *status_label = lv_label_create(lv_screen_active());
    lv_label_set_text(status_label, "Music Controller P4\ncheckpoint 2: WiFi connecting...");
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(status_label);
    bsp_display_unlock();

    /* Give the ESP32-C6 WiFi slave time to boot its esp_hosted slave firmware
     * before we call esp_wifi_init -- without this, the first hosted negotiation
     * may time out. 3 s is conservative; 2 s works in most boards. */
    vTaskDelay(pdMS_TO_TICKS(3000));

    if (wifi_init_sta() == ESP_OK) {
        ESP_LOGI(TAG, "checkpoint 2: WiFi OK -- ready for checkpoint 3 (Spotify)");
        bsp_display_lock(-1);
        lv_label_set_text(status_label, "Music Controller P4\ncheckpoint 2: WiFi OK");
        bsp_display_unlock();
    } else {
        ESP_LOGE(TAG, "checkpoint 2: WiFi FAILED");
        bsp_display_lock(-1);
        lv_label_set_text(status_label, "Music Controller P4\ncheckpoint 2: WiFi FAILED\ncheck secrets.h SSID/password");
        bsp_display_unlock();
    }
}
