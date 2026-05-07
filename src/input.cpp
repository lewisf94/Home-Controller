#include <Arduino.h>
#include "input.h"
#include "mcp_input.h"
#include "spotify.h"
#include "ui.h"

extern void ui_toggle_view();

// ── RE2-SW mute state ──────────────────────────────────────────────────────
static int  pre_mute_vol = 50;
static bool is_muted     = false;

// ── SW4 seek state machine ─────────────────────────────────────────────────
#define SW4_HOLD_MS         500
#define SW4_DOUBLE_CLICK_MS 300
#define SW4_SEEK_STEP_MS    5000  // RE1 steps → ±5 s per click
#define SW4_SINGLE_SKIP_MS  10000
#define SW4_DOUBLE_SKIP_MS  10000

static unsigned long sw4_press_start_ms   = 0;
static bool          sw4_active           = false;
static bool          sw4_in_hold_seek     = false;
static int32_t       sw4_seek_offset_ms_  = 0;
static bool          sw4_await_double     = false;
static unsigned long sw4_first_release_ms = 0;

static void _handle_sw4()
{
    bool pressed = btn_get_event(3);
    bool held    = btn_is_held(3);

    if (pressed) {
        sw4_press_start_ms = millis();
        sw4_seek_offset_ms_ = 0;
        sw4_in_hold_seek    = false;
        sw4_active          = true;
    }

    if (sw4_active && held && !sw4_in_hold_seek) {
        if (millis() - sw4_press_start_ms > SW4_HOLD_MS) {
            sw4_in_hold_seek = true;
        }
    }

    if (sw4_in_hold_seek) {
        // Consume RE1 so album browser doesn't scroll while seeking
        int32_t delta = re1_get_delta();
        sw4_seek_offset_ms_ += delta * SW4_SEEK_STEP_MS;
    }

    if (sw4_active && !held) {
        sw4_active = false;

        if (sw4_in_hold_seek) {
            int32_t new_pos = (int32_t)current_track_info.progress_ms + sw4_seek_offset_ms_;
            if (new_pos < 0) new_pos = 0;
            if (new_pos > (int32_t)current_track_info.duration_ms)
                new_pos = (int32_t)current_track_info.duration_ms;
            spotify_seek_position(new_pos);
            sw4_in_hold_seek    = false;
            sw4_seek_offset_ms_ = 0;
        } else {
            if (sw4_await_double &&
                millis() - sw4_first_release_ms < SW4_DOUBLE_CLICK_MS) {
                int32_t new_pos = (int32_t)current_track_info.progress_ms - SW4_DOUBLE_SKIP_MS;
                if (new_pos < 0) new_pos = 0;
                spotify_seek_position(new_pos);
                sw4_await_double = false;
            } else {
                sw4_await_double     = true;
                sw4_first_release_ms = millis();
            }
        }
    }

    if (sw4_await_double && !held &&
        millis() - sw4_first_release_ms > SW4_DOUBLE_CLICK_MS) {
        int32_t new_pos = (int32_t)current_track_info.progress_ms + SW4_SINGLE_SKIP_MS;
        if (new_pos > (int32_t)current_track_info.duration_ms)
            new_pos = (int32_t)current_track_info.duration_ms;
        spotify_seek_position(new_pos);
        sw4_await_double = false;
    }
}

// ── State queries ──────────────────────────────────────────────────────────
bool input_is_muted()               { return is_muted; }
bool input_sw4_seek_active()        { return sw4_in_hold_seek; }
int32_t input_sw4_seek_offset_ms()  { return sw4_seek_offset_ms_; }

// ── Public API ─────────────────────────────────────────────────────────────
void input_init() {}

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
        ui_show_volume_hud(current_volume_pct, is_muted);
    }

    // ── Push buttons ──────────────────────────────────────────────────────
    if (btn_get_event(0)) spotify_prev_track();
    if (btn_get_event(1)) spotify_toggle_play_pause();
    if (btn_get_event(2)) spotify_next_track();

    // ── SW4 seek ──────────────────────────────────────────────────────────
    _handle_sw4();

    // ── RE2 volume — debounced (fire HTTP only after 300 ms of inactivity) ─
    static unsigned long last_vol_change_ms = 0;
    static bool          vol_pending        = false;

    int32_t vol_delta = re2_get_delta();
    if (vol_delta != 0) {
        int new_vol = constrain(current_volume_pct + (int)(vol_delta * 5), 0, 100);
        current_volume_pct = new_vol;
        current_track_info.volume_pct = new_vol;
        vol_pending        = true;
        last_vol_change_ms = millis();
        ui_show_volume_hud(new_vol, is_muted);
    }
    if (vol_pending && millis() - last_vol_change_ms > 300) {
        spotify_set_volume(current_volume_pct);
        vol_pending = false;
    }
}
