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
//   SW4 (bit 3) -- Lights      : future HA, stubbed for now
//
// The full long-press timer / breakout-force arming / Lights HA integration
// is a follow-on task built on this foundation once the UART protocol is
// verified on hardware.

#include "knob_input.h"

#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"

#include "knob.h"
#include "ui.h"
#include "albums.h"
#include "home_controller.pb.h"

static const char *TAG = "knob_input";

// Scrub step: each detent = 500 ms of track position
#define SCRUB_STEP_MS 500

// Four logical menus
typedef enum {
    MENU_NOW_PLAYING = 0,  // SW1
    MENU_VOLUME      = 1,  // SW2
    MENU_ALBUMS      = 2,  // SW3
    MENU_LIGHTS      = 3,  // SW4 (future HA)
} menu_t;

static menu_t    s_active_menu   = MENU_NOW_PLAYING;
static int32_t   s_last_position = 0;
static uint32_t  s_last_press_nonce = 0;
static uint32_t  s_last_btn_mask    = 0;
static TimerHandle_t s_poll_timer = NULL;

// ---- KnobConfig builders ----

static void _send_albums_config(int32_t position)
{
    KnobConfig cfg = KnobConfig_init_zero;
    cfg.position               = position;
    cfg.position_nonce         = (uint32_t)xTaskGetTickCount();
    cfg.min_position           = 0;
    cfg.max_position           = (int32_t)albums_count() - 1;
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
    // Soft scrub feel; endstops at track start/end
    uint32_t duration_ms = ui_get_progress_ms();  // approximate via current progress
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

static void _activate_menu(menu_t menu)
{
    s_active_menu = menu;
    s_last_position = 0;

    switch (menu) {
    case MENU_NOW_PLAYING:
        _send_now_playing_config(0);
        break;
    case MENU_VOLUME:
        _send_volume_config(50);
        break;
    case MENU_ALBUMS:
        _send_albums_config(0);
        break;
    case MENU_LIGHTS:
        // Future HA -- no-op for now
        break;
    }
    ESP_LOGI(TAG, "active menu -> %d", (int)menu);
}

// ---- KnobState callback (runs on the UART RX task) ----
static void _on_state(const KnobState *state)
{
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
            // Future HA
            break;
        }
    }

    // Strain-gauge press (nonce increment = new press event)
    if (state->press_nonce != s_last_press_nonce) {
        s_last_press_nonce = state->press_nonce;
        switch (s_active_menu) {
        case MENU_ALBUMS:
            ui_play_centered_album();
            break;
        case MENU_VOLUME:
        case MENU_NOW_PLAYING:
            ui_request_toggle_play();
            break;
        case MENU_LIGHTS:
            break;
        }
    }

    // MX button changes -> menu activation
    uint32_t mask     = state->button_mask;
    uint32_t changed  = mask ^ s_last_btn_mask;
    uint32_t pressed  = changed & mask;  // newly pressed (rising edge)
    s_last_btn_mask   = mask;

    if (pressed & (1 << 0)) _activate_menu(MENU_NOW_PLAYING);
    if (pressed & (1 << 1)) _activate_menu(MENU_VOLUME);
    if (pressed & (1 << 2)) _activate_menu(MENU_ALBUMS);
    if (pressed & (1 << 3)) _activate_menu(MENU_LIGHTS);

    // Ambient light -> display brightness
    if (state->ambient_lux >= 0) {
        // Simple mapping: 0 lux -> 10%, 1000 lux -> 100%; clamped
        int brightness = (int)(10 + (state->ambient_lux * 90) / 1000);
        if (brightness > 100) brightness = 100;
        bsp_display_brightness_set(brightness);
    }

    // Battery percent -> serial log (surface to UI in a follow-on task)
    if (state->battery_percent >= 0) {
        ESP_LOGD(TAG, "battery %d%%", (int)state->battery_percent);
    }
}

// ---- Periodic poll: re-sync config if view changed ----
static void _poll_timer_cb(TimerHandle_t xTimer)
{
    static bool s_was_now_playing = false;
    bool is_now_playing = ui_is_now_playing();

    if (is_now_playing != s_was_now_playing) {
        s_was_now_playing = is_now_playing;
        // View changed: snap to the natural menu for this screen
        if (is_now_playing) {
            _activate_menu(MENU_NOW_PLAYING);
        } else {
            _activate_menu(MENU_ALBUMS);
        }
    }
}

void knob_input_start(void)
{
    esp_err_t err = knob_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "knob_init failed (%s) -- knob not connected?",
                 esp_err_to_name(err));
        return;
    }

    knob_set_state_callback(_on_state);

    // Poll for view changes every 100 ms
    s_poll_timer = xTimerCreate("knob_poll", pdMS_TO_TICKS(100),
                                pdTRUE, NULL, _poll_timer_cb);
    if (s_poll_timer) {
        xTimerStart(s_poll_timer, 0);
    }

    // Send initial config for the default menu (Albums browser)
    _activate_menu(MENU_ALBUMS);
}
