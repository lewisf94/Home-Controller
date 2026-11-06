// Copyright 2026 Lewis. Apache License, Version 2.0.
//
// Private seam between knob_input.c and ui.c for the knob Lights menu.
// This header stays next to the sources (not in the public include folder)
// because only the p4_shared component uses it.

#ifndef LIGHTS_KNOB_UI_H
#define LIGHTS_KNOB_UI_H

#include <stdbool.h>
#include <stdint.h>

#include "lights_knob.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool       active;        /* knob Lights menu owns the knob */
    bool       online;        /* backend accepts light commands */
    lk_mode_t  mode;
    lk_param_t param;
    int        selected;      /* row index, -1 = none */
    char       selected_id[LK_ENTITY_LEN];
    bool       has_light;     /* the fields below are valid */
    bool       is_on;
    int        brightness_pct;
    int        hue_deg;
} lk_view_t;

/* Open the Lights screen (HA build). Returns false when this build has no
 * Lights screen. Takes the LVGL lock. */
bool ui_knob_lights_open(void);

/* True while the Lights screen is the active screen. Takes the LVGL lock. */
bool ui_knob_lights_visible(void);

/* Copy the current light rows. *generation changes each time Home Assistant
 * sends a new list. Returns the row count, or -1 when the LVGL lock was busy
 * (the caller keeps its old copy). */
int ui_knob_lights_snapshot(lk_light_t *out, int max, uint32_t *generation);

/* Show the knob selection and the active parameter, and copy the knob's
 * optimistic values into the matching row. Takes the LVGL lock. */
void ui_knob_lights_show(const lk_view_t *view);

/* Backend readiness for light commands. Each build's main.c defines it:
 * the Home Assistant build reports the WebSocket session state, and the
 * direct Spotify build returns false. Must not block. */
bool ui_backend_lights_ready(void);

/* Defined by each build's main.c (no-op in the direct build). */
void ui_request_light_hue(const char *entity_id, int hue_deg, int sat_pct);

#ifdef __cplusplus
}
#endif

#endif /* LIGHTS_KNOB_UI_H */
