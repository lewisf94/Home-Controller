/*
 * Music Controller -- ESP32-P4 (Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3),
 * DIRECT SPOTIFY backend (no Home Assistant).
 *
 * STATUS: checkpoint-5 -- album art. WiFi via the onboard ESP32-C6; a Spotify
 * task refreshes the OAuth token, polls /me/player every 5 s, and drives the
 * LVGL UI (ui.c): an album-browser carousel + a now-playing screen (320x320
 * art / title / artist / progress) laid out for 800x480. On a track change the
 * 640px cover is downloaded to LittleFS, JPEG-decoded /2 to 320x320 (PSRAM) and
 * shown. Tapping a card posts SCMD_PLAY_ALBUM to the scmd_t queue, drained by
 * the Spotify task off the render path. Physical controls (cp6) come later;
 * touch (GT911) is the only input so far.
 *
 * Memory: large HTTPS/JPEG buffers spill to PSRAM via the threshold policy in
 * sdkconfig.defaults (see docs/PORT-NOTES.md), so the 256 KB response buffer
 * never lands in the scarce 768 KB internal SRAM.
 *
 * Checkpoint roadmap (see README.md):
 *  1  Display skeleton (hardware-verified)
 *  2  WiFi (hardware-verified)
 *  3  Spotify (hardware-verified)
 *  4  UI: port cyd ui.c (lvgl_port_lock -> bsp_display_lock); 800x480 layout (hardware-verified)
 *  5  Assets: album_art.cpp + littlefs + bigger thumbs/art (album-art download)  <-- HERE
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
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "secrets.h"
#include "spotify.h"
#include "sonos.h"
#include "ui.h"
#include "album_art.h"
#include "littlefs.h"

static const char *TAG = "main";

/* Optional direct Sonos control (Spotify can't drive a Sonos -- it's a
 * restricted device, so we talk to it over UPnP). Configure in secrets.h:
 *   - one speaker:  #define SONOS_HOST "192.168.1.50"   (any restricted device
 *                   routes here)
 *   - several speakers / multiple Sonos systems: map each speaker's
 *     Spotify-reported name to its LAN IP --
 *       #define SONOS_DEVICES { "Living Room", "192.168.1.50" }, \
 *                             { "Bedroom",     "192.168.1.51" }
 * Leave both undefined to disable. The active device name is printed in the
 * "now playing" log so you can see exactly what string to map. */
typedef struct { const char *name; const char *host; } sonos_dev_t;
#if defined(SONOS_DEVICES)
static const sonos_dev_t s_sonos_devices[] = { SONOS_DEVICES };
#endif

/* Sonos LAN IP for the given active-device name, or NULL if it isn't a Sonos we
 * know how to control. */
static const char *sonos_host_for(const char *device_name)
{
#if defined(SONOS_DEVICES)
    for (size_t i = 0; i < sizeof s_sonos_devices / sizeof s_sonos_devices[0]; i++)
        if (strcmp(s_sonos_devices[i].name, device_name) == 0)
            return s_sonos_devices[i].host;
    return NULL;
#elif defined(SONOS_HOST)
    (void)device_name;
    return SONOS_HOST[0] ? SONOS_HOST : NULL;   /* single speaker: any restricted device */
#else
    (void)device_name;
    return NULL;
#endif
}

/* First configured Sonos that is currently PLAYING, or NULL if none is. Used to
 * find the speaker for now-playing when Spotify's /me/player can't see it
 * (Sonos-native playback is invisible to the Web API). */
static const char *sonos_playing_host(void)
{
#if defined(SONOS_DEVICES)
    for (size_t i = 0; i < sizeof s_sonos_devices / sizeof s_sonos_devices[0]; i++)
        if (sonos_is_playing(s_sonos_devices[i].host)) return s_sonos_devices[i].host;
    return NULL;
#elif defined(SONOS_HOST)
    return (SONOS_HOST[0] && sonos_is_playing(SONOS_HOST)) ? SONOS_HOST : NULL;
#else
    return NULL;
#endif
}

/* Which Sonos to start an album on: the one playing, else the first configured
 * (so a cold album-start still has a target). NULL if none is configured. */
static const char *sonos_target_host(void)
{
#if defined(SONOS_DEVICES)
    const char *p = sonos_playing_host();
    return p ? p : s_sonos_devices[0].host;
#elif defined(SONOS_HOST)
    return SONOS_HOST[0] ? SONOS_HOST : NULL;
#else
    return NULL;
#endif
}

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define WIFI_MAX_RETRY     5
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

/* Now-playing album art. Spotify serves 640x640; album_art.cpp decodes /2 to
 * 320x320 RGB565. The 200 KB buffer lives in PSRAM (internal SRAM is scarce).
 * The JPEG is staged in a LittleFS scratch file between download and decode. */
#define ART_W          320
#define ART_H          320
#define ART_RGB_BYTES  (ART_W * ART_H * 2)
#define ART_JPEG_PATH  "/littlefs/nowplaying.jpg"

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
    SCMD_GET_DEVICES,    /* fetch device list -> ui_set_devices */
    SCMD_TRANSFER,       /* str = Spotify device id to transfer playback to */
    SCMD_SELECT_SONOS,   /* str = Sonos LAN IP to drive over UPnP */
} scmd_type_t;

typedef struct {
    scmd_type_t  type;
    uint32_t     param;     /* seek_ms (SCMD_SEEK_MS) or volume_pct (SCMD_SET_VOLUME) */
    char         str[64];   /* album URI / device id / Sonos host -- copied, self-contained */
} scmd_t;

static QueueHandle_t s_cmd_queue = NULL;

/* Now-playing art handed to the UI. Double-buffered in PSRAM: the Spotify task
 * decodes into the idle buffer, then ui_art_refresh swaps lv_image to it under
 * the LVGL lock -- so the decode never writes the pixels the render task is
 * reading (that cross-core race on one buffer could corrupt LVGL state).
 * s_art_url_loaded de-dupes downloads so we only fetch on a real art change. */
static uint8_t        *s_art_rgb[2] = { NULL, NULL };
static int             s_art_buf    = 0;
static lv_image_dsc_t  s_art_dsc = {0};
static char            s_art_url_loaded[256] = {0};

/* LAN IP of the Sonos the controller is currently driving (album started on it,
 * or /me/player named it as a restricted device), or "" for none. Drives both
 * command routing and the now-playing fallback when Spotify can't see playback.
 * Only the spotify_task touches it. */
static char            s_sonos_active[24] = {0};
/* True when the user explicitly chose a Sonos from the device selector. Prevents
 * the Spotify poll from auto-clearing s_sonos_active just because a Spotify
 * device is also active. Cleared when the user transfers to a Spotify device. */
static bool            s_sonos_explicit   = false;
/* True when the Sonos at s_sonos_active is confirmed to have audio: either
 * sonos_fetch_now_playing() returned a real track, or /me/player says the active
 * device is restricted (Sonos in Spotify Connect). Only when this is true do
 * transport commands (play/pause/next/prev/seek/volume) route over UPnP.
 * Without this guard, selecting a Sonos with an empty queue would silently break
 * play/pause for whatever was actually playing on the laptop. */
static bool            s_sonos_has_audio  = false;
/* Earliest tick at which to re-probe configured speakers for one playing
 * natively (when Spotify is 204 and we aren't already driving a Sonos). Backoff
 * keeps a powered-off speaker from being hammered with connect attempts. */
static TickType_t      s_sonos_probe_next = 0;

/* Bounded, always-NUL-terminating copy via an explicit loop. Hand-rolled (not
 * snprintf/strncpy) so GCC's -Werror=format-truncation / -Werror=stringop-
 * truncation -- which can't bound array/struct sources and reject any
 * potentially-truncating copy -- don't fail the build. Intentional truncation. */
static void copy_str(char *dst, size_t dstsz, const char *src)
{
    if (dstsz == 0) return;
    size_t i = 0;
    if (src) for (; i + 1 < dstsz && src[i] != '\0'; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static void _post_cmd(scmd_type_t type, uint32_t param, const char *str)
{
    if (!s_cmd_queue) return;
    scmd_t cmd = { .type = type, .param = param };
    if (str) { strncpy(cmd.str, str, sizeof cmd.str - 1); cmd.str[sizeof cmd.str - 1] = '\0'; }
    (void)xQueueSend(s_cmd_queue, &cmd, 0);
}

void ui_request_play(const char *uri)  { _post_cmd(SCMD_PLAY_ALBUM,  0,             uri);  }
void ui_request_toggle_play(void)      { _post_cmd(SCMD_TOGGLE_PLAY, 0,             NULL); }
void ui_request_prev(void)             { _post_cmd(SCMD_PREV_TRACK,  0,             NULL); }
void ui_request_next(void)             { _post_cmd(SCMD_NEXT_TRACK,  0,             NULL); }
void ui_request_seek(uint32_t ms)      { _post_cmd(SCMD_SEEK_MS,     ms,            NULL); }
void ui_request_volume(int pct)        { _post_cmd(SCMD_SET_VOLUME,  (uint32_t)pct, NULL); }
void ui_request_get_devices(void)              { _post_cmd(SCMD_GET_DEVICES,   0, NULL); }
void ui_request_transfer(const char *id)       { _post_cmd(SCMD_TRANSFER,      0, id);   }
void ui_request_select_sonos(const char *host) { _post_cmd(SCMD_SELECT_SONOS,  0, host); }

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

/* Decode the JPEG staged at ART_JPEG_PATH into s_art_rgb (PSRAM) and hand the
 * buffer to ui.c, which republishes it under the LVGL lock. */
static bool decode_and_publish_art(void)
{
    /* Decode into the idle buffer, then swap; never touch the buffer the
     * render task is currently displaying. */
    int next = s_art_buf ^ 1;
    uint8_t *buf = s_art_rgb[next];
    if (!buf) return false;

    uint16_t w = 0, h = 0;
    if (!album_art_decode_file(ART_JPEG_PATH,
                               (uint16_t *)buf, ART_W * ART_H, &w, &h)) {
        ESP_LOGW(TAG, "jpeg decode failed");
        return false;
    }
    ESP_LOGI(TAG, "decoded %ux%u album art (%u bytes)",
             (unsigned)w, (unsigned)h, (unsigned)(w * h * 2));
    ui_art_refresh(buf, w, h);
    s_art_buf = next;
    return true;
}

/* Fetch /me/player once and push it to the UI; on a track change download +
 * decode the new cover. If Spotify can't see playback (204) but we're driving a
 * Sonos, fall back to the Sonos's own now-playing over UPnP. Returns true if
 * any playback is active. */
static bool poll_and_publish(spotify_track_t *info)
{
    bool spotify_ok = spotify_fetch_player(info);

    if (spotify_ok) {
        /* Track which Sonos to drive: a restricted Connect device we have a
         * name->IP mapping for sets it; a controllable phone/laptop clears it
         * UNLESS the user explicitly chose a Sonos (s_sonos_explicit). */
        if (info->device_restricted) {
            const char *h = sonos_host_for(info->device_name);
            if (h) snprintf(s_sonos_active, sizeof s_sonos_active, "%s", h);
        } else if (!s_sonos_explicit) {
            s_sonos_active[0] = '\0';
        }
        ESP_LOGI(TAG, "now playing: %s -- %s [%lu/%lu ms, %s] @%s%s",
                 info->artist, info->title,
                 (unsigned long)info->progress_ms,
                 (unsigned long)info->duration_ms,
                 info->is_playing ? "playing" : "paused",
                 info->device_name[0] ? info->device_name : "?",
                 info->device_restricted ? " (restricted)" : "");
    }

    /* Spotify 204: probe for a Sonos playing natively (e.g. started from the
     * Spotify app or Sonos app). Backoff avoids hammering powered-off speakers. */
    if (!spotify_ok && !s_sonos_active[0] &&
        (int32_t)(xTaskGetTickCount() - s_sonos_probe_next) >= 0) {
        const char *h = sonos_playing_host();
        if (h) snprintf(s_sonos_active, sizeof s_sonos_active, "%s", h);
        else   s_sonos_probe_next = xTaskGetTickCount() + pdMS_TO_TICKS(10000);
    }

    /* If we have a Sonos target (restricted Connect, explicit user pick, or
     * native probe), read its now-playing over UPnP. This takes priority over
     * the Spotify desktop view so the screen reflects what the Sonos is doing. */
    if (s_sonos_active[0]) {
        sonos_np_t np;
        if (sonos_fetch_now_playing(s_sonos_active, &np)) {
            s_sonos_has_audio = true;
            memset(info, 0, sizeof *info);
            snprintf(info->title,  sizeof info->title,  "%s", np.title);
            snprintf(info->artist, sizeof info->artist, "%s", np.artist);
            snprintf(info->album,  sizeof info->album,  "%s", np.album);
            info->progress_ms       = np.progress_ms;
            info->duration_ms       = np.duration_ms;
            info->is_playing        = np.is_playing;
            info->volume_pct        = np.volume;
            info->device_restricted = true;
            snprintf(info->device_name, sizeof info->device_name, "Sonos");
            ESP_LOGI(TAG, "now playing (Sonos %s): %s -- %s [%lu/%lu ms, %s]",
                     s_sonos_active, info->artist, info->title,
                     (unsigned long)info->progress_ms,
                     (unsigned long)info->duration_ms,
                     info->is_playing ? "playing" : "paused");
            ui_set_track_info(info);
            return true;
        }
        /* Sonos unreachable or stopped (NOT_IMPLEMENTED in Connect passthrough
         * counts as stopped -- we fall back to Spotify for track display). */
        s_sonos_has_audio = false;
        if (!s_sonos_explicit) {
            s_sonos_active[0] = '\0';
            s_sonos_probe_next = xTaskGetTickCount() + pdMS_TO_TICKS(10000);
        }
    }

    if (spotify_ok) {
        /* Restricted device = Sonos in Spotify Connect: audio IS on the Sonos,
         * UPnP transport commands will work. Non-restricted = laptop/phone,
         * route commands via Spotify API even if s_sonos_active is set. */
        s_sonos_has_audio = info->device_restricted && s_sonos_active[0];
        ui_set_track_info(info);
        if (info->album_art_url[0] &&
            strcmp(info->album_art_url, s_art_url_loaded) != 0 &&
            littlefs_is_mounted()) {
            size_t bytes = 0;
            if (spotify_download_to_file(info->album_art_url, ART_JPEG_PATH, &bytes)) {
                ESP_LOGI(TAG, "downloaded %u bytes -> %s", (unsigned)bytes, ART_JPEG_PATH);
                if (decode_and_publish_art()) {
                    strncpy(s_art_url_loaded, info->album_art_url, sizeof(s_art_url_loaded) - 1);
                    s_art_url_loaded[sizeof(s_art_url_loaded) - 1] = '\0';
                }
            }
        }
        return true;
    }

    s_sonos_has_audio = false;
    ESP_LOGI(TAG, "no active playback (or fetch failed)");
    ui_set_track_info(NULL);
    return false;
}

/* Drains s_cmd_queue and runs the blocking Spotify HTTPS calls off the
 * render/input path. Polls /me/player every 5 s. After a track-changing command
 * (next/prev/play) Spotify's /me/player keeps reporting the OLD track for a
 * moment, so a single immediate poll would show stale title/artist until the
 * next 5 s tick (~6 s of lag the user sees). Instead we "settle": re-poll a few
 * times (~300 ms apart, capped ~2 s) until the title actually changes, so the
 * on-screen details catch up within ~1 s. Non-track commands just refresh once. */
static void spotify_task(void *arg)
{
    (void)arg;
    spotify_init(SPOTIFY_CLIENT_ID, SPOTIFY_CLIENT_SECRET, SPOTIFY_REFRESH_TOKEN);

    spotify_track_t info = {0};
    bool settle = false;
    char prev_title[sizeof info.title] = {0};

    while (1) {
        if (settle) {
            settle = false;
            for (int i = 0; i < 5; i++) {
                poll_and_publish(&info);
                if (strncmp(info.title, prev_title, sizeof prev_title) != 0) break;
                vTaskDelay(pdMS_TO_TICKS(300));
            }
        } else {
            poll_and_publish(&info);
        }

        /* Poll every 5 s, but wake early to service any queued command. */
        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(5000);
        for (;;) {
            scmd_t cmd = {0};
            TickType_t now  = xTaskGetTickCount();
            TickType_t wait = (now >= deadline) ? 0 : (deadline - now);
            if (xQueueReceive(s_cmd_queue, &cmd, wait) == pdTRUE) {
                bool ok = false;
                /* Route transport commands to the Sonos ONLY when it is confirmed
                 * to have audio (s_sonos_has_audio). Without this guard, selecting
                 * a Sonos with an empty queue silently breaks play/pause for
                 * whatever is actually playing on the laptop/phone. */
                const char *sh = (s_sonos_active[0] && s_sonos_has_audio)
                                 ? s_sonos_active : NULL;
                switch (cmd.type) {
                    case SCMD_PLAY_ALBUM: {
                        /* If the user has selected (or auto-detected) a Sonos,
                         * always start the album on it via UPnP regardless of
                         * whether a controllable Spotify device is also active.
                         * Otherwise use the Spotify Web API on the active device,
                         * or fall back to any configured Sonos target. */
                        const char *target = s_sonos_active[0] ? s_sonos_active
                                           : ((!info.device_restricted && info.device_name[0])
                                              ? NULL : sonos_target_host());
                        if (target) {
                            ok = sonos_play_spotify_album(target, cmd.str);
                            ESP_LOGI(TAG, "album-on-Sonos[%s] %s -> %s",
                                     target, cmd.str, ok ? "ok" : "FAILED");
                            if (ok) snprintf(s_sonos_active, sizeof s_sonos_active, "%s", target);
                            else    sonos_log_diag(target);  /* dump state to debug */
                        } else {
                            ok = spotify_play_album(cmd.str);
                            ESP_LOGI(TAG, "play_album(%s) -> %s", cmd.str, ok ? "ok" : "FAILED");
                        }
                        break;
                    }
                    case SCMD_TOGGLE_PLAY:
                        ok = sh ? (info.is_playing ? sonos_pause(sh) : sonos_play(sh))
                                : spotify_toggle_play_pause();
                        break;
                    case SCMD_PREV_TRACK:
                        ok = sh ? sonos_previous(sh) : spotify_prev_track();
                        break;
                    case SCMD_NEXT_TRACK:
                        ok = sh ? sonos_next(sh) : spotify_next_track();
                        break;
                    case SCMD_SEEK_MS:
                        ok = sh ? sonos_seek_ms(sh, cmd.param)
                                : spotify_seek_position(cmd.param);
                        break;
                    case SCMD_SET_VOLUME:
                        ok = sh ? sonos_set_volume(sh, (int)cmd.param)
                                : spotify_set_volume((int)cmd.param);
                        break;
                    case SCMD_GET_DEVICES: {
                        /* Combined picker: Spotify Connect devices (transfer
                         * targets) + configured Sonos speakers (UPnP targets).
                         * Static locals keep these arrays off the TLS stack. */
                        static ui_device_t      list[16];
                        static spotify_device_t sp[8];
                        int n = 0, sc = 0;
                        if (spotify_get_devices(sp, 8, &sc)) {
                            for (int i = 0; i < sc && n < 16; i++, n++) {
                                copy_str(list[n].name,   sizeof list[n].name,   sp[i].name);
                                copy_str(list[n].detail, sizeof list[n].detail, sp[i].type);
                                copy_str(list[n].id,     sizeof list[n].id,     sp[i].id);
                                list[n].is_active = sp[i].is_active;
                                list[n].is_sonos  = false;
                            }
                        }
#if defined(SONOS_DEVICES)
                        for (size_t i = 0;
                             i < sizeof s_sonos_devices / sizeof s_sonos_devices[0] && n < 16;
                             i++, n++) {
                            snprintf(list[n].name,   sizeof list[n].name,   "%s", s_sonos_devices[i].name);
                            snprintf(list[n].detail, sizeof list[n].detail, "Sonos");
                            snprintf(list[n].id,     sizeof list[n].id,     "%s", s_sonos_devices[i].host);
                            list[n].is_active = (strcmp(s_sonos_devices[i].host, s_sonos_active) == 0);
                            list[n].is_sonos  = true;
                        }
#elif defined(SONOS_HOST)
                        if (SONOS_HOST[0] && n < 16) {
                            snprintf(list[n].name,   sizeof list[n].name,   "Sonos");
                            snprintf(list[n].detail, sizeof list[n].detail, "Sonos");
                            snprintf(list[n].id,     sizeof list[n].id,     "%s", SONOS_HOST);
                            list[n].is_active = (strcmp(SONOS_HOST, s_sonos_active) == 0);
                            list[n].is_sonos  = true;
                            n++;
                        }
#endif
                        ui_set_devices(list, n);
                        ESP_LOGI(TAG, "devices: %d spotify + sonos -> %d total", sc, n);
                        break;
                    }
                    case SCMD_TRANSFER:
                        ok = spotify_transfer_playback(cmd.str);
                        if (ok) {
                            s_sonos_active[0] = '\0';
                            s_sonos_explicit  = false;  /* back on a Spotify device */
                        }
                        ESP_LOGI(TAG, "transfer -> %s: %s", cmd.str, ok ? "ok" : "FAILED");
                        break;
                    case SCMD_SELECT_SONOS:
                        copy_str(s_sonos_active, sizeof s_sonos_active, cmd.str);
                        s_sonos_explicit   = true;   /* user chose this -- poll won't clear it */
                        s_sonos_probe_next = 0;      /* fetch its now-playing now */
                        /* Resume the Sonos queue if something was previously loaded on it.
                         * If nothing is queued, sonos_play() returns false silently; the
                         * user can start an album from the browser. */
                        ok = sonos_play(cmd.str);
                        ESP_LOGI(TAG, "select sonos -> %s (resume: %s)",
                                 cmd.str, ok ? "ok" : "nothing queued");
                        ok = true;  /* selection itself always succeeds */
                        break;
                }
                (void)ok;
                /* Arm settle on the commands that change the current track, so the
                 * loop top re-polls until the new title lands. */
                if (cmd.type == SCMD_NEXT_TRACK || cmd.type == SCMD_PREV_TRACK ||
                    cmd.type == SCMD_PLAY_ALBUM  || cmd.type == SCMD_TRANSFER ||
                    cmd.type == SCMD_SELECT_SONOS) {
                    strncpy(prev_title, info.title, sizeof prev_title - 1);
                    prev_title[sizeof prev_title - 1] = '\0';
                    settle = true;
                }
                break;
            }
            if (xTaskGetTickCount() >= deadline) break;
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Music Controller P4 (direct Spotify) -- checkpoint 5: album art");

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

    /* Now-playing art buffer (PSRAM -- internal SRAM is scarce) + LittleFS
     * scratch for the downloaded JPEG. Both ready before spotify_task decodes. */
    s_art_rgb[0] = heap_caps_malloc(ART_RGB_BYTES, MALLOC_CAP_SPIRAM);
    s_art_rgb[1] = heap_caps_malloc(ART_RGB_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_art_rgb[0] || !s_art_rgb[1])
        ESP_LOGE(TAG, "failed to allocate %d-byte art buffers", ART_RGB_BYTES);
    if (!littlefs_mount()) ESP_LOGW(TAG, "littlefs mount failed -- album art disabled");

    /* Display first so we see something while WiFi connects. */
    bsp_display_cfg_t cfg = {
        .lv_adapter_cfg  = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation        = ESP_LV_ADAPTER_ROTATE_90,
        /* Full-frame triple buffering (not TRIPLE_PARTIAL): an animated screen
         * transition redraws the whole panel every frame, and partial-refresh
         * stalled the flush there -- the UI froze on a swipe out of now-playing
         * (heavy because the 320x320 art slides too). ROTATE_90 needs 3 buffers
         * either way, so this keeps tear-avoidance at the same memory cost. */
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_FULL,
        /* Touch is NOT auto-rotated by the adapter -- these flags must match the
         * ROTATE_90 display. GT911 reports native 480x800 portrait; swap_xy=1 is
         * required (without it a horizontal swipe reads as vertical and taps/
         * scroll miss). A 90deg rotation is swap + exactly one mirror; the two
         * choices differ by 180deg. swap_xy=1,mirror_x=0,mirror_y=1 came out
         * fully inverted on hardware, so the correct handedness is the other:
         * mirror_x=1, mirror_y=0 (hardware-confirmed). */
        .touch_flags     = { .swap_xy = 1, .mirror_x = 1, .mirror_y = 0 },
    };
    bsp_display_start_with_config(&cfg);
    bsp_display_backlight_on();

    bsp_display_lock(-1);
    lv_obj_t *status_label = lv_label_create(lv_screen_active());
    lv_label_set_text(status_label, "Music Controller P4\ncheckpoint 5: WiFi connecting...");
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(status_label);
    bsp_display_unlock();

    /* Let the ESP32-C6 WiFi slave boot its esp_hosted firmware before esp_wifi_init. */
    vTaskDelay(pdMS_TO_TICKS(3000));

    if (wifi_init_sta() != ESP_OK) {
        ESP_LOGE(TAG, "wifi did not connect -- cannot reach Spotify");
        bsp_display_lock(-1);
        lv_label_set_text(status_label, "Music Controller P4\ncheckpoint 5: WiFi FAILED\ncheck secrets.h SSID/password");
        bsp_display_unlock();
        return;
    }

    /* Build the LVGL UI (browser + now-playing) and load the browser. This
     * replaces the startup status_label screen. ui_init locks internally, so
     * it must run with the display lock released. */
    ui_init(&s_art_dsc);

    s_cmd_queue = xQueueCreate(8, sizeof(scmd_t));
    if (!s_cmd_queue) {
        ESP_LOGE(TAG, "failed to create command queue");
        return;
    }
    /* 10 KB stack: TLS handshake (cert-bundle validation) on top of the
     * esp_hosted transport is stack-hungry. CYD used 8 KB on native WiFi. */
    xTaskCreate(spotify_task, "spotify", 10240, NULL, 5, NULL);

    ESP_LOGI(TAG, "checkpoint 5: UI + art, Spotify task started");
}
