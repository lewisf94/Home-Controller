/*
 * LVGL UI for Music Controller (Phase 2 IDF port).
 *
 * Provides two screens:
 *   - Album browser  (default): horizontal snap-scroll carousel of
 *     hardcoded albums. Tap centred card to play it.
 *   - Now playing: 160x160 album art + title + artist + progress bar.
 *
 * Swipe up on the browser switches to now-playing; swipe down on
 * now-playing goes back. Touch is the only input on IDF (no MCP
 * encoder/buttons yet).
 *
 * All entry points lock the LVGL port internally, so callers from
 * non-LVGL tasks (e.g. spotify_task) don't need to.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "spotify_track.h"

/* Builds both screens, registers gesture/click handlers, and loads
 * the browser. Must be called once after lvgl_port_init / disp setup.
 *
 * The caller passes a long-lived lv_image_dsc_t pointer that lives
 * for the program's lifetime. The UI mutates dsc fields (cf, w, h,
 * data, data_size) under the LVGL lock from ui_art_refresh(). */
void ui_init(lv_image_dsc_t *art_dsc);

/* Updates the now-playing title, artist and progress bar from `info`.
 * Safe to call from any task (takes the LVGL lock). Pass NULL to
 * blank the labels (e.g. when no active playback). */
void ui_set_track_info(const spotify_track_t *info);

/* Rebinds the now-playing art widget to a fresh RGB565 buffer.
 * Mutates the dsc passed at ui_init and refreshes the image, all
 * under the LVGL lock. Safe to call from any task. */
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

/* UI state queries and actions -- must be called under the LVGL lock. */
bool     ui_is_now_playing(void);
void     ui_toggle_view(void);
void     ui_play_centered_album(void);
void     ui_scroll_browser(int32_t delta);
uint32_t ui_get_progress_ms(void);
void     ui_show_volume_hud(int pct, bool muted);

/* Last device volume reported by the Spotify poll (0..100, or -1 if unknown).
 * Published by ui_set_track_info under the LVGL lock; input_update reads it
 * (also under the lock) to seed the encoder/HUD base from the real level. */
int      ui_get_device_volume(void);
