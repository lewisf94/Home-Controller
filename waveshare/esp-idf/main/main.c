/*
 * Music Controller -- firmware for a handheld Spotify remote built on the
 * Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3 board (a small computer with a touch
 * screen). This file, main.c, is the STARTING POINT: app_main() near the bottom
 * runs once at power-on, sets everything up, then launches the background
 * workers that run forever. If you are new to the project, read this header
 * first -- it explains how the whole thing fits together.
 *
 * --- The big picture ---
 * The ESP32-P4 has two CPU cores. We split the work into a few independent
 * "tasks" (think of them as lightweight threads the chip rapidly switches
 * between) so that a slow job never freezes another. The important ones:
 *
 *   - The LVGL task: does ALL the on-screen drawing -- the album browser, the
 *     now-playing screen, animations. ("LVGL" is the graphics library; it is
 *     started for us inside the display driver / BSP, not in this file.)
 *   - spotify_task (in this file): does the SLOW work -- talking to Spotify
 *     over the internet (HTTPS), which can take a second or two per call. It
 *     runs separately so the screen stays smooth while it waits on the network.
 *   - A small audio task (audio.c): plays the UI click/beep sounds.
 *
 * --- How a tap becomes music without freezing the screen ---
 * When you tap an album, the UI does NOT call Spotify directly (that would
 * block drawing for a second). Instead it drops a small message -- a command,
 * type `scmd_t`, e.g. SCMD_PLAY_ALBUM -- into a QUEUE (a thread-safe mailbox,
 * `s_cmd_queue`). spotify_task takes commands out one at a time and makes the
 * real network call. Information flows ONE way: UI/input -> queue -> spotify_task.
 *
 * --- The "display lock" rule ---
 * Only one task may touch on-screen objects at a time. So any code OUTSIDE the
 * LVGL task that needs to update the UI (e.g. spotify_task showing a new track
 * title) must first take a lock, make its change, then release it. The ui_*()
 * helper functions in ui.c handle that locking for you -- call those, never
 * poke LVGL objects directly from here.
 *
 * --- Speakers / Sonos ---
 * Normally Spotify plays on a phone, laptop, or Spotify Connect speaker. A Sonos
 * is a special case: Spotify's API refuses to control it, so when a Sonos is the
 * chosen target we talk to it ourselves over the local network (see sonos.c).
 *
 * --- Memory note ---
 * The chip has a little fast memory (768 KB "internal SRAM") and a lot of slower
 * memory ("PSRAM"). Big buffers -- the 256 KB web-response buffer, the album art
 * -- are steered into PSRAM by a rule in sdkconfig.defaults so they do not use
 * up the scarce fast memory. (Full explanation in docs/PORT-NOTES.md.)
 *
 * Status: the UI is feature-complete and hardware-verified. The original
 * bring-up steps (display -> WiFi -> Spotify -> UI -> album art) are listed in
 * README.md. Physical knob/button controls are future work; touch is the only
 * input today.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "nvs_flash.h"
#include "esp_log.h"
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
#include "audio.h"
#include "knob_input.h"
#include "app_core_wifi.h"
#include "app_core_art.h"

static const char *TAG = "main";

#define CREDS_KEY_WIFI_SSID  "wifi_ssid"
#define CREDS_KEY_WIFI_PASS  "wifi_pass"
#define CREDS_KEY_SP_ID      "sp_id"
#define CREDS_KEY_SP_SECRET  "sp_secret"
#define CREDS_KEY_SP_REFRESH "sp_refresh"
#define CREDS_VAL_MAX 300
bool creds_get(const char *key, char *out, size_t out_len, const char *fallback);

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

/* Now-playing album art. Spotify serves 640x640; album_art.cpp decodes /2 to
 * 320x320 RGB565. The 200 KB buffer lives in PSRAM (internal SRAM is scarce).
 * The JPEG is staged in a LittleFS scratch file between download and decode. */
#define ART_W          320
#define ART_H          320
#define ART_RGB_BYTES  (ART_W * ART_H * 2)
#define ART_JPEG_PATH  "/littlefs/nowplaying.jpg"

/* A/B switch for the album-art decode path (see fetch_and_publish_art):
 *   0 (default) -- download to a LittleFS file, decode via JPEG_openFile. The
 *                  proven path: JPEGDEC 1.6.2's openRAM mis-handles some of
 *                  Spotify's mozjpeg streams, which is why this file detour exists.
 *   1           -- decode straight from a PSRAM download buffer (no flash, no
 *                  LittleFS for art). Set this to validate openRAM on the actual
 *                  covers ON HARDWARE before committing to removing LittleFS.
 *                  LittleFS still mounts either way -- flipping this flag changes
 *                  nothing else, so it's a clean on-device comparison. */
#define ART_DECODE_RAM 0

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
    SCMD_GET_ALBUM_CANDIDATES, /* fetch saved-library albums -> ui_set_album_candidates */
    SCMD_SEARCH_ALBUMS,  /* str = search text -> Spotify /v1/search -> ui_set_album_candidates */
    SCMD_REFRESH_COVERS, /* fetch+decode covers for runtime-added albums */
    SCMD_TRANSFER,       /* str = Spotify device id to transfer playback to */
    SCMD_SELECT_SONOS,   /* str = Sonos LAN IP to drive over UPnP */
    SCMD_TOGGLE_SHUFFLE,
} scmd_type_t;

typedef struct {
    scmd_type_t  type;
    uint32_t     param;     /* seek_ms (SCMD_SEEK_MS) or volume_pct (SCMD_SET_VOLUME) */
    char         str[64];   /* album URI / device id / Sonos host -- copied, self-contained */
} scmd_t;

/* Per-command metadata: name for logging, and whether the handler already logs
 * both the success and failure paths (so the generic fallback skips it). */
typedef struct { const char *name; bool self_logs; } scmd_meta_t;
static const scmd_meta_t k_scmd_meta[] = {
    [SCMD_PLAY_ALBUM]     = { "play_album",       true  },
    [SCMD_TOGGLE_PLAY]    = { "toggle_play_pause", false },
    [SCMD_PREV_TRACK]     = { "prev_track",        false },
    [SCMD_NEXT_TRACK]     = { "next_track",        false },
    [SCMD_SEEK_MS]        = { "seek",              false },
    [SCMD_SET_VOLUME]     = { "set_volume",        false },
    [SCMD_GET_DEVICES]    = { "get_devices",       true  },
    [SCMD_GET_ALBUM_CANDIDATES] = { "get_album_candidates", true },
    [SCMD_SEARCH_ALBUMS]  = { "search_albums",     true  },
    [SCMD_REFRESH_COVERS] = { "refresh_covers",    true  },
    [SCMD_TRANSFER]       = { "transfer",          true  },
    [SCMD_SELECT_SONOS]   = { "select_sonos",      true  },
    [SCMD_TOGGLE_SHUFFLE] = { "toggle_shuffle",    false },
};
_Static_assert(sizeof k_scmd_meta / sizeof k_scmd_meta[0] == SCMD_TOGGLE_SHUFFLE + 1,
               "k_scmd_meta is missing an entry -- update when adding a new scmd_type_t");

static QueueHandle_t s_cmd_queue = NULL;
#define ALBUM_CANDIDATE_MAX 16

/* `err` NULL = success; else a short human reason shown on the add screen. */
void ui_set_album_candidates(const void *list, int count, const char *err);

/* Runtime album catalogue + cover fetch (p4_shared/album_catalog.c + ui.c).
 * No shared header while the runtime catalogue is a prototype -- declared here
 * (mirrors the ui_set_album_candidates pattern above). */
#include "album_thumbs.h"          /* ALBUM_THUMB_W / _H / _BYTES */
size_t album_catalog_runtime_count(void);
bool   album_catalog_runtime_art_todo(size_t rt_idx, char *url_out, size_t url_len);
bool   album_catalog_set_thumb(size_t rt_idx, const uint16_t *rgb);
void   ui_notify_covers_updated(void);
#define RT_ART_PATH "/littlefs/rtart.jpg"   /* transient scratch for cover fetch */

/* Now-playing art handed to the UI. Double-buffered in PSRAM: the Spotify task
 * decodes into the idle buffer, then ui_art_refresh swaps lv_image to it under
 * the LVGL lock -- so the decode never writes the pixels the render task is
 * reading (that cross-core race on one buffer could corrupt LVGL state).
 * s_art_url_loaded de-dupes downloads so we only fetch on a real art change. */
static art_buffer_t    s_art = {0};
static lv_image_dsc_t  s_art_dsc = {0};
static char            s_art_url_loaded[256] = {0};
/* URLs whose JPEG decode failed deterministically. Without this, a malformed /
 * unsupported cover gets re-downloaded + re-decoded every 5 s for the whole
 * track (wasted bandwidth, flash writes, log spam). */
static char            s_art_url_failed[256] = {0};

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
/* After a failed now-playing fetch, hold off the next attempt this long even for
 * an explicitly-picked Sonos (which the poll otherwise never stops re-fetching),
 * so a powered-off pinned speaker can't stall the poll every cycle. */
static TickType_t      s_sonos_fetch_hold = 0;



/* --- Posting commands to the Spotify task (the "mailbox") ---
 * When the user does something, the UI (and, later, physical buttons) call one
 * of the ui_request_*() functions below. Those do NOT make the slow network
 * call here -- they package the request into a small `scmd_t` struct and drop it
 * into the queue (s_cmd_queue). spotify_task collects it later and does the real
 * work. This is what keeps taps responsive: posting is instant, the waiting
 * happens on the other task. */
static void _post_cmd(scmd_type_t type, uint32_t param, const char *str)
{
    if (!s_cmd_queue) return;                            /* not set up yet -> ignore */
    scmd_t cmd = { .type = type, .param = param };
    if (str) { strlcpy(cmd.str, str, sizeof cmd.str); }  /* copy the string INTO the command,
                                                          * so the caller's buffer can vanish */
    (void)xQueueSend(s_cmd_queue, &cmd, 0);              /* the 0 = "post and return at once,
                                                          * never wait"; dropped if queue full */
}

/* The UI (see ui.h) calls these; each just enqueues one command and returns. */
void ui_request_play(const char *uri)  { _post_cmd(SCMD_PLAY_ALBUM,     0,             uri);  }
void ui_request_toggle_play(void)      { _post_cmd(SCMD_TOGGLE_PLAY,    0,             NULL); }
void ui_request_prev(void)             { _post_cmd(SCMD_PREV_TRACK,     0,             NULL); }
void ui_request_next(void)             { _post_cmd(SCMD_NEXT_TRACK,     0,             NULL); }
void ui_request_seek(uint32_t ms)      { _post_cmd(SCMD_SEEK_MS,        ms,            NULL); }
void ui_request_volume(int pct)        { _post_cmd(SCMD_SET_VOLUME,     (uint32_t)pct, NULL); }
void ui_request_shuffle(void)          { _post_cmd(SCMD_TOGGLE_SHUFFLE, 0,             NULL); }
void ui_request_get_devices(void)              { _post_cmd(SCMD_GET_DEVICES,   0, NULL); }
void ui_request_get_album_candidates(void)     { _post_cmd(SCMD_GET_ALBUM_CANDIDATES, 0, NULL); }
void ui_request_search_album_candidates(const char *query)
{
    if (query && query[0]) _post_cmd(SCMD_SEARCH_ALBUMS, 0, query);
    else                   ui_request_get_album_candidates();
}
void ui_request_refresh_covers(void)          { _post_cmd(SCMD_REFRESH_COVERS, 0, NULL); }
void ui_request_transfer(const char *id)       { _post_cmd(SCMD_TRANSFER,      0, id);   }
void ui_request_select_sonos(const char *host) { _post_cmd(SCMD_SELECT_SONOS,  0, host); }

/* Lights are an HA-only concept (waveshare/esp-idf-ha/main/ha_client.c) -- this
 * build talks straight to the Spotify Web API and has no lights backend, so
 * the seam is a no-op here (mirrors how the HA build no-ops
 * ui_request_select_sonos the other way). The shared UI only exposes the
 * Lights entry point when the HA build defines P4_HAS_HA_LIGHTS. */
void ui_request_get_lights(void)                                 { }
void ui_request_light_toggle(const char *entity_id)               { (void)entity_id; }
void ui_request_light_brightness(const char *entity_id, int pct)  { (void)entity_id; (void)pct; }
void ui_request_light_hue(const char *entity_id, int hue_deg, int sat_pct)
{
    (void)entity_id;
    (void)hue_deg;
    (void)sat_pct;
}

/* WiFi connect (with resilient background reconnect) and the connect chime
 * (first successful connection only, whether that's this call or a later
 * background reconnect) now live in app_core_wifi -- see wifi.c. */
static void on_wifi_first_connect(void)
{
    audio_play(AUDIO_SFX_CONNECT);
    /* Fetch covers for any runtime-added albums now the network is up (the
     * queue exists by the time DHCP completes; _post_cmd no-ops if not). */
    ui_request_refresh_covers();
}

/* Download the cover at `url`, decode it into the idle art buffer (PSRAM)
 * and hand it to ui.c, which republishes under the LVGL lock. Decodes into the
 * idle buffer then swaps, so the render task's current buffer is never touched.
 * The download+decode path is selected by ART_DECODE_RAM (see its definition).
 * Returns: 1 = published, 0 = decode failed (record-and-skip this url),
 * -1 = download failed (transient -- retry next poll). */
static int fetch_and_publish_art(const char *url)
{
    uint8_t *buf = art_buffer_idle(&s_art);
    if (!buf) return -1;

    uint16_t w = 0, h = 0;
#if ART_DECODE_RAM
    size_t len = 0;
    unsigned char *jpeg = spotify_download_bytes(url, &len);
    if (!jpeg) return -1;
    ESP_LOGI(TAG, "downloaded %u bytes (RAM decode)", (unsigned)len);
    bool ok = album_art_decode(jpeg, len, (uint16_t *)buf, ART_W * ART_H, &w, &h);
    free(jpeg);
#else
    size_t bytes = 0;
    if (!spotify_download_to_file(url, ART_JPEG_PATH, &bytes)) return -1;
    ESP_LOGI(TAG, "downloaded %u bytes -> %s", (unsigned)bytes, ART_JPEG_PATH);
    bool ok = album_art_decode_file(ART_JPEG_PATH,
                                    (uint16_t *)buf, ART_W * ART_H, &w, &h);
#endif

    if (!ok) {
        ESP_LOGW(TAG, "jpeg decode failed");
        return 0;
    }
    ESP_LOGI(TAG, "decoded %ux%u album art (%u bytes)",
             (unsigned)w, (unsigned)h, (unsigned)(w * h * 2));
    art_buffer_publish(&s_art, w, h);
    return 1;
}

/* Fetch + decode browser thumbnails for any runtime-added albums that don't
 * have art yet. Runs on the Spotify task (network + JPEG decode, off the UI).
 * Triggered on first WiFi connect (boot) and after each ADD. */
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
        if (!spotify_download_to_file(url, RT_ART_PATH, &bytes) || bytes == 0) {
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

/* Gate the per-poll "now playing" INFO lines to actual state changes (track,
 * device or play/pause) so a steady-state serial log stays readable: without
 * this they repeat every 5-15 s forever. The line still fires whenever the
 * device NAME changes, preserving the documented way to discover Sonos speaker
 * names for secrets.h. Call with the polled info (all-empty on idle/fail --
 * the playing->idle transition then logs exactly once too). */
static bool np_state_changed(const spotify_track_t *info)
{
    static spotify_track_t last;
    if (strcmp(info->title, last.title) == 0 &&
        strcmp(info->device_name, last.device_name) == 0 &&
        info->is_playing == last.is_playing) {
        return false;
    }
    strlcpy(last.title, info->title, sizeof last.title);
    strlcpy(last.device_name, info->device_name, sizeof last.device_name);
    last.is_playing = info->is_playing;
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
        if (np_state_changed(info)) {
            ESP_LOGI(TAG, "now playing: %s -- %s [%lu/%lu ms, %s] @%s%s",
                     info->artist, info->title,
                     (unsigned long)info->progress_ms,
                     (unsigned long)info->duration_ms,
                     info->is_playing ? "playing" : "paused",
                     info->device_name[0] ? info->device_name : "?",
                     info->device_restricted ? " (restricted)" : "");
        }
    }

    /* Spotify 204: probe for a Sonos playing natively (e.g. started from the
     * Spotify app or Sonos app). Backoff avoids hammering powered-off speakers.
     * The "(int32_t)(now - deadline) >= 0" form just means "has the deadline
     * passed?" -- written this way so it stays correct even when the millisecond
     * tick counter eventually wraps back to zero (a plain now >= deadline would
     * misbehave right at that wrap). */
    if (!spotify_ok && !s_sonos_active[0] &&
        (int32_t)(xTaskGetTickCount() - s_sonos_probe_next) >= 0) {
        const char *h = sonos_playing_host();
        if (h) snprintf(s_sonos_active, sizeof s_sonos_active, "%s", h);
        else   s_sonos_probe_next = xTaskGetTickCount() + pdMS_TO_TICKS(10000);
    }

    /* If we have a Sonos target (restricted Connect, explicit user pick, or
     * native probe), read its now-playing over UPnP. This takes priority over
     * the Spotify desktop view so the screen reflects what the Sonos is doing. */
    if (s_sonos_active[0] &&
        (int32_t)(xTaskGetTickCount() - s_sonos_fetch_hold) >= 0) {
        sonos_np_t np;
        if (sonos_fetch_now_playing(s_sonos_active, &np)) {
            s_sonos_has_audio  = true;
            s_sonos_fetch_hold = 0;   /* reachable -- resume per-poll fetch */
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
            if (np_state_changed(info)) {
                ESP_LOGI(TAG, "now playing (Sonos %s): %s -- %s [%lu/%lu ms, %s]",
                         s_sonos_active, info->artist, info->title,
                         (unsigned long)info->progress_ms,
                         (unsigned long)info->duration_ms,
                         info->is_playing ? "playing" : "paused");
            }
            ui_set_track_info(info);
            return true;
        }
        /* Sonos unreachable or stopped (NOT_IMPLEMENTED in Connect passthrough
         * counts as stopped -- we fall back to Spotify for track display). */
        s_sonos_has_audio = false;
        /* Back off the next attempt so an unreachable speaker doesn't stall the
         * poll every cycle. A non-explicit Sonos also un-pins (falls back to the
         * Spotify view); an explicitly-picked one stays pinned but is retried
         * only after the hold expires. */
        s_sonos_fetch_hold = xTaskGetTickCount() + pdMS_TO_TICKS(10000);
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
        /* The file decode path needs LittleFS mounted; the RAM path doesn't. */
#if ART_DECODE_RAM
        bool art_ready = true;
#else
        bool art_ready = littlefs_is_mounted();
#endif
        if (info->album_art_url[0] &&
            strcmp(info->album_art_url, s_art_url_loaded) != 0 &&
            strcmp(info->album_art_url, s_art_url_failed) != 0 &&
            art_ready) {
            int r = fetch_and_publish_art(info->album_art_url);
            if (r == 1) {
                strlcpy(s_art_url_loaded, info->album_art_url, sizeof s_art_url_loaded);
            } else if (r == 0) {
                /* Decode is deterministic -- a malformed cover fails the same way
                 * every time. Record so the next poll doesn't re-fetch + re-decode
                 * the same broken art every 5 s. */
                strlcpy(s_art_url_failed, info->album_art_url, sizeof s_art_url_failed);
                ESP_LOGW(TAG, "art decode failed, not retrying this url");
            }
            /* r == -1: download failed, unrecorded (transient -- retry next poll). */
        }
        return true;
    }

    s_sonos_has_audio = false;
    if (np_state_changed(info))   /* title is empty here: logs once per transition into idle */
        ESP_LOGI(TAG, "no active playback (or fetch failed)");
    ui_set_track_info(NULL);
    return false;
}

/* Drains s_cmd_queue and runs the blocking Spotify HTTPS calls off the
 * render/input path. Polls /me/player every 5 s. After a track-changing command
 * (next/prev/play) Spotify's /me/player keeps reporting the OLD track for a
 * moment, so a single immediate poll would show stale title/artist until the
 * next 5 s tick (~6 s of lag the user sees). Instead we "settle": re-poll a few
 * times (~300 ms apart, capped ~2 s) until the title, device or progress shows
 * the command took effect, so the on-screen details catch up within ~1 s.
 * Non-track commands just refresh once. */
/* Effective Spotify credentials: a Settings > SETUP override stored in NVS
 * wins over the compiled secrets.h value (creds.h). Static because
 * spotify_init keeps the pointers for the program's lifetime. */
static char s_cred_sp_id[80];
static char s_cred_sp_secret[80];
static char s_cred_sp_refresh[CREDS_VAL_MAX];

static void spotify_task(void *arg)
{
    (void)arg;
    creds_get(CREDS_KEY_SP_ID,      s_cred_sp_id,      sizeof s_cred_sp_id,      SPOTIFY_CLIENT_ID);
    creds_get(CREDS_KEY_SP_SECRET,  s_cred_sp_secret,  sizeof s_cred_sp_secret,  SPOTIFY_CLIENT_SECRET);
    creds_get(CREDS_KEY_SP_REFRESH, s_cred_sp_refresh, sizeof s_cred_sp_refresh, SPOTIFY_REFRESH_TOKEN);
    spotify_init(s_cred_sp_id, s_cred_sp_secret, s_cred_sp_refresh);

    spotify_track_t info = {0};
    bool settle = false;
    char prev_title[sizeof info.title] = {0};
    char prev_device[sizeof info.device_name] = {0};
    uint32_t prev_progress = 0;

    while (1) {
        if (settle) {
            settle = false;
            for (int i = 0; i < 5; i++) {
                poll_and_publish(&info);
                /* The new state has landed when the track changed -- or, for
                 * commands that keep the same title, when the device switched
                 * (transfer / select-Sonos) or progress jumped backwards
                 * (prev-as-restart). Without those, same-title commands always
                 * burned the full five polls. */
                if (strncmp(info.title, prev_title, sizeof prev_title) != 0 ||
                    strcmp(info.device_name, prev_device) != 0 ||
                    info.progress_ms < prev_progress) break;
                vTaskDelay(pdMS_TO_TICKS(300));
            }
        } else {
            poll_and_publish(&info);
        }

        /* Diagnostics: while the Settings FPS DISPLAY toggle is on, emit a
         * compact network/poll/Sonos health line each poll so it sits in the
         * serial log next to the UI stats block for copy-paste analysis. */
        if (ui_diagnostics_enabled()) {
            TickType_t now = xTaskGetTickCount();
            int32_t hold_ticks = (int32_t)(s_sonos_fetch_hold - now);
            int sonos_hold_s = (s_sonos_fetch_hold && hold_ticks > 0)
                             ? (int)(pdTICKS_TO_MS(hold_ticks) / 1000) : 0;
            ESP_LOGI(TAG,
                "NETSTAT: poll_interval=%ds token_expiry=%ds poll_holdoff=%ds last_cmd=%d sonos=%s sonos_audio=%d sonos_hold=%ds",
                info.is_playing ? 5 : 15,
                spotify_token_expiry_seconds(),
                spotify_poll_holdoff_seconds(),
                spotify_last_cmd_status(),
                s_sonos_active[0] ? s_sonos_active : "-",
                s_sonos_has_audio ? 1 : 0,
                sonos_hold_s);
        }

        /* Adaptive poll: 5 s while playing, back off to 15 s when paused or
         * idle (each poll is a TLS round-trip). A queued command still wakes
         * the task early, so control stays responsive. */
        TickType_t deadline = xTaskGetTickCount() +
                              pdMS_TO_TICKS(info.is_playing ? 5000 : 15000);
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
                            if (ok && target != s_sonos_active) snprintf(s_sonos_active, sizeof s_sonos_active, "%s", target);
                            else {
                                sonos_log_diag(target);  /* dump state to debug */
                                ui_show_toast("Sonos play failed", 3000);
                            }
                        } else {
                            ok = spotify_play_album(cmd.str);
                            ESP_LOGI(TAG, "play_album(%s) -> %s", cmd.str, ok ? "ok" : "FAILED");
                            if (!ok) ui_show_toast("No active Spotify device", 3000);
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
                        static ui_device_t      list[MAX_DEVICES];
                        static spotify_device_t sp[8];
                        int n = 0, sc = 0;
                        if (spotify_get_devices(sp, 8, &sc)) {
                            for (int i = 0; i < sc && n < MAX_DEVICES; i++, n++) {
                                strlcpy(list[n].name,   sp[i].name, sizeof list[n].name);
                                strlcpy(list[n].detail, sp[i].type, sizeof list[n].detail);
                                strlcpy(list[n].id,     sp[i].id,   sizeof list[n].id);
                                list[n].is_active = sp[i].is_active;
                                list[n].is_sonos  = false;
                            }
                        }
#if defined(SONOS_DEVICES)
                        for (size_t i = 0;
                             i < sizeof s_sonos_devices / sizeof s_sonos_devices[0] && n < MAX_DEVICES;
                             i++, n++) {
                            snprintf(list[n].name,   sizeof list[n].name,   "%s", s_sonos_devices[i].name);
                            snprintf(list[n].detail, sizeof list[n].detail, "Sonos");
                            snprintf(list[n].id,     sizeof list[n].id,     "%s", s_sonos_devices[i].host);
                            list[n].is_active = (strcmp(s_sonos_devices[i].host, s_sonos_active) == 0);
                            list[n].is_sonos  = true;
                        }
#elif defined(SONOS_HOST)
                        if (SONOS_HOST[0] && n < MAX_DEVICES) {
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
                    case SCMD_GET_ALBUM_CANDIDATES: {
                        static spotify_album_candidate_t sp[ALBUM_CANDIDATE_MAX];
                        static char aerr[224];
                        int sc = 0;
                        bool got = spotify_get_saved_albums(sp, ALBUM_CANDIDATE_MAX, &sc,
                                                            aerr, sizeof aerr);
                        ui_set_album_candidates(sp, got ? sc : 0,
                            got ? NULL : (aerr[0] ? aerr : "Spotify request failed"));
                        ESP_LOGI(TAG, "album candidates: %d saved albums (ok=%d)", got ? sc : 0, got ? 1 : 0);
                        break;
                    }
                    case SCMD_SEARCH_ALBUMS: {
                        static spotify_album_candidate_t sp[ALBUM_CANDIDATE_MAX];
                        static char aerr[224];
                        int sc = 0;
                        bool got = spotify_search_albums(cmd.str, sp, ALBUM_CANDIDATE_MAX,
                                                         &sc, aerr, sizeof aerr);
                        ui_set_album_candidates(got ? sp : NULL, got ? sc : 0,
                            got ? (sc ? NULL : "No albums matched") : (aerr[0] ? aerr : "Spotify search failed"));
                        ESP_LOGI(TAG, "album search \"%s\": %d results (ok=%d)", cmd.str, got ? sc : 0, got ? 1 : 0);
                        break;
                    }
                    case SCMD_REFRESH_COVERS:
                        fetch_runtime_covers();
                        break;
                    case SCMD_TRANSFER:
                        ok = spotify_transfer_playback(cmd.str);
                        if (ok) {
                            s_sonos_active[0] = '\0';
                            s_sonos_explicit  = false;  /* back on a Spotify device */
                        }
                        ESP_LOGI(TAG, "transfer -> %s: %s", cmd.str, ok ? "ok" : "FAILED");
                        break;
                    case SCMD_SELECT_SONOS:
                        strlcpy(s_sonos_active, cmd.str, sizeof s_sonos_active);
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
                    case SCMD_TOGGLE_SHUFFLE:
                        ok = spotify_toggle_shuffle();
                        break;
                }
                /* Surface silent transport failures so a "button did nothing"
                 * complaint is debuggable from the serial log. Commands whose
                 * handlers already log both outcomes set self_logs=true in
                 * k_scmd_meta, so the generic fallback skips them. */
                if (!ok && !k_scmd_meta[cmd.type].self_logs) {
                    ESP_LOGW(TAG, "cmd %s FAILED (route=%s)",
                             k_scmd_meta[cmd.type].name, sh ? "sonos" : "spotify");
                    /* A 403 on the Spotify route means the active device is
                     * restricted (phone/tablet in Connect) -- surface the
                     * transfer hint instead of a silent no-op. Sonos can't 403,
                     * so gate on !sh. */
                    if (!sh && spotify_last_cmd_status() == 403)
                        ui_show_toast("Active device is restricted -- transfer first", 3000);
                }
                /* Arm settle on the commands that change the current track, so the
                 * loop top re-polls until the new title lands. */
                if (cmd.type == SCMD_NEXT_TRACK || cmd.type == SCMD_PREV_TRACK ||
                    cmd.type == SCMD_PLAY_ALBUM  || cmd.type == SCMD_TRANSFER ||
                    cmd.type == SCMD_SELECT_SONOS) {
                    strlcpy(prev_title, info.title, sizeof prev_title);
                    strlcpy(prev_device, info.device_name, sizeof prev_device);
                    prev_progress = info.progress_ms;
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
    art_buffer_alloc(&s_art, ART_RGB_BYTES);
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

    /* Bring up the ES8311 speaker + UI-sound task (independent of WiFi/display). */
    audio_init();

    /* Start the RP2040 haptic knob driver. Gated OFF by default so builds
     * for a board WITHOUT the knob daughterboard are completely unaffected
     * (the UART is never configured, no GPIO is touched). Set KNOB_ENABLED
     * to 1 once the daughterboard is wired and KNOB_UART_TX/RX_PIN in knob.h
     * are confirmed against the Waveshare header (must avoid the LCD/touch/
     * audio/SDIO-to-C6 pins -- see docs/DESIGN_NOTES.md open decision #2). */
#ifndef KNOB_ENABLED
#define KNOB_ENABLED 0
#endif
#if KNOB_ENABLED
    knob_input_start();
#endif

    bsp_display_lock(-1);
    lv_obj_t *status_label = lv_label_create(lv_screen_active());
    lv_label_set_text(status_label, "Music Controller P4\ncheckpoint 5: WiFi connecting...");
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(status_label);
    bsp_display_unlock();

    /* Let the ESP32-C6 WiFi slave boot its esp_hosted firmware before esp_wifi_init. */
    vTaskDelay(pdMS_TO_TICKS(3000));

    /* Effective WiFi credentials: Settings > SETUP override, else secrets.h.
     * Static -- the WiFi stack may reference them beyond this call. */
    static char s_cred_ssid[33], s_cred_pass[65];
    creds_get(CREDS_KEY_WIFI_SSID, s_cred_ssid, sizeof s_cred_ssid, WIFI_SSID);
    creds_get(CREDS_KEY_WIFI_PASS, s_cred_pass, sizeof s_cred_pass, WIFI_PASSWORD);
    if (app_core_wifi_connect(s_cred_ssid, s_cred_pass, WIFI_MAX_RETRY,
                              on_wifi_first_connect) != ESP_OK) {
        /* Initial connect exhausted its fast retries. Don't abort: app_core_wifi
         * has armed the slow background reconnect timer, which will keep trying
         * every 20 s and recover transparently once the AP is reachable. Bring
         * up the UI anyway so the browser is usable (and the "No active Spotify
         * device" toast surfaces if the user taps a card before the link is up). */
        ESP_LOGE(TAG, "wifi did not connect -- continuing; background reconnect will keep trying");
    }

    /* Build the LVGL UI (browser + now-playing) and load the browser. This
     * replaces the startup status_label screen. ui_init locks internally, so
     * it must run with the display lock released. */
    ui_init(&s_art_dsc);

    /* Create the "mailbox" (room for 8 commands) the UI posts into, then launch
     * the background worker that drains it. After the next line spotify_task
     * runs on its own forever, and app_main has done its job. */
    s_cmd_queue = xQueueCreate(8, sizeof(scmd_t));
    if (!s_cmd_queue) {
        ESP_LOGE(TAG, "failed to create command queue");
        return;   /* no mailbox -> nothing could drive playback; give up */
    }
    /* xTaskCreate(function, name, stack-size-bytes, arg, priority, out-handle).
     * 10 KB stack: the TLS handshake (cert-bundle validation) over the esp_hosted
     * WiFi transport is stack-hungry. (The CYD board used 8 KB on native WiFi.) */
    xTaskCreate(spotify_task, "spotify", 10240, NULL, 5, NULL);

    /* Boot: fetch covers for any runtime-added albums (spotify_task drains this
     * once WiFi is up). The on_wifi_first_connect post covers a late connect. */
    ui_request_refresh_covers();

    ESP_LOGI(TAG, "checkpoint 5: UI + art, Spotify task started");
}
