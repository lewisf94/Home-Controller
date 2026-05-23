#include "input.h"
#include "mcp_input.h"
#include "ui.h"
#include "esp_timer.h"

static int  s_pre_mute_vol = 50;
static bool s_is_muted     = false;
static int  s_current_vol  = 50;

static uint32_t _millis(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

bool input_is_muted(void) { return s_is_muted; }

void input_init(void) {}

void input_update(void)
{
    bool now_playing = ui_is_now_playing();

    /* RE1 push-switch: mute toggle (now-playing) or play centred album (browser) */
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

    /* SW1: browser -> scroll one album left; now-playing -> prev/restart */
    if (btn_get_event(0)) {
        if (now_playing) {
            if (ui_get_progress_ms() > 5000) ui_request_seek(0);
            else                             ui_request_prev();
        } else {
            ui_scroll_browser(-1);
        }
    }

    /* SW2: browser -> select centred album; now-playing -> play/pause */
    if (btn_get_event(1)) {
        if (now_playing) ui_request_toggle_play();
        else             ui_play_centered_album();
    }

    /* SW3: browser -> scroll one album right; now-playing -> next track */
    if (btn_get_event(2)) {
        if (now_playing) ui_request_next();
        else             ui_scroll_browser(1);
    }

    /* SW4: toggle browser <-> now-playing */
    if (btn_get_event(3)) ui_toggle_view();

    /* RE1: scroll carousel (browser) or adjust volume (now-playing) */
    static uint32_t s_last_vol_ms = 0;
    static bool     s_vol_pending = false;

    int32_t delta = re1_get_delta();
    if (delta != 0) {
        if (now_playing) {
            int new_vol = s_current_vol - (int)(delta * 5);
            if (new_vol < 0)   new_vol = 0;
            if (new_vol > 100) new_vol = 100;
            s_current_vol = new_vol;
            s_vol_pending = true;
            s_last_vol_ms = _millis();
            ui_show_volume_hud(new_vol, s_is_muted);
        } else {
            ui_scroll_browser(delta);
        }
    }

    /* Debounced volume PUT: fire 300 ms after the last encoder tick */
    if (s_vol_pending && (_millis() - s_last_vol_ms) > 300) {
        ui_request_volume(s_current_vol);
        s_vol_pending = false;
    }
}
