// Copyright 2026 Lewis. Apache License, Version 2.0.
//
// Context-aware input mapper: translates KnobState events from the RP2040
// co-MCU into ui_request_*() calls. Calls only the backend-neutral seam in
// ui.h, so this file works identically in waveshare/esp-idf/ (Spotify) and
// the future waveshare/esp-idf-ha/ (Home Assistant).
//
// Four menus, one per MX button:
//   SW1 (bit 0) -- Now Playing : scrub timeline (soft detents, hard endstops)
//   SW2 (bit 1) -- Volume      : integer 0-100, hard stops, 1 vol% per detent
//   SW3 (bit 2) -- Albums      : album carousel, medium clicks
//   SW4 (bit 3) -- Lights      : opens the Lights screen. The confirmed knob
//                                interaction (select, toggle, adjust) is in
//                                lights_knob.c; docs/LIGHTS-INTERACTION.md is
//                                the specification.
//
// Knob-press duration: the RP2040 reports the press level in button_mask
// bit 4 and sets bit 5 to say that bit 4 is valid (see
// proto/home_controller.proto). Older RP2040 firmware sets neither bit; then
// the Lights menu treats each press_nonce change as a short press, and a long
// press is not available.
//
// None of the knob behaviour is verified on hardware yet. The knob stays off
// unless the build sets KNOB_ENABLED=1.

#include "knob_input.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"

#include "knob.h"
#include "ui.h"
#include "albums.h"
#include "home_controller.pb.h"
#include "lights_knob.h"
#include "lights_knob_ui.h"

static const char *TAG = "knob_input";

size_t album_catalog_count(void);

// Scrub step: each detent = 500 ms of track position
#define SCRUB_STEP_MS 500

// Four logical menus
typedef enum {
    MENU_NOW_PLAYING = 0,  // SW1
    MENU_VOLUME      = 1,  // SW2
    MENU_ALBUMS      = 2,  // SW3
    MENU_LIGHTS      = 3,  // SW4 (Home Assistant build)
} menu_t;

// button_mask bits above the four MX buttons (RP2040 interface_task.cpp).
#define KNOB_MASK_PRESS_DOWN  (1u << 4)   // knob press is down now
#define KNOB_MASK_PRESS_LEVEL (1u << 5)   // bit 4 is valid (newer RP2040 firmware)

// Poll period: view changes, Lights list refresh, long-press timing when the
// knob sends no state, rate-limited light commands.
#define POLL_MS 50
// Leave the Lights menu when the Lights screen does not show within this time.
#define LIGHTS_OPEN_GRACE_MS 2000

static menu_t    s_active_menu   = MENU_NOW_PLAYING;
static int32_t   s_last_position = 0;
static uint32_t  s_last_press_nonce = 0;
static uint32_t  s_last_btn_mask    = 0;
static int       s_last_brightness  = -1;   // last lux-derived panel brightness
static TimerHandle_t s_poll_timer = NULL;

// _on_state (UART RX task) and _poll_timer_cb (timer task) both drive the menu
// state, so they run under this lock. The lock is never held by LVGL code, so
// taking the LVGL lock inside it cannot deadlock.
static SemaphoreHandle_t s_lock = NULL;

// Lights menu state.
static lk_t       s_lk;
static lk_light_t s_lk_rows[LK_MAX_LIGHTS];
static uint32_t   s_lk_generation   = 0;
static bool       s_lk_have_list    = false;
static bool       s_lk_seen_visible = false;
static uint32_t   s_lk_enter_ms     = 0;
static bool       s_press_down      = false;   // last reported press level

static uint32_t _now_ms(void)
{
    return (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount());
}

static void _lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static void _unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

// ---- KnobConfig builders ----

static void _send_albums_config(int32_t position)
{
    KnobConfig cfg = KnobConfig_init_zero;
    cfg.position               = position;
    cfg.position_nonce         = (uint32_t)xTaskGetTickCount();
    cfg.min_position           = 0;
    cfg.max_position           = (int32_t)album_catalog_count() - 1;
    cfg.position_width_radians = 10.0f * (float)M_PI / 180.0f;
    cfg.detent_strength_unit   = 1.0f;
    cfg.endstop_strength_unit  = 1.0f;
    cfg.snap_point             = 1.1f;
    knob_send_config(&cfg);
}

static void _send_volume_config(int32_t volume_pct)
{
    KnobConfig cfg = KnobConfig_init_zero;
    cfg.position               = volume_pct;
    cfg.position_nonce         = (uint32_t)xTaskGetTickCount();
    cfg.min_position           = 0;
    cfg.max_position           = 100;
    cfg.position_width_radians = 3.6f * (float)M_PI / 180.0f;  // 1 deg per 1%
    cfg.detent_strength_unit   = 0.7f;
    cfg.endstop_strength_unit  = 1.0f;
    cfg.snap_point             = 1.1f;
    knob_send_config(&cfg);
}

static void _send_now_playing_config(int32_t scrub_pos)
{
    // Soft scrub feel; endstops at track start/end. Use the TRACK DURATION for
    // the upper endstop -- ui_get_progress_ms() is the current playhead, which
    // would cap the scrub at "now" and make forward scrubbing impossible.
    uint32_t duration_ms = ui_get_duration_ms();
    int32_t max_pos = (duration_ms > 0)
                    ? (int32_t)(duration_ms / SCRUB_STEP_MS)
                    : 240;  // sensible default before first poll

    KnobConfig cfg = KnobConfig_init_zero;
    cfg.position               = scrub_pos;
    cfg.position_nonce         = (uint32_t)xTaskGetTickCount();
    cfg.min_position           = 0;
    cfg.max_position           = max_pos;
    cfg.position_width_radians = 5.0f * (float)M_PI / 180.0f;
    cfg.detent_strength_unit   = 0.2f;
    cfg.endstop_strength_unit  = 0.8f;
    cfg.snap_point             = 0.55f;
    knob_send_config(&cfg);
}

// ---- Lights menu ----
// Initial haptic profiles (docs/LIGHTS-INTERACTION.md). They need physical
// tuning: none of these values has been felt on the real knob yet.
static void _send_lights_config(void)
{
    lk_haptic_t h;
    lk_haptic_profile(&s_lk, &h);

    KnobConfig cfg = KnobConfig_init_zero;
    cfg.position       = h.position;
    cfg.position_nonce = (uint32_t)xTaskGetTickCount();
    cfg.min_position   = h.min_position;
    cfg.max_position   = h.max_position;
    cfg.snap_point     = 1.1f;
    switch (h.kind) {
    case LK_HAPTIC_SELECT:       // distinct medium detents, one per light
        cfg.position_width_radians = 15.0f * (float)M_PI / 180.0f;
        cfg.detent_strength_unit   = 0.6f;
        cfg.endstop_strength_unit  = 1.0f;
        break;
    case LK_HAPTIC_BRIGHTNESS:   // small, precise detents; end stops at 0 and 100
        cfg.position_width_radians = 3.0f * (float)M_PI / 180.0f;
        cfg.detent_strength_unit   = 0.3f;
        cfg.endstop_strength_unit  = 1.0f;
        break;
    case LK_HAPTIC_HUE:          // continuous: no detents, no end stops
        cfg.position_width_radians = 3.0f * (float)M_PI / 180.0f;
        cfg.detent_strength_unit   = 0.0f;
        cfg.endstop_strength_unit  = 0.0f;
        break;
    }
    knob_send_config(&cfg);
    s_last_position = h.position;
}

static void _lights_show(void)
{
    lk_view_t v;
    memset(&v, 0, sizeof v);
    v.active   = s_lk.active;
    v.online   = s_lk.online;
    v.mode     = s_lk.mode;
    v.param    = s_lk.param;
    v.selected = s_lk.selected;
    snprintf(v.selected_id, sizeof v.selected_id, "%s", s_lk.selected_id);
    const lk_light_t *l = lk_selected(&s_lk);
    if (l) {
        v.has_light      = true;
        v.is_on          = l->is_on;
        v.brightness_pct = l->brightness_pct;
        v.hue_deg        = l->hue_deg;
    }
    ui_knob_lights_show(&v);
}

// Run the state machine's output: queue commands (non-blocking enqueue into
// the backend task, the same path the touch controls use), send a haptic
// profile, show feedback and repaint.
static void _lights_apply(const lk_output_t *o)
{
    for (int i = 0; i < o->cmd_count; i++) {
        const lk_cmd_t *c = &o->cmds[i];
        switch (c->type) {
        case LK_CMD_TOGGLE:
            ui_request_light_toggle(c->entity_id);
            break;
        case LK_CMD_BRIGHTNESS:
            ui_request_light_brightness(c->entity_id, c->value);
            break;
        case LK_CMD_HUE:
            ui_request_light_hue(c->entity_id, c->value, c->sat_pct);
            break;
        }
    }
    if (o->haptic_changed && s_active_menu == MENU_LIGHTS) _send_lights_config();

    switch (o->feedback) {
    case LK_FEEDBACK_OFFLINE:        ui_show_toast("Home Assistant is not connected", 2000); break;
    case LK_FEEDBACK_UNAVAILABLE:    ui_show_toast("Light unavailable in Home Assistant", 2000); break;
    case LK_FEEDBACK_NOT_ADJUSTABLE: ui_show_toast("This light has no brightness or colour control", 2000); break;
    case LK_FEEDBACK_SINGLE_PARAM:   ui_show_toast("This light has one adjustable setting", 2000); break;
    case LK_FEEDBACK_NO_LIGHTS:      ui_show_toast("No lights", 1500); break;
    case LK_FEEDBACK_NONE:           break;
    }

    if (o->view_changed || o->cmd_count > 0) _lights_show();
}

// Refresh the state machine's copy of the light list when Home Assistant sent
// a new one. `force` reads it even when the generation did not change.
static void _lights_refresh_list(bool force, lk_output_t *out)
{
    uint32_t gen = 0;
    int n = ui_knob_lights_snapshot(s_lk_rows, LK_MAX_LIGHTS, &gen);
    if (n < 0) return;   // LVGL lock busy: try again on the next poll
    if (!force && s_lk_have_list && gen == s_lk_generation) return;
    s_lk_generation = gen;
    s_lk_have_list  = true;
    lk_set_lights(&s_lk, s_lk_rows, n, out);
}

static void _lights_sync_online(uint32_t now, lk_output_t *out)
{
    bool ready = ui_backend_lights_ready();
    if (ready == s_lk.online) return;
    lk_set_online(&s_lk, ready, now, out);
    // Reconnected while the Lights menu is open: ask for a fresh list. The
    // selection follows the entity_id when the list arrives.
    if (ready && s_lk.active) ui_request_get_lights();
}

static bool _lights_enter(void)
{
    if (!ui_knob_lights_open()) {
        ui_show_toast("Lights need the Home Assistant build", 2000);
        return false;
    }
    uint32_t now = _now_ms();
    lk_output_t out = {0};
    s_active_menu     = MENU_LIGHTS;
    s_lk_seen_visible = false;
    s_lk_enter_ms     = now;
    _lights_sync_online(now, &out);
    _lights_refresh_list(true, &out);
    // A press that is down now (for example a press that started before the
    // menu opened) is ignored until it is released.
    lk_enter(&s_lk, s_press_down, now, &out);
    _lights_apply(&out);
    ESP_LOGI(TAG, "active menu -> %d (lights)", (int)MENU_LIGHTS);
    return true;
}

static void _lights_leave(void)
{
    if (!s_lk.active) return;
    lk_output_t out = {0};
    lk_leave(&s_lk, _now_ms(), &out);
    _lights_apply(&out);
}

static void _activate_menu(menu_t menu)
{
    if (menu == MENU_LIGHTS) {
        // Opening Lights resets the knob to light selection. When this build
        // has no Lights screen the current menu stays active.
        _lights_enter();
        return;
    }
    _lights_leave();
    s_active_menu = menu;

    // Anchor s_last_position to the SAME position we send in the config, so the
    // motor's first re-anchored report produces a zero delta instead of a
    // spurious jump (the knob reports back the position it was anchored to).
    // All three anchors below read the LIVE value rather than assuming 0/50:
    // anchoring to a stale/default value desyncs the knob's endstop feel from
    // where the content actually is (e.g. hitting a phantom "start of list"
    // wall a few detents after activating Albums mid-browse).
    int32_t anchor = 0;

    switch (menu) {
    case MENU_NOW_PLAYING: {
        // Anchor to the live playback position so the endstop feel matches
        // where the track actually is, not track-start.
        anchor = (int32_t)(ui_get_progress_ms() / SCRUB_STEP_MS);
        _send_now_playing_config(anchor);
        break;
    }
    case MENU_VOLUME: {
        // Anchor to the live device volume so the first detent doesn't snap
        // playback to 50%. -1 (unknown, pre-first-poll) falls back to 50.
        int vol = ui_get_volume();
        anchor = (vol >= 0) ? vol : 50;
        _send_volume_config(anchor);
        break;
    }
    case MENU_ALBUMS: {
        // Anchor to the actually-centred album. -1 (before the first browser
        // build) falls back to 0.
        int idx = ui_get_centered_album_index();
        anchor = (idx >= 0) ? idx : 0;
        _send_albums_config(anchor);
        break;
    }
    case MENU_LIGHTS:
        break;   // handled by _lights_enter() above
    }

    s_last_position = anchor;
    ESP_LOGI(TAG, "active menu -> %d", (int)menu);
}

// ---- KnobState callback (runs on the UART RX task) ----
static void _on_state(const KnobState *state)
{
    _lock();
    uint32_t now = _now_ms();
    lk_output_t lights_out = {0};

    uint32_t mask        = state->button_mask;
    bool     level_known = (mask & KNOB_MASK_PRESS_LEVEL) != 0;
    if (level_known) s_press_down = (mask & KNOB_MASK_PRESS_DOWN) != 0;

    int32_t pos  = state->current_position;
    int32_t delta = pos - s_last_position;

    // Position change -> route to active menu
    if (delta != 0) {
        s_last_position = pos;
        switch (s_active_menu) {
        case MENU_ALBUMS:
            ui_scroll_browser(delta);
            // Re-anchor knob so physical position tracks screen position
            _send_albums_config(pos);
            break;
        case MENU_VOLUME:
            ui_request_volume(pos);
            ui_show_volume_hud(pos, pos == 0);
            break;
        case MENU_NOW_PLAYING:
            ui_request_seek((uint32_t)(pos * SCRUB_STEP_MS));
            break;
        case MENU_LIGHTS:
            lk_rotate(&s_lk, delta, now, &lights_out);
            break;
        }
    }

    // Knob press. The Lights menu needs the press duration (short press on
    // release, long press at 500 ms), so it uses the press level when the
    // RP2040 reports it. The other menus keep their press-start action.
    bool nonce_changed = (state->press_nonce != s_last_press_nonce);
    s_last_press_nonce = state->press_nonce;
    if (level_known) {
        lk_press_level(&s_lk, s_press_down, now, &lights_out);
    }
    if (nonce_changed) {
        switch (s_active_menu) {
        case MENU_ALBUMS:
            ui_play_centered_album();
            break;
        case MENU_VOLUME:
        case MENU_NOW_PLAYING:
            ui_request_toggle_play();
            break;
        case MENU_LIGHTS:
            // Older RP2040 firmware: no press level, so one press = short press.
            if (!level_known) lk_press_event(&s_lk, now, &lights_out);
            break;
        }
    }
    lk_tick(&s_lk, now, &lights_out);
    _lights_apply(&lights_out);

    // MX button changes -> menu activation
    uint32_t buttons  = mask & 0x0Fu;
    uint32_t changed  = buttons ^ s_last_btn_mask;
    uint32_t pressed  = changed & buttons;  // newly pressed (rising edge)
    s_last_btn_mask   = buttons;

    if (pressed & (1 << 0)) _activate_menu(MENU_NOW_PLAYING);
    if (pressed & (1 << 1)) _activate_menu(MENU_VOLUME);
    if (pressed & (1 << 2)) _activate_menu(MENU_ALBUMS);
    if (pressed & (1 << 3)) _activate_menu(MENU_LIGHTS);

    // Ambient light -> display brightness.
    // State arrives every ~5 ms during a scroll, but ambient_lux only refreshes
    // every 2 s on the RP2040, so write the panel only when the mapped value
    // actually moves (>= 3%) -- otherwise this fires hundreds of redundant PWM
    // writes that also fight the idle auto-dim. (TODO at hardware bring-up:
    // make auto-dim scale this lux-derived base rather than both owning duty.)
    if (state->ambient_lux >= 0) {
        // Simple mapping: 0 lux -> 10%, 1000 lux -> 100%; clamped
        int brightness = (int)(10 + (state->ambient_lux * 90) / 1000);
        if (brightness > 100) brightness = 100;
        if (s_last_brightness < 0 || abs(brightness - s_last_brightness) >= 3) {
            s_last_brightness = brightness;
            bsp_display_brightness_set(brightness);
        }
    }

    // Battery percent -> serial log (surface to UI in a follow-on task)
    if (state->battery_percent >= 0) {
        ESP_LOGD(TAG, "battery %d%%", (int)state->battery_percent);
    }
    _unlock();
}

// ---- Periodic poll: view changes and the Lights menu ----
static void _poll_timer_cb(TimerHandle_t xTimer)
{
    static bool s_was_now_playing = false;
    _lock();
    uint32_t now = _now_ms();
    bool is_now_playing = ui_is_now_playing();
    bool view_changed = (is_now_playing != s_was_now_playing);
    s_was_now_playing = is_now_playing;

    if (s_active_menu == MENU_LIGHTS) {
        // The Lights menu keeps the knob while the Lights screen shows. When
        // touch navigation leaves the screen (or it never opened), return to
        // the natural menu for the screen that shows now.
        bool visible = ui_knob_lights_visible();
        if (visible) s_lk_seen_visible = true;
        if (!visible && (s_lk_seen_visible ||
                         (uint32_t)(now - s_lk_enter_ms) > LIGHTS_OPEN_GRACE_MS)) {
            _activate_menu(is_now_playing ? MENU_NOW_PLAYING : MENU_ALBUMS);
        } else {
            lk_output_t out = {0};
            _lights_sync_online(now, &out);
            _lights_refresh_list(false, &out);
            lk_tick(&s_lk, now, &out);
            _lights_apply(&out);
        }
    } else if (view_changed) {
        // View changed: snap to the natural menu for this screen
        _activate_menu(is_now_playing ? MENU_NOW_PLAYING : MENU_ALBUMS);
    }
    _unlock();
}

void knob_input_start(void)
{
    esp_err_t err = knob_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "knob_init failed (%s) -- knob not connected?",
                 esp_err_to_name(err));
        return;
    }

    lk_init(&s_lk);
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        ESP_LOGW(TAG, "knob input lock: out of memory");
        return;
    }

    knob_set_state_callback(_on_state);

    s_poll_timer = xTimerCreate("knob_poll", pdMS_TO_TICKS(POLL_MS),
                                pdTRUE, NULL, _poll_timer_cb);
    if (s_poll_timer) {
        xTimerStart(s_poll_timer, 0);
    }

    // Send initial config for the default menu (Albums browser)
    _lock();
    _activate_menu(MENU_ALBUMS);
    _unlock();
}
