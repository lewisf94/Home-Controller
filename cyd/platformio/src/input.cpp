#include <Arduino.h>
#include "input.h"
#include "mcp_input.h"
#include "ui.h"

// Volume is tracked locally and pushed to Spotify (debounced) via the command
// queue, mirroring the ESP-IDF input dispatcher. input_update() never blocks:
// every Spotify action is an enqueue, every UI action takes the LVGL lock
// briefly. Called once per render-loop iteration.
static int  s_pre_mute_vol = 50;
static bool s_is_muted     = false;
static int  s_current_vol  = 50;

bool input_is_muted() { return s_is_muted; }

void input_init() {}

void input_update()
{
    bool now_playing = ui_is_now_playing();

    // RE1 push-switch: mute toggle (now-playing) or play centred album (browser)
    if (re1_sw_get_event()) {
        if (now_playing) {
            if (!s_is_muted) {
                s_pre_mute_vol = s_current_vol;
                ui_request_volume(0);
                s_is_muted = true;
            } else {
                ui_request_volume(s_pre_mute_vol);
                s_is_muted = false;
            }
            ui_show_volume_hud(s_current_vol, s_is_muted);
        } else {
            ui_play_centered_album();
        }
    }

    // First three buttons: album selection in the browser, transport controls
    // in now-playing. SW4 always toggles the view.
    if (btn_get_event(0)) {
        if (now_playing) {
            if (ui_get_progress_ms() > 5000) ui_request_seek(0);
            else                             ui_request_prev();
        } else {
            ui_scroll_browser(-1);   // scroll one album left
        }
    }
    if (btn_get_event(1)) {
        if (now_playing) ui_request_toggle_play();
        else             ui_play_centered_album();   // select centred album
    }
    if (btn_get_event(2)) {
        if (now_playing) ui_request_next();
        else             ui_scroll_browser(1);        // scroll one album right
    }
    // SW4: short press = toggle view; long hold (>500 ms, now-playing) = shuffle
    {
        static bool          s_sw4_was_held  = false;
        static bool          s_sw4_long_done = false;
        static unsigned long s_sw4_press_ms  = 0;

        bool sw4_event = btn_get_event(3);
        bool sw4_held  = btn_is_held(3);

        if (sw4_event) {
            s_sw4_press_ms  = millis();
            s_sw4_long_done = false;
        }

        if (sw4_held && !s_sw4_long_done && now_playing &&
            millis() - s_sw4_press_ms > 500) {
            ui_request_shuffle();
            s_sw4_long_done = true;
        }

        if (s_sw4_was_held && !sw4_held && !s_sw4_long_done)
            ui_toggle_view();

        s_sw4_was_held = sw4_held;
    }

    // RE1 turn: scroll carousel (browser) or adjust volume (now-playing).
    static unsigned long s_last_vol_ms = 0;
    static bool          s_vol_pending = false;

    int32_t delta = re1_get_delta();
    if (delta != 0) {
        if (now_playing) {
            // Clockwise (positive delta) raises volume.
            int new_vol = constrain(s_current_vol - (int)(delta * 5), 0, 100);
            s_current_vol = new_vol;
            s_vol_pending = true;
            s_last_vol_ms = millis();
            ui_show_volume_hud(new_vol, s_is_muted);
        } else {
            ui_scroll_browser(delta);
        }
    }

    // Debounced volume PUT: fire 300 ms after the last encoder tick.
    if (s_vol_pending && millis() - s_last_vol_ms > 300) {
        ui_request_volume(s_current_vol);
        s_vol_pending = false;
    }
}
