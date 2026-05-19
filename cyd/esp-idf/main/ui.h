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
#include "spotify.h"

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
