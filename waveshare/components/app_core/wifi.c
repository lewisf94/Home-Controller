#include "app_core_wifi.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "app_core_wifi";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
/* Slow background reconnect, armed after the fast retries are exhausted.
 * Without it the device would give up forever on any network blip (router
 * reboot, brief out-of-range) and need a power cycle to recover. */
#define WIFI_RECONNECT_PERIOD_US (20ULL * 1000 * 1000)   /* every 20 s */

static EventGroupHandle_t   s_event_group;
static esp_timer_handle_t   s_reconnect_timer = NULL;
static uint32_t             s_retry_count = 0;
static uint32_t             s_max_retry   = 0;
static void                (*s_on_first_connect)(void) = NULL;
static bool                  s_connected_once = false;

static void reconnect_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "background reconnect attempt");
    esp_wifi_connect();
}

static void event_handler(void *arg, esp_event_base_t base,
                          int32_t event_id, void *event_data)
{
    (void)arg;
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_count < s_max_retry) {
            s_retry_count++;
            ESP_LOGW(TAG, "disconnected, retry %lu/%lu",
                     (unsigned long)s_retry_count, (unsigned long)s_max_retry);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "failed after %lu retries -- arming background reconnect every %llu s",
                     (unsigned long)s_max_retry, WIFI_RECONNECT_PERIOD_US / 1000000);
            xEventGroupSetBits(s_event_group, WIFI_FAIL_BIT);
            if (s_reconnect_timer == NULL) {
                const esp_timer_create_args_t args = {
                    .callback = reconnect_cb,
                    .name     = "wifi_reconnect",
                };
                esp_timer_create(&args, &s_reconnect_timer);
            }
            if (s_reconnect_timer && !esp_timer_is_active(s_reconnect_timer)) {
                esp_timer_start_periodic(s_reconnect_timer, WIFI_RECONNECT_PERIOD_US);
            }
        }
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        (void)event_data;  /* IP intentionally not logged (keeps logs shareable) */
        ESP_LOGI(TAG, "connected (DHCP lease acquired)");
        s_retry_count = 0;
        if (s_reconnect_timer && esp_timer_is_active(s_reconnect_timer)) {
            esp_timer_stop(s_reconnect_timer);
        }
        if (!s_connected_once) {
            s_connected_once = true;
            if (s_on_first_connect) s_on_first_connect();
        }
        xEventGroupSetBits(s_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t app_core_wifi_connect(const char *ssid, const char *password,
                                uint32_t max_retry,
                                void (*on_first_connect)(void))
{
    s_max_retry         = max_retry;
    s_on_first_connect  = on_first_connect;
    s_event_group       = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    wifi_config_t wifi_config = { 0 };
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof wifi_config.sta.ssid);
    strlcpy((char *)wifi_config.sta.password, password, sizeof wifi_config.sta.password);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "connecting...");  /* SSID not logged (keeps logs shareable) */

    EventBits_t bits = xEventGroupWaitBits(s_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

    return (bits & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_FAIL;
}
