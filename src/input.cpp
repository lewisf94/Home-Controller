#include <Arduino.h>
#include "input.h"
#include "mcp_input.h"
#include "spotify.h"

extern void ui_toggle_view();

// ── RE2-SW mute state ──────────────────────────────────────────────────────
static int  pre_mute_vol = 50;
static bool is_muted     = false;

// ── SW4 seek state machine ─────────────────────────────────────────────────
#define SW4_HOLD_MS         500   // hold duration before entering seek mode
#define SW4_DOUBLE_CLICK_MS 300   // max gap between clicks for double-click
#define SW4_SEEK_STEP_MS    5000  // RE1 steps → ms per click in seek mode
#define SW4_SINGLE_SKIP_MS  10000 // single click = +10 s
#define SW4_DOUBLE_SKIP_MS  10000 // double click = -10 s

static unsigned long sw4_press_start_ms    = 0;
static bool          sw4_active            = false; // button was pressed, not yet resolved
static bool          sw4_in_hold_seek      = false;
static int32_t       sw4_seek_offset_ms    = 0;
static bool          sw4_await_double      = false; // waiting to see if second click arrives
static unsigned long sw4_first_release_ms  = 0;

static void _handle_sw4()
{
    bool pressed  = btn_get_event(3);
    bool held     = btn_is_held(3);

    // ── New press ─────────────────────────────────────────────────────────
    if (pressed) {
        sw4_press_start_ms = millis();
        sw4_seek_offset_ms = 0;
        sw4_in_hold_seek   = false;
        sw4_active         = true;
    }

    // ── Transition into hold-seek mode ────────────────────────────────────
    if (sw4_active && held && !sw4_in_hold_seek) {
        if (millis() - sw4_press_start_ms > SW4_HOLD_MS) {
            sw4_in_hold_seek = true;
        }
    }

    // ── Accumulate RE1 while in seek mode ─────────────────────────────────
    // Consuming re1_get_delta() here prevents ui_update() from scrolling albums.
    if (sw4_in_hold_seek) {
        int32_t delta = re1_get_delta();
        sw4_seek_offset_ms += delta * SW4_SEEK_STEP_MS;
    }

    // ── Button released ───────────────────────────────────────────────────
    if (sw4_active && !held) {
        sw4_active = false;

        if (sw4_in_hold_seek) {
            // Execute variable seek
            int32_t new_pos = (int32_t)current_track_info.progress_ms + sw4_seek_offset_ms;
            if (new_pos < 0) new_pos = 0;
            if (new_pos > (int32_t)current_track_info.duration_ms)
                new_pos = (int32_t)current_track_info.duration_ms;
            spotify_seek_position(new_pos);
            sw4_in_hold_seek = false;
        } else {
            // Short press: check for double-click
            if (sw4_await_double &&
                millis() - sw4_first_release_ms < SW4_DOUBLE_CLICK_MS) {
                // Double-click confirmed — seek backward
                int32_t new_pos = (int32_t)current_track_info.progress_ms - SW4_DOUBLE_SKIP_MS;
                if (new_pos < 0) new_pos = 0;
                spotify_seek_position(new_pos);
                sw4_await_double = false;
            } else {
                // Start double-click window
                sw4_await_double     = true;
                sw4_first_release_ms = millis();
            }
        }
    }

    // ── Fire single-click once double-click window expires ────────────────
    if (sw4_await_double && !held &&
        millis() - sw4_first_release_ms > SW4_DOUBLE_CLICK_MS) {
        int32_t new_pos = (int32_t)current_track_info.progress_ms + SW4_SINGLE_SKIP_MS;
        if (new_pos > (int32_t)current_track_info.duration_ms)
            new_pos = (int32_t)current_track_info.duration_ms;
        spotify_seek_position(new_pos);
        sw4_await_double = false;
    }
}

// ── Public API ─────────────────────────────────────────────────────────────
void input_init()
{
    // MCP23017 is initialised directly from main.cpp via mcp_input_init()
}

void input_update()
{
    // ── Encoder push-switches ─────────────────────────────────────────────
    if (re1_sw_get_event()) ui_toggle_view();

    if (re2_sw_get_event()) {
        if (!is_muted) {
            pre_mute_vol = current_volume_pct;
            spotify_set_volume(0);
            is_muted = true;
        } else {
            spotify_set_volume(pre_mute_vol);
            is_muted = false;
        }
    }

    // ── Push buttons ──────────────────────────────────────────────────────
    if (btn_get_event(0)) spotify_prev_track();
    if (btn_get_event(1)) spotify_toggle_play_pause();
    if (btn_get_event(2)) spotify_next_track();

    // ── SW4 seek (single/double click + hold+RE1) ─────────────────────────
    _handle_sw4();

    // ── RE2 volume (±5% per step) ─────────────────────────────────────────
    int32_t vol_delta = re2_get_delta();
    if (vol_delta != 0) {
        int new_vol = constrain(current_volume_pct + (int)(vol_delta * 5), 0, 100);
        spotify_set_volume(new_vol);
    }
}
