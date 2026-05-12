#include <Arduino.h>
#include "input.h"
#include "mcp_input.h"
#include "spotify.h"
#include "ui.h"

static int  pre_mute_vol = 50;
static bool is_muted     = false;

bool input_is_muted() { return is_muted; }

void input_init() {}

void input_update()
{
    bool now_playing = ui_is_now_playing();

    // ── RE1 push-switch: mute (now-playing) or play selected album (browser) ─
    if (re1_sw_get_event()) {
        if (now_playing) {
            if (!is_muted) {
                pre_mute_vol = current_volume_pct;
                spotify_set_volume(0);
                is_muted = true;
            } else {
                spotify_set_volume(pre_mute_vol);
                is_muted = false;
            }
            ui_show_volume_hud(current_volume_pct, is_muted);
        } else {
            ui_play_centered_album();
        }
    }

    // ── Push buttons ──────────────────────────────────────────────────────
    if (btn_get_event(0)) spotify_prev_track();
    if (btn_get_event(1)) spotify_toggle_play_pause();
    if (btn_get_event(2)) spotify_next_track();
    if (btn_get_event(3)) ui_toggle_view();

    // ── RE1 volume (now-playing only; browser scroll handled by ui_update) ─
    static unsigned long last_vol_change_ms = 0;
    static bool          vol_pending        = false;

    if (now_playing) {
        int32_t vol_delta = re1_get_delta();
        if (vol_delta != 0) {
            int new_vol = constrain(current_volume_pct + (int)(vol_delta * 5), 0, 100);
            current_volume_pct = new_vol;
            current_track_info.volume_pct = new_vol;
            vol_pending        = true;
            last_vol_change_ms = millis();
            ui_show_volume_hud(new_vol, is_muted);
        }
    }
    if (vol_pending && millis() - last_vol_change_ms > 300) {
        bool ok = spotify_set_volume(current_volume_pct);
        Serial.printf("[VOL ] set %d%% → %s\n", current_volume_pct, ok ? "ok" : "FAILED");
        vol_pending = false;
    }
}
