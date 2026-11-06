// Copyright 2026 Lewis. Apache License, Version 2.0.
//
// Lights knob state machine (pure C, no ESP-IDF or LVGL dependency).
//
// This module holds the confirmed knob interaction for the Lights screen.
// docs/LIGHTS-INTERACTION.md is the specification. Summary:
//
//   Light selection mode
//     Rotate      : move the selection between lights (one detent per light)
//     Short press : toggle the selected light
//     Long press  : enter adjustment mode for the selected light
//   Light adjustment mode (starts on brightness)
//     Rotate      : adjust the active parameter (brightness or hue)
//     Short press : switch between brightness and hue
//     Long press  : return to light selection (selected light is kept)
//
// A short press fires on release. A long press fires once, 500 ms after the
// press starts, and the release that follows it does not fire a short press.
//
// The caller (knob_input.c) owns all I/O. Each call appends commands, a
// haptic-profile change flag and a feedback code to an lk_output_t. The caller
// then sends the commands to the existing ui_request_light_* functions, sends
// the haptic profile to the knob, and repaints the screen. This keeps network
// work and LVGL work out of this module and lets the host tests drive it with
// a fake clock.

#ifndef LIGHTS_KNOB_H
#define LIGHTS_KNOB_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LK_MAX_LIGHTS          16
#define LK_ENTITY_LEN          96
#define LK_LONG_PRESS_MS       500   /* confirmed design value */
#define LK_SEND_INTERVAL_MS    220   /* same as the touchscreen LIGHT_LIVE_SEND_MS */
#define LK_PRESS_STALE_MS      250   /* no press report for this long = link lost */
#define LK_BRIGHTNESS_STEP_PCT 1     /* brightness change per detent */
#define LK_HUE_STEP_DEG        5     /* hue change per position (no detents) */
#define LK_MAX_CMDS            4

typedef struct {
    char entity_id[LK_ENTITY_LEN];
    bool available;      /* false: Home Assistant reports the light unavailable */
    bool is_on;
    bool dimmable;       /* brightness control is supported */
    int  brightness_pct; /* 0..100, used only when dimmable */
    bool hue_supported;  /* hue/saturation control is supported */
    int  hue_deg;        /* 0..359, used only when hue_supported */
    int  sat_pct;        /* 1..100, sent with each hue command */
} lk_light_t;

typedef enum {
    LK_MODE_SELECT = 0,
    LK_MODE_ADJUST = 1,
} lk_mode_t;

typedef enum {
    LK_PARAM_BRIGHTNESS = 0,
    LK_PARAM_HUE        = 1,
} lk_param_t;

typedef enum {
    LK_CMD_TOGGLE = 1,
    LK_CMD_BRIGHTNESS,
    LK_CMD_HUE,
} lk_cmd_type_t;

typedef struct {
    lk_cmd_type_t type;
    char entity_id[LK_ENTITY_LEN];
    int  value;          /* brightness percent or hue degrees */
    int  sat_pct;        /* LK_CMD_HUE only */
} lk_cmd_t;

typedef enum {
    LK_FEEDBACK_NONE = 0,
    LK_FEEDBACK_OFFLINE,         /* command refused: Home Assistant not ready */
    LK_FEEDBACK_UNAVAILABLE,     /* selected light is unavailable */
    LK_FEEDBACK_NOT_ADJUSTABLE,  /* light has no brightness and no hue control */
    LK_FEEDBACK_SINGLE_PARAM,    /* short press in adjust mode, only one parameter */
    LK_FEEDBACK_NO_LIGHTS,       /* the list is empty */
} lk_feedback_t;

typedef struct {
    lk_cmd_t      cmds[LK_MAX_CMDS];
    int           cmd_count;
    bool          haptic_changed;  /* send a new KnobConfig (lk_haptic_profile) */
    bool          view_changed;    /* repaint the selection / mode indicator */
    lk_feedback_t feedback;
} lk_output_t;

typedef enum {
    LK_HAPTIC_SELECT = 0,   /* medium detents, one per light, end stops */
    LK_HAPTIC_BRIGHTNESS,   /* small detents, end stops at 0 and 100 */
    LK_HAPTIC_HUE,          /* continuous, no detents, no end stops */
} lk_haptic_kind_t;

typedef struct {
    lk_haptic_kind_t kind;
    int32_t position;       /* knob position the config anchors to */
    int32_t min_position;   /* -1 together with max -1 = unbounded */
    int32_t max_position;
} lk_haptic_t;

typedef struct {
    lk_light_t lights[LK_MAX_LIGHTS];
    int        count;
    int        selected;                 /* -1 when the list is empty */
    char       selected_id[LK_ENTITY_LEN];
    bool       active;                   /* Lights menu owns the knob */
    lk_mode_t  mode;
    lk_param_t param;
    bool       online;                   /* backend accepts light commands */

    /* Press tracking (press level reported by the knob). */
    bool       press_down;               /* last reported level */
    bool       press_tracking;           /* a press started while armed */
    bool       press_armed;              /* false: ignore until the next release */
    bool       long_fired;
    uint32_t   press_start_ms;
    uint32_t   last_press_report_ms;

    /* Rate-limited value command. */
    bool       pending;
    lk_param_t pending_param;
    int        pending_value;
    char       pending_entity[LK_ENTITY_LEN];
    bool       sent_any;
    uint32_t   last_send_ms;
    lk_param_t last_sent_param;
    int        last_sent_value;
    char       last_sent_entity[LK_ENTITY_LEN];
} lk_t;

void lk_init(lk_t *lk);

/* Replace the light list (for example after a Home Assistant refresh). The
 * selection follows the same entity_id. In adjust mode the local value of the
 * selected light is kept, so a stale refresh does not move it backwards. */
void lk_set_lights(lk_t *lk, const lk_light_t *lights, int count, lk_output_t *out);

/* The Lights button made the Lights menu active. press_down is the current
 * knob-press level: a press that is already down is ignored until release, so
 * opening the screen never toggles a light. */
void lk_enter(lk_t *lk, bool press_down, uint32_t now_ms, lk_output_t *out);

/* Another menu took the knob. A pending value is sent first. */
void lk_leave(lk_t *lk, uint32_t now_ms, lk_output_t *out);

/* Knob-press level from the knob (new RP2040 firmware). */
void lk_press_level(lk_t *lk, bool down, uint32_t now_ms, lk_output_t *out);

/* One press event without duration (older RP2040 firmware: the press nonce
 * changed). Acts as a short press. A long press is not possible. */
void lk_press_event(lk_t *lk, uint32_t now_ms, lk_output_t *out);

/* Knob rotation in detents (or positions for hue). */
void lk_rotate(lk_t *lk, int32_t delta, uint32_t now_ms, lk_output_t *out);

/* Periodic tick: long-press timing, stale-press cancel, rate-limited send. */
void lk_tick(lk_t *lk, uint32_t now_ms, lk_output_t *out);

/* Backend readiness. Offline drops any pending value and refuses commands;
 * the mode and the selection stay as they are. */
void lk_set_online(lk_t *lk, bool online, uint32_t now_ms, lk_output_t *out);

/* Haptic profile for the current mode. */
void lk_haptic_profile(const lk_t *lk, lk_haptic_t *out);

/* Selected light, or NULL. */
const lk_light_t *lk_selected(const lk_t *lk);

#ifdef __cplusplus
}
#endif

#endif /* LIGHTS_KNOB_H */
