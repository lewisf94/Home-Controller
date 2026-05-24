/*
 * Music Controller -- ESP32-P4 (Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3),
 * DIRECT SPOTIFY backend (no Home Assistant).
 *
 * STATUS: checkpoint-3 -- Spotify. Connects to WiFi via the onboard ESP32-C6,
 * then runs a Spotify task that refreshes the OAuth token and polls
 * /me/player every 5 s, logging the current track. A typed scmd_t command
 * queue is in place for playback controls (drained by the Spotify task);
 * nothing posts to it yet -- the UI (cp4) and touch input (cp6) come later.
 *
 * Memory: large HTTPS/JPEG buffers spill to PSRAM via the threshold policy in
 * sdkconfig.defaults (see docs/PORT-NOTES.md), so the 256 KB response buffer
 * never lands in the scarce 768 KB internal SRAM.
 *
 * Checkpoint roadmap (see README.md):
 *  1  Display skeleton (hardware-verified)
 *  2  WiFi (hardware-verified)
 *  3  Spotify -- this file                                <-- HERE
 *  4  UI: port cyd ui.c (lvgl_port_lock -> bsp_display_lock); 800x480 layout
 *  5  Assets: album_art.cpp + littlefs + bigger thumbs/art (album-art download)
 *  6  Touch controls: prev/play-pause/next + volume -> ui_request_*()
 *  7  Parity: WiFi-strength indicator, volume HUD, progress bar, view toggle
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
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
#include "spotify.h"

#define WIFI_MAX_RETRY     5
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static const char *TAG = "main";
static EventGroupHandle_t s_wifi_event_group;
static int s_wifi_retry_count = 0;

/* Typed Spotify command queue. UI / input post requests here; spotify_task
 * drains them so the blocking HTTPS calls stay off the render/input path.
 * Ported from cyd/esp-idf/main.c -- kept now so cp4/cp6 just wire ui_request_*
 * to real controls. Depth 8 is generous; commands dispatch in <2 s. */
typedef enum {
    SCMD_PLAY_ALBUM,
    SCMD_TOGGLE_PLAY,
    SCMD_PREV_TRACK,
    SCMD_NEXT_TRACK,
    SCMD_SEEK_MS,
    SCMD_SET_VOLUME,
} scmd_type_t;

typedef struct {
    scmd_type_t  type;
    uint32_t     param;  /* seek_ms (SCMD_SEEK_MS) or volume_pct (SCMD_SET_VOLUME) */
    const char  *uri;    /* SCMD_PLAY_ALBUM only; points into .rodata, always valid */
} scmd_t;

static QueueHandle_t s_cmd_queue = NULL;

static void _post_cmd(scmd_type_t type, uint32_t param, const char *uri)
{
    if (!s_cmd_queue) return;
    scmd_t cmd = { .type = type, .param = param, .uri = uri };
    (void)xQueueSend(s_cmd_queue, &cmd, 0);
}

void ui_request_play(const char *uri)  { _post_cmd(SCMD_PLAY_ALBUM,  0,             uri);  }
void ui_request_toggle_play(void)      { _post_cmd(SCMD_TOGGLE_PLAY, 0,             NULL); }
void ui_request_prev(void)             { _post_cmd(SCMD_PREV_TRACK,  0,             NULL); }
void ui_request_next(void)             { _post_cmd(SCMD_NEXT_TRACK,  0,             NULL); }
void ui_request_seek(uint32_t ms)      { _post_cmd(SCMD_SEEK_MS,     ms,            NULL); }
void ui_request_volume(int pct)        { _post_cmd(SCMD_SET_VOLUME,  (uint32_t)pct, NULL); }

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
        (void)event_data;  /* IP intentionally not logged (keeps logs shareable) */
        ESP_LOGI(TAG, "wifi connected (DHCP lease acquired)");
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

    ESP_LOGI(TAG, "wifi connecting...");  /* SSID not logged (keeps logs shareable) */

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

    return (bits & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_FAIL;
}

/* Drains s_cmd_queue and runs the blocking Spotify HTTPS calls off any
 * future render/input path. Also polls /me/player every 5 s and logs the
 * current track. cp3 only logs; cp4 will push state into ui.c and cp5 will
 * download + decode album art. */
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
        } else {
            ESP_LOGI(TAG, "no active playback (or fetch failed)");
        }

        /* Poll every 5 s, but wake early to service any queued command. */
        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(5000);
        for (;;) {
            scmd_t cmd = {0};
            TickType_t now  = xTaskGetTickCount();
            TickType_t wait = (now >= deadline) ? 0 : (deadline - now);
            if (xQueueReceive(s_cmd_queue, &cmd, wait) == pdTRUE) {
                bool ok = false;
                switch (cmd.type) {
                    case SCMD_PLAY_ALBUM:
                        ok = spotify_play_album(cmd.uri);
                        ESP_LOGI(TAG, "play_album(%s) -> %s", cmd.uri, ok ? "ok" : "FAILED");
                        break;
                    case SCMD_TOGGLE_PLAY:  ok = spotify_toggle_play_pause();        break;
                    case SCMD_PREV_TRACK:   ok = spotify_prev_track();               break;
                    case SCMD_NEXT_TRACK:   ok = spotify_next_track();               break;
                    case SCMD_SEEK_MS:      ok = spotify_seek_position(cmd.param);   break;
                    case SCMD_SET_VOLUME:   ok = spotify_set_volume((int)cmd.param); break;
                }
                (void)ok;
                break;
            }
            if (xTaskGetTickCount() >= deadline) break;
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Music Controller P4 (direct Spotify) -- checkpoint 3: Spotify");

    /* esp_netif logs the assigned IP/mask/gw at INFO; silence it so the serial
     * log stays shareable without redacting. Our own handler logs only that a
     * lease was acquired, never the address. */
    esp_log_level_set("esp_netif_handlers", ESP_LOG_WARN);

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
    lv_label_set_text(status_label, "Music Controller P4\ncheckpoint 3: WiFi connecting...");
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(status_label);
    bsp_display_unlock();

    /* Let the ESP32-C6 WiFi slave boot its esp_hosted firmware before esp_wifi_init. */
    vTaskDelay(pdMS_TO_TICKS(3000));

    if (wifi_init_sta() != ESP_OK) {
        ESP_LOGE(TAG, "wifi did not connect -- cannot reach Spotify");
        bsp_display_lock(-1);
        lv_label_set_text(status_label, "Music Controller P4\ncheckpoint 3: WiFi FAILED\ncheck secrets.h SSID/password");
        bsp_display_unlock();
        return;
    }

    bsp_display_lock(-1);
    lv_label_set_text(status_label, "Music Controller P4\ncheckpoint 3: Spotify\n(see serial monitor)");
    bsp_display_unlock();

    s_cmd_queue = xQueueCreate(8, sizeof(scmd_t));
    if (!s_cmd_queue) {
        ESP_LOGE(TAG, "failed to create command queue");
        return;
    }
    /* 10 KB stack: TLS handshake (cert-bundle validation) on top of the
     * esp_hosted transport is stack-hungry. CYD used 8 KB on native WiFi. */
    xTaskCreate(spotify_task, "spotify", 10240, NULL, 5, NULL);

    ESP_LOGI(TAG, "checkpoint 3: Spotify task started");
}
