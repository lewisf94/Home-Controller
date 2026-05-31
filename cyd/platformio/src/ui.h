#ifndef UI_H
#define UI_H

#include <stdint.h>
#include "spotify.h"   // SpotifyTrackInfo

// Build the LVGL screens and start the progress timer. Call once after the
// LVGL display/touch glue is initialised.
void ui_init();

// Publish the latest player state (or nullptr when there is no active
// playback). Safe to call from the Spotify task; takes the LVGL lock.
void ui_set_track_info(const SpotifyTrackInfo *info);

// Publish a decoded RGB565 album-art buffer for the now-playing screen.
void ui_art_refresh(const uint8_t *rgb_data, uint16_t w, uint16_t h);

// View state / navigation.
bool     ui_is_now_playing();
void     ui_toggle_view();
void     ui_play_centered_album();
void     ui_scroll_browser(int32_t delta);
uint32_t ui_get_progress_ms();

// Transient volume / mute overlay on the now-playing screen.
void ui_show_volume_hud(int pct, bool muted);

// ── Async Spotify command requests (implemented in main.cpp) ───────────────
// These post onto the command queue drained by the Spotify task, so they
// never block the caller (LVGL / input run on a different context).
void ui_request_play(const char *uri);
void ui_request_toggle_play();
void ui_request_prev();
void ui_request_next();
void ui_request_seek(uint32_t ms);
void ui_request_volume(int pct);

// ── Legacy no-op shims ─────────────────────────────────────────────────────
// spotify.cpp still calls these around its network I/O (they guarded the old
// TFT_eSPI Sprite, which no longer exists under LVGL). Kept as no-ops so
// spotify.cpp compiles unchanged.
void ui_suspend_sprite();
void ui_resume_sprite();

#endif // UI_H
