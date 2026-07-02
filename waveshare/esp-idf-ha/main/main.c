/*
 * Music Controller -- ESP32-P4 HOME ASSISTANT build.
 *
 * Same board and UI as waveshare/esp-idf/, but the backend is Home Assistant
 * over a local WebSocket instead of the Spotify Web API. HA OS on a Pi 5 runs
 * the Spotify integration via Music Assistant; this firmware subscribes to the
 * configured media_player entity and maps its push-state into the shared
 * spotify_track_t that the UI already understands.
 *
 * Architecture (mirrors the direct-Spotify build):
 *   LVGL task     -- all rendering, started by the BSP
 *   ha_task       -- drains s_cmd_queue, calls ha_client, updates track state
 *   audio task    -- synthesised UI sounds (via audio.c)
 *   knob task     -- optional haptic RP2040 co-MCU (gated on KNOB_ENABLED)
 *
 * Information flows ONE way: UI touch -> queue -> ha_task. The display lock
 * rule (bsp_display_lock/unlock) applies; ui_*() helpers take it for you.
 *
 * Credentials in include/secrets.h (gitignored; see include/secrets.h.example).
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "secrets.h"
#include "ha_client.h"
#include "player.h"
#include "ui.h"
#include "album_art.h"
#include "littlefs.h"
#include "audio.h"

#define KNOB_ENABLED 0   /* set 1 when the RP2040 haptic knob hardware is fitted */
#if KNOB_ENABLED
#include "knob_input.h"
#endif

static const char *TAG = "main";

/* ── WiFi ──────────────────────────────────────────────────────────────────── */
#define WIFI_MAX_RETRY     10
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_event_group;
static int                s_wifi_retry = 0;

static void on_wifi_event(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_retry < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_wifi_retry++;
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_wifi_retry = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void)
{
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t h1, h2;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL, &h1));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event, NULL, &h2));

    wifi_config_t wc = { .sta = { .ssid = WIFI_SSID, .password = WIFI_PASSWORD } };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
        portMAX_DELAY);
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected");
    } else {
        ESP_LOGE(TAG, "WiFi failed after %d retries", WIFI_MAX_RETRY);
    }
}

/* ── Command queue ─────────────────────────────────────────────────────────── */
typedef enum {
    HCMD_TOGGLE_PLAY = 0,
    HCMD_PREV,
    HCMD_NEXT,
    HCMD_SEEK,
    HCMD_VOLUME,
    HCMD_SHUFFLE,
    HCMD_PLAY_ALBUM,
    HCMD_GET_DEVICES,  /* enumerate HA media_player entities */
    HCMD_TRANSFER,     /* switch the active media_player entity */
} hcmd_type_t;

typedef struct {
    hcmd_type_t type;
    union {
        uint32_t seek_ms;
        int      volume_pct;
        char     album_uri[64];
        char     device_id[64];
    };
} hcmd_t;

static QueueHandle_t s_cmd_queue;
#define CMD_QUEUE_DEPTH 8

/* ── ui_request_* callbacks (called from the LVGL task, post to ha_task) ─── */
void ui_request_play(const char *uri)
{
    hcmd_t c = { .type = HCMD_PLAY_ALBUM };
    snprintf(c.album_uri, sizeof c.album_uri, "%s", uri);
    xQueueSend(s_cmd_queue, &c, 0);
}
void ui_request_toggle_play(void)
    { hcmd_t c = {.type=HCMD_TOGGLE_PLAY}; xQueueSend(s_cmd_queue,&c,0); }
void ui_request_prev(void)
    { hcmd_t c = {.type=HCMD_PREV}; xQueueSend(s_cmd_queue,&c,0); }
void ui_request_next(void)
    { hcmd_t c = {.type=HCMD_NEXT}; xQueueSend(s_cmd_queue,&c,0); }
void ui_request_seek(uint32_t ms)
    { hcmd_t c = {.type=HCMD_SEEK,.seek_ms=ms}; xQueueSend(s_cmd_queue,&c,0); }
void ui_request_volume(int pct)
    { hcmd_t c = {.type=HCMD_VOLUME,.volume_pct=pct}; xQueueSend(s_cmd_queue,&c,0); }
void ui_request_shuffle(void)
    { hcmd_t c = {.type=HCMD_SHUFFLE}; xQueueSend(s_cmd_queue,&c,0); }
void ui_request_get_devices(void)
    { hcmd_t c = {.type=HCMD_GET_DEVICES}; xQueueSend(s_cmd_queue,&c,0); }
void ui_request_transfer(const char *device_id)
{
    hcmd_t c = { .type = HCMD_TRANSFER };
    snprintf(c.device_id, sizeof c.device_id, "%s", device_id ? device_id : "");
    xQueueSend(s_cmd_queue, &c, 0);
}
void ui_request_select_sonos(const char *host)
    { (void)host; /* all HA media_players switch via ui_request_transfer */ }

/* ── Art decode ─────────────────────────────────────────────────────────────── */
/* Double-buffered in PSRAM: ha_task decodes into the idle buffer, then
 * ui_art_refresh republishes it to LVGL under the LVGL lock -- so a decode
 * never overwrites the pixels the render task is currently reading. Mirrors
 * waveshare/esp-idf/main.c's s_art_rgb pattern (a single shared buffer here
 * previously let JPEGDEC overwrite the live art mid-render, tearing the
 * on-screen cover). */
static uint8_t        *s_art_rgb[2]  = { NULL, NULL };
static int             s_art_buf_idx = 0;
static lv_image_dsc_t  s_art_dsc = {0};

#define ART_DECODE_W 320
#define ART_DECODE_H 320
#define ART_RGB_BYTES ((size_t)ART_DECODE_W * ART_DECODE_H * 2)
#define ART_FILE_PATH "/littlefs/art.jpg"

static void decode_art(const char *path)
{
    int      next = s_art_buf_idx ^ 1;
    uint8_t *buf  = s_art_rgb[next];
    if (!buf) return;
    uint16_t w = 0, h = 0;
    if (!album_art_decode_file(path, (uint16_t *)buf, ART_DECODE_W * ART_DECODE_H, &w, &h)) {
        ESP_LOGE(TAG, "art decode failed");
        return;
    }
    ui_art_refresh(buf, w, h);
    s_art_buf_idx = next;
}

/* ── HA task ────────────────────────────────────────────────────────────────── */
static void ha_task(void *arg)
{
    (void)arg;
    ha_client_start();
    ESP_LOGI(TAG, "ha_task running");

    hcmd_t cmd;
    char art_rel[256];

    for (;;) {
        /* Check for a pending art URL from the HA event handler. */
        if (ha_take_pending_art(art_rel, sizeof art_rel)) {
            char art_url[320];
            ha_art_full_url(art_rel, art_url, sizeof art_url);
            size_t art_len = 0;
            if (ha_download_to_file(art_url, ART_FILE_PATH, &art_len) && art_len > 0) {
                decode_art(ART_FILE_PATH);
            }
        }

        /* Drain command queue (non-blocking check). */
        if (xQueueReceive(s_cmd_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE) {
            switch (cmd.type) {
            case HCMD_TOGGLE_PLAY:  ha_toggle_play_pause(); break;
            case HCMD_PREV:         ha_prev_track();        break;
            case HCMD_NEXT:         ha_next_track();        break;
            case HCMD_SHUFFLE:      ha_toggle_shuffle();    break;
            case HCMD_SEEK:         ha_seek_position(cmd.seek_ms);    break;
            case HCMD_VOLUME:       ha_set_volume(cmd.volume_pct);    break;
            case HCMD_PLAY_ALBUM:   ha_play_album(cmd.album_uri);        break;
            case HCMD_GET_DEVICES:  ha_request_devices();                break;
            case HCMD_TRANSFER:     ha_set_active_entity(cmd.device_id); break;
            default: break;
            }
        }
    }
}

/* ── app_main ───────────────────────────────────────────────────────────────── */
void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* Initialise the Waveshare BSP (display + touch + LVGL adapter).
     * Matches the non-HA build: ROTATE_90, TRIPLE_FULL buffering, and
     * hardware-confirmed touch flags (swap_xy=1, mirror_x=1, mirror_y=0). */
    bsp_display_cfg_t disp_cfg = {
        .lv_adapter_cfg  = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation        = ESP_LV_ADAPTER_ROTATE_90,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_FULL,
        .touch_flags     = { .swap_xy = 1, .mirror_x = 1, .mirror_y = 0 },
    };
    bsp_display_start_with_config(&disp_cfg);
    bsp_display_backlight_on();
    ESP_LOGI(TAG, "display up");

    wifi_init();

    /* Art decode buffers in PSRAM (double-buffered, see decode_art above). */
    s_art_rgb[0] = heap_caps_malloc(ART_RGB_BYTES, MALLOC_CAP_SPIRAM);
    s_art_rgb[1] = heap_caps_malloc(ART_RGB_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_art_rgb[0] || !s_art_rgb[1]) ESP_LOGE(TAG, "art buf alloc failed");

    littlefs_mount();

    audio_init();

    /* Build the UI screens (browser + now-playing + settings). */
    bsp_display_lock(-1);
    ui_init(&s_art_dsc);
    bsp_display_unlock();

    ha_client_init(HA_HOST, HA_PORT, HA_TOKEN, HA_ENTITY);

    s_cmd_queue = xQueueCreate(CMD_QUEUE_DEPTH, sizeof(hcmd_t));
    xTaskCreatePinnedToCore(ha_task, "ha_task", 8192, NULL, 5, NULL, 0);

#if KNOB_ENABLED
    knob_input_init();
#endif

    ESP_LOGI(TAG, "init done -- ha_task running");
}
