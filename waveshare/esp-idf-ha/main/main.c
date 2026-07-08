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
#include <stdbool.h>
#include "nvs_flash.h"
#include "esp_log.h"
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
#include "app_core_wifi.h"
#include "app_core_art.h"

#define KNOB_ENABLED 0   /* set 1 when the RP2040 haptic knob hardware is fitted */
#if KNOB_ENABLED
#include "knob_input.h"
#endif

static const char *TAG = "main";

#define CREDS_KEY_WIFI_SSID  "wifi_ssid"
#define CREDS_KEY_WIFI_PASS  "wifi_pass"
#define CREDS_KEY_SP_ID      "sp_id"
#define CREDS_KEY_SP_SECRET  "sp_secret"
#define CREDS_KEY_SP_REFRESH "sp_refresh"
#ifndef SPOTIFY_CLIENT_ID
#define SPOTIFY_CLIENT_ID ""
#endif
#ifndef SPOTIFY_CLIENT_SECRET
#define SPOTIFY_CLIENT_SECRET ""
#endif
#ifndef SPOTIFY_REFRESH_TOKEN
#define SPOTIFY_REFRESH_TOKEN ""
#endif
bool creds_get(const char *key, char *out, size_t out_len, const char *fallback);

/* ── WiFi ──────────────────────────────────────────────────────────────────── */
/* Connect (with resilient background reconnect after fast retries exhaust)
 * lives in app_core_wifi -- see waveshare/components/app_core/wifi.c. No
 * connect chime here (unlike the non-HA build) -- matches this build's prior
 * behaviour; pass a callback to app_core_wifi_connect() if one is wanted. */
#define WIFI_MAX_RETRY 10

/* ── Command queue ─────────────────────────────────────────────────────────── */
typedef enum {
    HCMD_TOGGLE_PLAY = 0,
    HCMD_PREV,
    HCMD_NEXT,
    HCMD_SEEK,
    HCMD_VOLUME,
    HCMD_SHUFFLE,
    HCMD_PLAY_ALBUM,
    HCMD_GET_DEVICES,       /* enumerate HA media_player entities */
    HCMD_TRANSFER,          /* switch the active media_player entity */
    HCMD_GET_ALBUM_CANDIDATES, /* discover HA media libraries for albums */
    HCMD_GET_LIGHTS,        /* enumerate HA light entities */
    HCMD_LIGHT_TOGGLE,      /* toggle one light on/off */
    HCMD_LIGHT_BRIGHTNESS,  /* set one light's brightness */
    HCMD_LIGHT_HUE,         /* set one light's hue/saturation */
    HCMD_REFRESH_COVERS,    /* fetch+decode covers for runtime-added albums */
} hcmd_type_t;

typedef struct {
    hcmd_type_t type;
    union {
        uint32_t seek_ms;
        int      volume_pct;
        char     album_uri[64];
        char     album_query[80];
        char     device_id[64];
        char     light_id[96];   /* HCMD_LIGHT_TOGGLE -- matches ui_light_t.entity_id */
        struct {
            char entity_id[96];
            int  pct;
        } light_brightness;      /* HCMD_LIGHT_BRIGHTNESS */
        struct {
            char entity_id[96];
            int  hue_deg;
            int  sat_pct;
        } light_hue;             /* HCMD_LIGHT_HUE */
    };
} hcmd_t;

static QueueHandle_t s_cmd_queue;
#define CMD_QUEUE_DEPTH 16

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
void ui_request_get_album_candidates(void)
    { hcmd_t c = {.type=HCMD_GET_ALBUM_CANDIDATES}; xQueueSend(s_cmd_queue,&c,0); }
void ui_request_search_album_candidates(const char *query)
{
    hcmd_t c = { .type = HCMD_GET_ALBUM_CANDIDATES };
    snprintf(c.album_query, sizeof c.album_query, "%s", query ? query : "");
    xQueueSend(s_cmd_queue, &c, 0);
}
void ui_request_refresh_covers(void)
    { hcmd_t c = {.type=HCMD_REFRESH_COVERS}; xQueueSend(s_cmd_queue,&c,0); }
void ui_request_transfer(const char *device_id)
{
    hcmd_t c = { .type = HCMD_TRANSFER };
    snprintf(c.device_id, sizeof c.device_id, "%s", device_id ? device_id : "");
    xQueueSend(s_cmd_queue, &c, 0);
}
void ui_request_select_sonos(const char *host)
    { (void)host; /* all HA media_players switch via ui_request_transfer */ }
void ui_request_get_lights(void)
    { hcmd_t c = {.type=HCMD_GET_LIGHTS}; xQueueSend(s_cmd_queue,&c,0); }
void ui_request_light_toggle(const char *entity_id)
{
    hcmd_t c = { .type = HCMD_LIGHT_TOGGLE };
    snprintf(c.light_id, sizeof c.light_id, "%s", entity_id ? entity_id : "");
    xQueueSend(s_cmd_queue, &c, 0);
}
void ui_request_light_brightness(const char *entity_id, int pct)
{
    hcmd_t c = { .type = HCMD_LIGHT_BRIGHTNESS };
    snprintf(c.light_brightness.entity_id, sizeof c.light_brightness.entity_id,
             "%s", entity_id ? entity_id : "");
    c.light_brightness.pct = pct;
    xQueueSend(s_cmd_queue, &c, 0);
}
void ui_request_light_hue(const char *entity_id, int hue_deg, int sat_pct)
{
    hcmd_t c = { .type = HCMD_LIGHT_HUE };
    snprintf(c.light_hue.entity_id, sizeof c.light_hue.entity_id,
             "%s", entity_id ? entity_id : "");
    c.light_hue.hue_deg = hue_deg;
    c.light_hue.sat_pct = sat_pct;
    xQueueSend(s_cmd_queue, &c, 0);
}

/* ── Art decode ─────────────────────────────────────────────────────────────── */
/* Double-buffered in PSRAM (app_core_art, shared with waveshare/esp-idf/main.c):
 * ha_task decodes into the idle buffer, then art_buffer_publish republishes it
 * to LVGL under the LVGL lock -- so a decode never overwrites the pixels the
 * render task is currently reading (a single shared buffer here previously let
 * JPEGDEC overwrite the live art mid-render, tearing the on-screen cover). */
static art_buffer_t    s_art = {0};
static lv_image_dsc_t  s_art_dsc = {0};

#define ART_DECODE_W 320
#define ART_DECODE_H 320
#define ART_RGB_BYTES ((size_t)ART_DECODE_W * ART_DECODE_H * 2)
#define ART_FILE_PATH "/littlefs/art.jpg"
#define LIGHT_REFRESH_DELAY_MS 400

/* Runtime album catalogue + cover fetch (p4_shared/album_catalog.c + ui.c). No
 * shared header while the runtime catalogue is a prototype -- declared here. */
#include "album_thumbs.h"          /* ALBUM_THUMB_W / _H / _BYTES */
#define RT_ART_PATH "/littlefs/rtart.jpg"   /* transient scratch for cover fetch */
size_t album_catalog_runtime_count(void);
bool   album_catalog_runtime_art_todo(size_t rt_idx, char *url_out, size_t url_len);
bool   album_catalog_set_thumb(size_t rt_idx, const uint16_t *rgb);
void   ui_notify_covers_updated(void);

static void refresh_lights_after_command(bool sent)
{
    if (!sent) return;
    vTaskDelay(pdMS_TO_TICKS(LIGHT_REFRESH_DELAY_MS));
    ha_request_lights();
}

static void decode_art(const char *path)
{
    uint8_t *buf = art_buffer_idle(&s_art);
    if (!buf) return;
    uint16_t w = 0, h = 0;
    if (!album_art_decode_file(path, (uint16_t *)buf, ART_DECODE_W * ART_DECODE_H, &w, &h)) {
        ESP_LOGE(TAG, "art decode failed");
        return;
    }
    art_buffer_publish(&s_art, w, h);
}

/* Fetch + decode browser thumbnails for any runtime-added albums that don't
 * have art yet (Spotify cover URLs from the search path). Runs on ha_task (net
 * + JPEG decode). Triggered on first WiFi connect (boot) and after each ADD. */
static void fetch_runtime_covers(void)
{
    size_t n = album_catalog_runtime_count();
    if (n == 0) return;
    uint16_t *thumb = heap_caps_malloc(ALBUM_THUMB_BYTES, MALLOC_CAP_SPIRAM);
    if (!thumb) { ESP_LOGW(TAG, "cover thumb scratch alloc failed"); return; }

    int fetched = 0;
    for (size_t i = 0; i < n; i++) {
        char url[128];
        if (!album_catalog_runtime_art_todo(i, url, sizeof url)) continue;
        size_t bytes = 0;
        if (!ha_download_to_file(url, RT_ART_PATH, &bytes) || bytes == 0) {
            ESP_LOGW(TAG, "runtime cover download failed (album %u)", (unsigned)i);
            continue;
        }
        if (album_art_make_thumb_file(RT_ART_PATH, thumb, ALBUM_THUMB_W, ALBUM_THUMB_H) &&
            album_catalog_set_thumb(i, thumb)) {
            fetched++;
            ESP_LOGI(TAG, "runtime cover fetched (album %u, %u bytes)", (unsigned)i, (unsigned)bytes);
        }
    }
    heap_caps_free(thumb);
    if (fetched > 0) ui_notify_covers_updated();
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
        ha_client_tick();

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
            case HCMD_GET_ALBUM_CANDIDATES:
                ha_request_album_candidates(cmd.album_query);
                break;
            case HCMD_GET_LIGHTS:   ha_request_lights();                break;
            case HCMD_LIGHT_TOGGLE:
                refresh_lights_after_command(ha_light_toggle(cmd.light_id));
                break;
            case HCMD_LIGHT_BRIGHTNESS:
                ha_light_set_brightness(cmd.light_brightness.entity_id,
                                        cmd.light_brightness.pct);
                break;
            case HCMD_LIGHT_HUE:
                ha_light_set_hs(cmd.light_hue.entity_id,
                                cmd.light_hue.hue_deg,
                                cmd.light_hue.sat_pct);
                break;
            case HCMD_REFRESH_COVERS:
                fetch_runtime_covers();
                break;
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

    /* Effective WiFi credentials: Settings > SETUP override, else secrets.h.
     * Static -- the WiFi stack may reference them beyond this call. */
    static char s_cred_ssid[33], s_cred_pass[65];
    static char s_cred_sp_id[160], s_cred_sp_secret[160], s_cred_sp_refresh[300];
    creds_get(CREDS_KEY_WIFI_SSID, s_cred_ssid, sizeof s_cred_ssid, WIFI_SSID);
    creds_get(CREDS_KEY_WIFI_PASS, s_cred_pass, sizeof s_cred_pass, WIFI_PASSWORD);
    creds_get(CREDS_KEY_SP_ID, s_cred_sp_id, sizeof s_cred_sp_id, SPOTIFY_CLIENT_ID);
    creds_get(CREDS_KEY_SP_SECRET, s_cred_sp_secret, sizeof s_cred_sp_secret,
              SPOTIFY_CLIENT_SECRET);
    creds_get(CREDS_KEY_SP_REFRESH, s_cred_sp_refresh, sizeof s_cred_sp_refresh,
              SPOTIFY_REFRESH_TOKEN);
    if (app_core_wifi_connect(s_cred_ssid, s_cred_pass, WIFI_MAX_RETRY, NULL) != ESP_OK) {
        /* Initial connect exhausted its fast retries. Don't abort: app_core_wifi
         * has armed the slow background reconnect timer, which will keep trying
         * every 20 s and recover transparently once the AP is reachable. */
        ESP_LOGE(TAG, "wifi did not connect -- continuing; background reconnect will keep trying");
    }

    /* Art decode buffers in PSRAM (double-buffered, see decode_art above). */
    art_buffer_alloc(&s_art, ART_RGB_BYTES);

    littlefs_mount();

    audio_init();

    /* Build the UI screens (browser + now-playing + settings). */
    bsp_display_lock(-1);
    ui_init(&s_art_dsc);
    bsp_display_unlock();

    ha_client_init(HA_HOST, HA_PORT, HA_TOKEN, HA_ENTITY);
    ha_spotify_init(s_cred_sp_id, s_cred_sp_secret, s_cred_sp_refresh);

    s_cmd_queue = xQueueCreate(CMD_QUEUE_DEPTH, sizeof(hcmd_t));
    if (!s_cmd_queue) {
        ESP_LOGE(TAG, "failed to create command queue");
        return;   /* no mailbox -> nothing could drive playback; give up */
    }
    xTaskCreatePinnedToCore(ha_task, "ha_task", 8192, NULL, 5, NULL, 0);

    /* Boot: fetch covers for any runtime-added albums (ha_task drains this once
     * the network is up). */
    ui_request_refresh_covers();

#if KNOB_ENABLED
    knob_input_init();
#endif

    ESP_LOGI(TAG, "init done -- ha_task running");
    ESP_LOGI(TAG, "feature marker: ha_devices_v2_lights_presets_v1 add_albums_spotify_search_v1");
}
