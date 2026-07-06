/*
 * LVGL UI for Music Controller (Waveshare ESP32-P4 port, cp4).
 *
 * Provides two screens:
 *   - Album browser  (default): horizontal snap-scroll carousel of
 *     hardcoded albums. Tap centred card to play it.
 *   - Now playing: album art + title + artist + progress bar.
 *
 * Swipe up on the browser switches to now-playing; swipe down on
 * now-playing goes back. Touch (GT911) is the only input at cp4
 * (physical controls come later via the input.c seam).
 *
 * All entry points lock the LVGL adapter internally (bsp_display_lock),
 * so callers from non-LVGL tasks (e.g. spotify_task) don't need to.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "player.h"

/* Builds both screens, registers gesture/click handlers, and loads
 * the browser. Must be called once after the BSP display is up.
 *
 * The caller passes a long-lived lv_image_dsc_t pointer that lives
 * for the program's lifetime. The UI mutates dsc fields (cf, w, h,
 * data, data_size) under the LVGL lock from ui_art_refresh(). At cp4
 * the art is left blank (decode path arrives at cp5). */
void ui_init(lv_image_dsc_t *art_dsc);

/* Updates the now-playing title, artist and progress bar from `info`.
 * Safe to call from any task (takes the LVGL lock). Pass NULL to
 * blank the labels (e.g. when no active playback). */
void ui_set_track_info(const spotify_track_t *info);

/* Rebinds the now-playing art widget to a fresh RGB565 buffer.
 * Mutates the dsc passed at ui_init and refreshes the image, all
 * under the LVGL lock. Safe to call from any task. (cp5) */
void ui_art_refresh(const uint8_t *rgb_data, uint16_t w, uint16_t h);

/* Hook used by ui.c when the user taps a card on the browser. The host
 * (main.c) implements this and forwards the URI to the Spotify task via
 * a FreeRTOS queue so the HTTPS PUT doesn't block the LVGL render loop.
 * The pointer must stay valid until the task picks it up -- in practice
 * we hand over album URIs that live in .rodata, which is always valid. */
void ui_request_play(const char *context_uri);

/* Spotify command callbacks -- implemented in main.c, post to the Spotify
 * task queue. Safe to call from any context including under the LVGL lock. */
void ui_request_toggle_play(void);
void ui_request_prev(void);
void ui_request_next(void);
void ui_request_seek(uint32_t ms);
void ui_request_volume(int pct);
void ui_request_shuffle(void);

/* Device selector. ui_request_get_devices() asks the Spotify task to fetch the
 * current device list and hand it back via ui_set_devices(). Tapping a row then
 * calls ui_request_transfer() (Spotify Connect device id) or
 * ui_request_select_sonos() (Sonos LAN IP) to switch the active target. */
void ui_request_get_devices(void);
void ui_request_transfer(const char *device_id);
void ui_request_select_sonos(const char *host);

/* One row in the device selector: a Spotify Connect device (is_sonos=false,
 * id = Spotify device id) or a configured Sonos speaker (is_sonos=true,
 * id = LAN IP). `detail` is the device type / "Sonos". */
#define MAX_DEVICES 16
typedef struct {
    char name[40];
    char detail[24];
    char id[64];
    bool is_active;
    bool is_sonos;
} ui_device_t;

/* Populate the DEVICES screen list (copies the rows). Safe from any task. */
void ui_set_devices(const ui_device_t *list, int count);

/* Lights selector (HA build only). ui_request_get_lights() asks the backend
 * to fetch all light.* entities and hand them back via ui_set_lights().
 * Tapping a row's power icon calls ui_request_light_toggle(); releasing its
 * brightness slider calls ui_request_light_brightness(). The non-HA build
 * implements all three as no-ops (there is no lights backend there) -- the
 * screen still opens and shows "No lights configured", mirroring how the
 * HA build no-ops ui_request_select_sonos() the other way. */
void ui_request_get_lights(void);
void ui_request_light_toggle(const char *entity_id);
void ui_request_light_brightness(const char *entity_id, int pct);

/* One row in the lights selector. `entity_id` is the full HA entity_id (e.g.
 * "light.living_room" -- 96 B matches the eid[96] scratch buffers ha_client.c
 * uses everywhere else for one, so unlike ui_device_t.id no index-indirection
 * table is needed). brightness_pct is -1 when the light isn't dimmable or its
 * brightness is unknown (typically: an off light whose state object omits the
 * attribute) -- the row then shows just a toggle. */
#define MAX_LIGHTS 16
typedef struct {
    char name[40];
    char entity_id[96];
    bool is_on;
    int  brightness_pct;
} ui_light_t;

/* Populate the LIGHTS screen list (copies the rows). Safe from any task. */
void ui_set_lights(const ui_light_t *list, int count);

/* UI state queries and actions. Safe to call from any task -- each takes the
 * LVGL lock internally. This is the seam for physical controls (the future
 * RP2040 knob/button co-MCU feeding events from a UART task). */
bool     ui_is_now_playing(void);
void     ui_toggle_view(void);
void     ui_play_centered_album(void);
void     ui_scroll_browser(int32_t delta);
uint32_t ui_get_progress_ms(void);
uint32_t ui_get_duration_ms(void);
int      ui_get_volume(void);          /* device volume 0..100, or -1 if unknown */
void     ui_show_volume_hud(int pct, bool muted);
int      ui_get_centered_album_index(void);  /* browser's current centred album, or -1 before the first poll */

/* Brief auto-hiding notification on the now-playing screen. Safe to call from
 * any task (takes the LVGL lock). Used to surface failures the user would
 * otherwise see as silent inaction (e.g. play failed -- no active device). */
void     ui_show_toast(const char *msg, uint32_t ms_dur);

/* Browser <-> now-playing transition style. Every screen switch (swipe,
 * tap-to-play, encoder) routes through one helper that honours this, so the
 * choice is a single source of truth (a future settings screen flips it).
 * NONE is instant: it skips the animated full-screen composite that hammers
 * the DSI triple-buffer flush and could stall the render task. */
typedef enum {
    UI_TRANSITION_OVER = 0,  /* new screen slides over the old (the original) */
    UI_TRANSITION_MOVE,      /* both screens slide together (push) */
    UI_TRANSITION_FADE,      /* cross-fade */
    UI_TRANSITION_NONE,      /* instant, no animation */
    UI_TRANSITION_COUNT,
} ui_transition_t;

void            ui_set_transition_style(ui_transition_t style);
ui_transition_t ui_get_transition_style(void);

/* True while the Settings FPS DISPLAY toggle is on -- used as the "diagnostics
 * enabled" gate so other modules (e.g. the Spotify task) can emit their own
 * periodic stats lines alongside the UI stats block. */
bool            ui_diagnostics_enabled(void);
