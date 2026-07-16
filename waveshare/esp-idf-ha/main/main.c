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
#include "freertos/idf_additions.h"
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
#include "sendspin_player.h"
#include "app_core_wifi.h"
#include "app_core_art.h"
#include "app_core_reliability.h"
#include "app_core_ota.h"

void album_art_init(void);

/* RP2040 haptic knob. Default off; enable with `idf.py build -DKNOB_ENABLED=1`
 * (plumbed through main/CMakeLists.txt) or by editing this default once the
 * daughterboard is fitted. Off = the UART is never configured, no GPIO touched. */
#ifndef KNOB_ENABLED
#define KNOB_ENABLED 0
#endif
#if KNOB_ENABLED
#include "knob_input.h"
#endif

static const char *TAG = "main";

#define CREDS_KEY_WIFI_SSID  "wifi_ssid"
#define CREDS_KEY_WIFI_PASS  "wifi_pass"
#define CREDS_KEY_SP_ID      "sp_id"
#define CREDS_KEY_SP_SECRET  "sp_secret"
#define CREDS_KEY_SP_REFRESH "sp_refresh"
#define CREDS_KEY_OTA_URL    "ota_url"
#ifndef SPOTIFY_CLIENT_ID
#define SPOTIFY_CLIENT_ID ""
#endif
#ifndef SPOTIFY_CLIENT_SECRET
#define SPOTIFY_CLIENT_SECRET ""
#endif
#ifndef SPOTIFY_REFRESH_TOKEN
#define SPOTIFY_REFRESH_TOKEN ""
#endif
#ifndef OTA_URL
#define OTA_URL ""            /* optional secrets.h default firmware URL */
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
    HCMD_LIGHT_HUE,         /* set one light's hue/saturation or Kelvin temperature */
    HCMD_REFRESH_COVERS,    /* fetch+decode covers for runtime-added albums */
    HCMD_GET_QUEUE,
    HCMD_QUEUE_ADD,
    HCMD_QUEUE_CLEAR,
    HCMD_SEARCH_QUEUE_TRACKS,
    HCMD_OTA,               /* over-the-air firmware update (reads FIRMWARE URL cred) */
} hcmd_type_t;

typedef struct {
    hcmd_type_t type;
    union {
        uint32_t seek_ms;
        int      volume_pct;
        char     album_uri[64];
        char     album_query[80];
        char     queue_query[80];
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
        struct {
            char uri[96];
            bool play_next;
        } queue_add;
    };
} hcmd_t;

static QueueHandle_t s_cmd_queue;
#define CMD_QUEUE_DEPTH 16

typedef enum {
    MEDIA_WORK_NOW_PLAYING_ART = 0,
    MEDIA_WORK_RUNTIME_COVERS,
} media_work_type_t;

typedef struct {
    media_work_type_t type;
    char art_rel[256];
} media_work_t;

static QueueHandle_t s_media_queue;
#define MEDIA_QUEUE_DEPTH 4
static TaskHandle_t s_ha_task_handle;
static TaskHandle_t s_media_task_handle;

/* ── ui_request_* callbacks (called from the LVGL task, post to ha_task) ─── */
void ui_request_play(const char *uri)
{
    hcmd_t c = { .type = HCMD_PLAY_ALBUM };
    snprintf(c.album_uri, sizeof c.album_uri, "%s", uri);
    xQueueSend(s_cmd_queue, &c, 0);
}
void ui_request_toggle_play(void)
    { hcmd_t c = {.type=HCMD_TOGGLE_PLAY}; xQueueSendToFront(s_cmd_queue,&c,0); }
void ui_request_prev(void)
    { hcmd_t c = {.type=HCMD_PREV}; xQueueSendToFront(s_cmd_queue,&c,0); }
void ui_request_next(void)
    { hcmd_t c = {.type=HCMD_NEXT}; xQueueSendToFront(s_cmd_queue,&c,0); }
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
void ui_request_get_queue(void)
    { hcmd_t c = {.type=HCMD_GET_QUEUE}; xQueueSend(s_cmd_queue,&c,0); }
void ui_request_queue_add(const char *uri, bool play_next)
{
    hcmd_t c = { .type = HCMD_QUEUE_ADD };
    snprintf(c.queue_add.uri, sizeof c.queue_add.uri, "%s", uri ? uri : "");
    c.queue_add.play_next = play_next;
    xQueueSend(s_cmd_queue, &c, 0);
}
void ui_request_queue_clear(void)
    { hcmd_t c = {.type=HCMD_QUEUE_CLEAR}; xQueueSend(s_cmd_queue,&c,0); }
void ui_request_search_queue_tracks(const char *query)
{
    hcmd_t c = { .type = HCMD_SEARCH_QUEUE_TRACKS };
    snprintf(c.queue_query, sizeof c.queue_query, "%s", query ? query : "");
    xQueueSend(s_cmd_queue, &c, 0);
}
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
void ui_request_ota(void)
    { hcmd_t c = {.type=HCMD_OTA}; xQueueSend(s_cmd_queue,&c,0); }

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

/* Runtime album catalogue + cover fetch (p4_shared/album_catalog.c + ui.c). No
 * shared header while the runtime catalogue is a prototype -- declared here. */
#include "album_thumbs.h"          /* ALBUM_THUMB_W / _H / _BYTES */
#define RT_ART_PATH "/littlefs/rtart.jpg"   /* transient scratch for cover fetch */
size_t album_catalog_runtime_count(void);
bool   album_catalog_runtime_art_todo(size_t rt_idx, char *url_out, size_t url_len);
bool   album_catalog_runtime_needs_art(size_t rt_idx, char *url_out, size_t url_len,
                                       char *uri_out, size_t uri_len);
bool   album_catalog_set_image_url(size_t rt_idx, const char *image_url);
bool   album_catalog_set_thumb(size_t rt_idx, const uint16_t *rgb);
void   ui_notify_covers_updated(void);
bool   ha_spotify_album_image_url(const char *spotify_uri, char *out, size_t out_len);
/* From p4_shared/audio_stream_bridge.h (root-private to p4_shared; declared here
 * to match this file's other cross-component externs). True while the ES8311 is
 * streaming music, so cover work can hold off and protect the audio feed. */
bool   audio_stream_is_active(void);

static void refresh_lights_after_command(bool sent)
{
    /* Non-blocking: arms the coalesced settle deadline in ha_client_tick().
     * Nothing here may delay -- play/pause and further light commands queue
     * behind this dispatch. */
    if (!sent) return;
    ha_request_lights_fresh();
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
typedef enum { COVERS_DONE, COVERS_PROGRESS, COVERS_DEFERRED } cover_status_t;

static cover_status_t fetch_runtime_covers(void)
{
    size_t n = album_catalog_runtime_count();
    if (n == 0) return COVERS_DONE;

    /* Never fetch/decode covers while music is streaming: the HTTPS download and
     * JPEG decode contend with the audio feed and glitch playback. Also bail if
     * internal SRAM is critically low. Covers are browser-only, so waiting until
     * playback stops costs nothing the user sees on the now-playing screen. */
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    bool music = audio_stream_is_active();
    /* Runtime covers come from Spotify's API/CDN using the catalogue
     * credentials, so they do not depend on the HA websocket being online.
     * Keeping HA authentication in this gate left albums permanently blank
     * during an HA outage even when WiFi and Spotify search were healthy. The
     * strict memory and no-music gates still protect ESP-Hosted/audio. */
    if (music || free_internal < 64 * 1024 || largest_internal < 32 * 1024) {
        static TickType_t last_log = 0;
        TickType_t now = xTaskGetTickCount();
        if (last_log == 0 || now - last_log > pdMS_TO_TICKS(30000)) {
            last_log = now;
            ESP_LOGI(TAG, "runtime covers deferred (%s; internal free=%u largest=%u)",
                     music ? "music playing" : "low internal heap",
                     (unsigned)free_internal, (unsigned)largest_internal);
        }
        return COVERS_DEFERRED;
    }

    uint16_t *thumb = heap_caps_malloc(ALBUM_THUMB_BYTES, MALLOC_CAP_SPIRAM);
    if (!thumb) { ESP_LOGW(TAG, "cover thumb scratch alloc failed"); return COVERS_DEFERRED; }

    int attempted = 0;
    int fetched = 0;
    bool more_pending = false;
    bool retry_later = false;
    /* Newest additions first: the album the user just added gets its cover on
     * the first pass instead of waiting behind every older uncached row. */
    for (size_t rev = n; rev > 0; rev--) {
        size_t i = rev - 1;
        char url[128];
        char uri[64];
        if (!album_catalog_runtime_needs_art(i, url, sizeof url, uri, sizeof uri)) continue;
        if (attempted >= 1) {
            more_pending = true;
            break;
        }
        attempted++;
        if (!url[0] && uri[0]) {
            if (ha_spotify_album_image_url(uri, url, sizeof url)) {
                album_catalog_set_image_url(i, url);
            } else {
                ESP_LOGW(TAG, "runtime cover URL repair failed (album %u)", (unsigned)i);
                retry_later = true;
                break;
            }
        }
        if (!url[0]) {
            retry_later = true;
            break;
        }
        size_t bytes = 0;
        if (!ha_download_to_file(url, RT_ART_PATH, &bytes, false) || bytes == 0) {
            ESP_LOGW(TAG, "runtime cover download failed (album %u)", (unsigned)i);
            retry_later = true;
            break;
        }
        if (album_art_make_thumb_file(RT_ART_PATH, thumb, ALBUM_THUMB_W, ALBUM_THUMB_H) &&
            album_catalog_set_thumb(i, thumb)) {
            fetched++;
            ESP_LOGI(TAG, "runtime cover fetched (album %u, %u bytes)", (unsigned)i, (unsigned)bytes);
        } else {
            retry_later = true;
            break;
        }
    }
    heap_caps_free(thumb);
    if (fetched > 0) ui_notify_covers_updated();
    return retry_later ? COVERS_DEFERRED :
           more_pending ? COVERS_PROGRESS : COVERS_DONE;
}

static void queue_now_playing_art(const char *art_rel)
{
    if (!s_media_queue || !art_rel || !art_rel[0]) return;
    media_work_t work = { .type = MEDIA_WORK_NOW_PLAYING_ART };
    snprintf(work.art_rel, sizeof(work.art_rel), "%s", art_rel);
    if (xQueueSendToFront(s_media_queue, &work, 0) != pdTRUE)
        ESP_LOGW(TAG, "media queue full; dropping now-playing art request");
}

static void queue_runtime_covers(void)
{
    if (!s_media_queue) return;
    media_work_t work = { .type = MEDIA_WORK_RUNTIME_COVERS };
    if (xQueueSend(s_media_queue, &work, 0) != pdTRUE)
        ESP_LOGW(TAG, "media queue full; runtime covers already pending");
}

static void media_task(void *arg)
{
    (void)arg;
    TickType_t next_runtime_cover = 0;
    media_work_t work;

    for (;;) {
        TickType_t now = xTaskGetTickCount();
        TickType_t wait = portMAX_DELAY;
        if (next_runtime_cover) {
            int32_t remaining = (int32_t)(next_runtime_cover - now);
            wait = remaining > 0 ? (TickType_t)remaining : 0;
        }

        if (xQueueReceive(s_media_queue, &work, wait) == pdTRUE) {
            if (work.type == MEDIA_WORK_NOW_PLAYING_ART) {
                char art_url[320];
                ha_art_full_url(work.art_rel, art_url, sizeof(art_url));
                size_t art_len = 0;
                if (ha_download_to_file(art_url, ART_FILE_PATH, &art_len, true) && art_len > 0)
                    decode_art(ART_FILE_PATH);
            } else {
                next_runtime_cover = xTaskGetTickCount();
            }
        }

        now = xTaskGetTickCount();
        if (next_runtime_cover && (int32_t)(now - next_runtime_cover) >= 0 &&
            uxQueueMessagesWaiting(s_media_queue) == 0) {
            cover_status_t st = fetch_runtime_covers();
            TickType_t delay = st == COVERS_PROGRESS ? pdMS_TO_TICKS(1500)   /* next album soon */
                             : st == COVERS_DEFERRED ? pdMS_TO_TICKS(30000)  /* wait out playback */
                             : 0;                                            /* all done */
            next_runtime_cover = delay ? xTaskGetTickCount() + delay : 0;
        }
    }
}

/* ── HA task ────────────────────────────────────────────────────────────────── */
static void ha_task(void *arg)
{
    (void)arg;
    ha_client_start();
    ESP_LOGI(TAG, "ha_task running");

    hcmd_t cmd;
    TickType_t ha_ready_since = 0;
    bool sendspin_started = false;
    for (;;) {
        /* Keep transport latency bounded even while the media worker is doing
         * an HTTPS cover download or JPEG decode. */
        if (xQueueReceive(s_cmd_queue, &cmd, pdMS_TO_TICKS(20)) == pdTRUE) {
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
                refresh_lights_after_command(
                    ha_light_set_brightness(cmd.light_brightness.entity_id,
                                            cmd.light_brightness.pct));
                break;
            case HCMD_LIGHT_HUE:
                refresh_lights_after_command(
                    ha_light_set_hs(cmd.light_hue.entity_id,
                                    cmd.light_hue.hue_deg,
                                    cmd.light_hue.sat_pct));
                break;
            case HCMD_REFRESH_COVERS:
                queue_runtime_covers();
                break;
            case HCMD_GET_QUEUE:   ha_request_queue(); break;
            case HCMD_QUEUE_ADD:
                ha_queue_add(cmd.queue_add.uri, cmd.queue_add.play_next);
                break;
            case HCMD_QUEUE_CLEAR: ha_queue_clear(); break;
            case HCMD_SEARCH_QUEUE_TRACKS:
                ha_search_queue_tracks(cmd.queue_query);
                break;
            case HCMD_OTA: {
                char url[300];
                creds_get(CREDS_KEY_OTA_URL, url, sizeof url, OTA_URL);
                app_core_ota_start(url);
                break;
            }
            default: break;
            }
        }

        ha_client_tick();
        app_core_reliability_tick();

        /* Sendspin and HA share the ESP32-C6 SDIO transport. Starting mDNS,
         * its websocket server, and discovery while HA's large initial
         * get_states frame is still arriving exhausts the hosted RX pool.
         * Wait until that frame has been parsed, then leave one quiet second. */
        if (!sendspin_started) {
            if (ha_client_is_ready()) {
                if (!ha_ready_since) ha_ready_since = xTaskGetTickCount();
                if (xTaskGetTickCount() - ha_ready_since >= pdMS_TO_TICKS(1000)) {
                    sendspin_started = sendspin_player_start();
                    if (!sendspin_started)
                        ESP_LOGE(TAG, "local Sendspin player unavailable");
                }
            } else {
                ha_ready_since = 0;
            }
        }

        char art_rel[256];
        if (ha_take_pending_art(art_rel, sizeof(art_rel)))
            queue_now_playing_art(art_rel);
    }
}

/* ── app_main ───────────────────────────────────────────────────────────────── */
void app_main(void)
{
    app_core_reliability_init();

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
    app_core_reliability_checkpoint("display ready");

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
    app_core_reliability_checkpoint("wifi ready");

    /* Art decode buffers in PSRAM (double-buffered, see decode_art above). */
    art_buffer_alloc(&s_art, ART_RGB_BYTES);

    littlefs_mount();

    /* Reserve the internal-SRAM JPEG decode buffer BEFORE Sendspin's FLAC
     * decoder / mDNS / websocket server fragment the internal heap, so album
     * art can still decode while music is playing. */
    album_art_init();

    audio_init();

    /* Build the UI screens (browser + now-playing + settings). */
    bsp_display_lock(-1);
    ui_init(&s_art_dsc);
    bsp_display_unlock();
    app_core_reliability_checkpoint("ui ready");

    ha_client_init(HA_HOST, HA_PORT, HA_TOKEN, HA_ENTITY);
    ha_spotify_init(s_cred_sp_id, s_cred_sp_secret, s_cred_sp_refresh);

    s_cmd_queue = xQueueCreateWithCaps(CMD_QUEUE_DEPTH, sizeof(hcmd_t),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_cmd_queue) {
        ESP_LOGE(TAG, "failed to create command queue");
        return;   /* no mailbox -> nothing could drive playback; give up */
    }
    s_media_queue = xQueueCreateWithCaps(MEDIA_QUEUE_DEPTH, sizeof(media_work_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_media_queue) {
        ESP_LOGE(TAG, "failed to create media worker queue");
        return;
    }
    if (xTaskCreatePinnedToCoreWithCaps(media_task, "ha_media", 8192, NULL, 3,
                                        &s_media_task_handle, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "failed to create media task in PSRAM");
        return;
    }
    if (xTaskCreatePinnedToCoreWithCaps(ha_task, "ha_task", 8192, NULL, 5,
                                        &s_ha_task_handle, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "failed to create HA task in PSRAM");
        return;
    }
    app_core_reliability_register_task("ha_media", s_media_task_handle, 8192);
    app_core_reliability_register_task("ha_task", s_ha_task_handle, 8192);
    app_core_reliability_checkpoint("application tasks ready");

    /* Boot: fetch covers for any runtime-added albums (ha_task drains this once
     * the network is up). */
    ui_request_refresh_covers();

#if KNOB_ENABLED
    knob_input_start();
#endif

    ESP_LOGI(TAG, "init done -- ha_task running");
    ESP_LOGI(TAG, "feature marker: ha_devices_v2_lights_presets_v1 add_albums_spotify_search_v1");
}
