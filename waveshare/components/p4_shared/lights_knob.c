// Copyright 2026 Lewis. Apache License, Version 2.0.
//
// Lights knob state machine. See lights_knob.h and docs/LIGHTS-INTERACTION.md.

#include "lights_knob.h"

#include <stdio.h>
#include <string.h>

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int wrap_hue(int v)
{
    v %= 360;
    if (v < 0) v += 360;
    return v;
}

static void copy_id(char *dst, const char *src)
{
    snprintf(dst, LK_ENTITY_LEN, "%s", src ? src : "");
}

static lk_light_t *selected_light(lk_t *lk)
{
    if (lk->selected < 0 || lk->selected >= lk->count) return NULL;
    return &lk->lights[lk->selected];
}

const lk_light_t *lk_selected(const lk_t *lk)
{
    if (lk->selected < 0 || lk->selected >= lk->count) return NULL;
    return &lk->lights[lk->selected];
}

static void set_feedback(lk_output_t *out, lk_feedback_t f)
{
    if (out && out->feedback == LK_FEEDBACK_NONE) out->feedback = f;
}

static void push_cmd(lk_output_t *out, lk_cmd_type_t type, const char *entity,
                     int value, int sat)
{
    if (!out || out->cmd_count >= LK_MAX_CMDS) return;
    lk_cmd_t *c = &out->cmds[out->cmd_count++];
    memset(c, 0, sizeof(*c));
    c->type = type;
    copy_id(c->entity_id, entity);
    c->value = value;
    c->sat_pct = sat;
}

static bool param_supported(const lk_light_t *l, lk_param_t p)
{
    if (!l || !l->available) return false;
    return (p == LK_PARAM_BRIGHTNESS) ? l->dimmable : l->hue_supported;
}

void lk_init(lk_t *lk)
{
    memset(lk, 0, sizeof(*lk));
    lk->selected = -1;
    lk->mode = LK_MODE_SELECT;
    lk->param = LK_PARAM_BRIGHTNESS;
    lk->press_armed = true;
}

/* ---- Rate-limited value commands ---- */

static void send_pending(lk_t *lk, uint32_t now_ms, lk_output_t *out)
{
    if (!lk->pending) return;
    lk->pending = false;
    if (!lk->online) return;

    /* Do not repeat a value that was already sent for the same light. */
    if (lk->sent_any && lk->last_sent_param == lk->pending_param &&
        lk->last_sent_value == lk->pending_value &&
        strcmp(lk->last_sent_entity, lk->pending_entity) == 0) {
        return;
    }

    if (lk->pending_param == LK_PARAM_BRIGHTNESS) {
        push_cmd(out, LK_CMD_BRIGHTNESS, lk->pending_entity, lk->pending_value, 0);
    } else {
        int sat = 100;
        for (int i = 0; i < lk->count; i++) {
            if (strcmp(lk->lights[i].entity_id, lk->pending_entity) == 0) {
                sat = lk->lights[i].sat_pct;
                break;
            }
        }
        push_cmd(out, LK_CMD_HUE, lk->pending_entity, lk->pending_value,
                 clamp_int(sat, 1, 100));
    }
    lk->sent_any = true;
    lk->last_send_ms = now_ms;
    lk->last_sent_param = lk->pending_param;
    lk->last_sent_value = lk->pending_value;
    copy_id(lk->last_sent_entity, lk->pending_entity);
}

static void queue_value(lk_t *lk, lk_param_t param, int value, uint32_t now_ms,
                        lk_output_t *out)
{
    const lk_light_t *l = selected_light(lk);
    if (!l) return;
    lk->pending = true;
    lk->pending_param = param;
    lk->pending_value = value;
    copy_id(lk->pending_entity, l->entity_id);
    if (!lk->sent_any || (uint32_t)(now_ms - lk->last_send_ms) >= LK_SEND_INTERVAL_MS) {
        send_pending(lk, now_ms, out);
    }
}

/* ---- Mode changes ---- */

static void set_mode(lk_t *lk, lk_mode_t mode, lk_param_t param, lk_output_t *out)
{
    if (lk->mode != mode || lk->param != param) {
        lk->mode = mode;
        lk->param = param;
        if (out) {
            out->haptic_changed = true;
            out->view_changed = true;
        }
    }
}

static void short_press(lk_t *lk, uint32_t now_ms, lk_output_t *out)
{
    lk_light_t *l = selected_light(lk);
    if (lk->mode == LK_MODE_SELECT) {
        if (!l) { set_feedback(out, LK_FEEDBACK_NO_LIGHTS); return; }
        if (!l->available) { set_feedback(out, LK_FEEDBACK_UNAVAILABLE); return; }
        if (!lk->online) { set_feedback(out, LK_FEEDBACK_OFFLINE); return; }
        push_cmd(out, LK_CMD_TOGGLE, l->entity_id, 0, 0);
        l->is_on = !l->is_on;   /* optimistic, the next refresh confirms it */
        if (out) out->view_changed = true;
        return;
    }

    /* Adjust mode: switch parameter when the light has both. */
    lk_param_t other = (lk->param == LK_PARAM_BRIGHTNESS) ? LK_PARAM_HUE
                                                         : LK_PARAM_BRIGHTNESS;
    if (!param_supported(l, other)) {
        set_feedback(out, LK_FEEDBACK_SINGLE_PARAM);
        return;
    }
    send_pending(lk, now_ms, out);   /* final value of the old parameter */
    set_mode(lk, LK_MODE_ADJUST, other, out);
}

static void long_press(lk_t *lk, uint32_t now_ms, lk_output_t *out)
{
    if (lk->mode == LK_MODE_ADJUST) {
        send_pending(lk, now_ms, out);
        set_mode(lk, LK_MODE_SELECT, LK_PARAM_BRIGHTNESS, out);
        return;
    }

    const lk_light_t *l = selected_light(lk);
    if (!l) { set_feedback(out, LK_FEEDBACK_NO_LIGHTS); return; }
    if (!l->available) { set_feedback(out, LK_FEEDBACK_UNAVAILABLE); return; }
    if (l->dimmable) {
        set_mode(lk, LK_MODE_ADJUST, LK_PARAM_BRIGHTNESS, out);
    } else if (l->hue_supported) {
        /* Brightness is the default, but this light has no brightness control. */
        set_mode(lk, LK_MODE_ADJUST, LK_PARAM_HUE, out);
    } else {
        set_feedback(out, LK_FEEDBACK_NOT_ADJUSTABLE);
    }
}

/* ---- Public API ---- */

void lk_set_lights(lk_t *lk, const lk_light_t *lights, int count, lk_output_t *out)
{
    if (count < 0) count = 0;
    if (count > LK_MAX_LIGHTS) count = LK_MAX_LIGHTS;

    /* Keep the in-progress value of the light being adjusted. */
    lk_light_t keep;
    bool have_keep = false;
    const lk_light_t *old_sel = selected_light(lk);
    if (old_sel && lk->mode == LK_MODE_ADJUST) {
        keep = *old_sel;
        have_keep = true;
    }

    int old_count = lk->count;
    int old_index = lk->selected;
    lk->count = count;
    for (int i = 0; i < count; i++) {
        lk->lights[i] = lights[i];
        lk->lights[i].brightness_pct = clamp_int(lights[i].brightness_pct, 0, 100);
        lk->lights[i].hue_deg = wrap_hue(lights[i].hue_deg);
        lk->lights[i].sat_pct = clamp_int(lights[i].sat_pct > 0 ? lights[i].sat_pct : 100, 1, 100);
        lk->lights[i].entity_id[LK_ENTITY_LEN - 1] = '\0';
    }

    int found = -1;
    if (lk->selected_id[0]) {
        for (int i = 0; i < count; i++) {
            if (strcmp(lk->lights[i].entity_id, lk->selected_id) == 0) { found = i; break; }
        }
    }

    if (found >= 0) {
        lk->selected = found;
        if (have_keep) {
            lk_light_t *l = &lk->lights[found];
            if (lk->param == LK_PARAM_BRIGHTNESS) l->brightness_pct = keep.brightness_pct;
            else                                  l->hue_deg = keep.hue_deg;
        }
    } else if (count > 0) {
        /* The selected light went away (or nothing was selected yet). Keep
         * the same row position, so the selection does not jump to the top. */
        lk->selected = clamp_int(old_index < 0 ? 0 : old_index, 0, count - 1);
        copy_id(lk->selected_id, lk->lights[lk->selected].entity_id);
        if (lk->mode == LK_MODE_ADJUST) {
            lk->pending = false;   /* the value belonged to the removed light */
            set_mode(lk, LK_MODE_SELECT, LK_PARAM_BRIGHTNESS, out);
        }
    } else {
        lk->selected = -1;
        if (lk->mode == LK_MODE_ADJUST) {
            lk->pending = false;
            set_mode(lk, LK_MODE_SELECT, LK_PARAM_BRIGHTNESS, out);
        }
    }

    /* The selected light can lose a capability (or become unavailable). */
    if (lk->mode == LK_MODE_ADJUST) {
        const lk_light_t *l = selected_light(lk);
        if (!param_supported(l, lk->param)) {
            lk_param_t other = (lk->param == LK_PARAM_BRIGHTNESS) ? LK_PARAM_HUE
                                                                 : LK_PARAM_BRIGHTNESS;
            lk->pending = false;
            if (param_supported(l, other)) set_mode(lk, LK_MODE_ADJUST, other, out);
            else                           set_mode(lk, LK_MODE_SELECT, LK_PARAM_BRIGHTNESS, out);
        }
    }

    if (out) {
        out->view_changed = true;
        /* In selection mode the detent count follows the list length. */
        if (lk->mode == LK_MODE_SELECT && (count != old_count || lk->selected != old_index))
            out->haptic_changed = true;
    }
}

void lk_enter(lk_t *lk, bool press_down, uint32_t now_ms, lk_output_t *out)
{
    lk->active = true;
    lk->mode = LK_MODE_SELECT;
    lk->param = LK_PARAM_BRIGHTNESS;
    lk->pending = false;
    lk->press_down = press_down;
    lk->press_tracking = false;
    lk->long_fired = false;
    lk->press_armed = !press_down;
    lk->last_press_report_ms = now_ms;
    if (lk->selected < 0 && lk->count > 0) {
        lk->selected = 0;
        copy_id(lk->selected_id, lk->lights[0].entity_id);
    }
    if (out) {
        out->haptic_changed = true;
        out->view_changed = true;
    }
}

void lk_leave(lk_t *lk, uint32_t now_ms, lk_output_t *out)
{
    if (!lk->active) return;
    send_pending(lk, now_ms, out);
    lk->active = false;
    lk->mode = LK_MODE_SELECT;
    lk->param = LK_PARAM_BRIGHTNESS;
    lk->press_tracking = false;
    lk->long_fired = false;
    if (out) out->view_changed = true;
}

void lk_press_level(lk_t *lk, bool down, uint32_t now_ms, lk_output_t *out)
{
    lk->last_press_report_ms = now_ms;
    if (down == lk->press_down) return;
    lk->press_down = down;

    if (down) {
        if (!lk->active || !lk->press_armed) return;
        lk->press_tracking = true;
        lk->long_fired = false;
        lk->press_start_ms = now_ms;
        return;
    }

    /* Release. */
    bool was_tracking = lk->press_tracking;
    bool was_long = lk->long_fired;
    lk->press_tracking = false;
    lk->long_fired = false;
    if (!lk->press_armed) {
        lk->press_armed = true;   /* the guard ends at the first release */
        return;
    }
    if (lk->active && was_tracking && !was_long) {
        /* A release that arrives late is still a long press, not a short one. */
        if ((uint32_t)(now_ms - lk->press_start_ms) >= LK_LONG_PRESS_MS)
            long_press(lk, now_ms, out);
        else
            short_press(lk, now_ms, out);
    }
}

void lk_press_event(lk_t *lk, uint32_t now_ms, lk_output_t *out)
{
    if (!lk->active) return;
    short_press(lk, now_ms, out);
}

void lk_rotate(lk_t *lk, int32_t delta, uint32_t now_ms, lk_output_t *out)
{
    if (!lk->active || delta == 0) return;
    lk_light_t *l = selected_light(lk);

    if (lk->mode == LK_MODE_SELECT) {
        if (lk->count <= 0) return;
        int next = clamp_int(lk->selected + (int)delta, 0, lk->count - 1);
        if (next != lk->selected) {
            lk->selected = next;
            copy_id(lk->selected_id, lk->lights[next].entity_id);
            if (out) out->view_changed = true;
        }
        return;
    }

    if (!param_supported(l, lk->param)) return;
    if (!lk->online) { set_feedback(out, LK_FEEDBACK_OFFLINE); return; }

    if (lk->param == LK_PARAM_BRIGHTNESS) {
        int v = clamp_int(l->brightness_pct + (int)delta * LK_BRIGHTNESS_STEP_PCT, 0, 100);
        if (v == l->brightness_pct) return;   /* at an end stop */
        l->brightness_pct = v;
        l->is_on = (v > 0);
        queue_value(lk, LK_PARAM_BRIGHTNESS, v, now_ms, out);
    } else {
        int v = wrap_hue(l->hue_deg + (int)delta * LK_HUE_STEP_DEG);
        if (v == l->hue_deg) return;
        l->hue_deg = v;
        queue_value(lk, LK_PARAM_HUE, v, now_ms, out);
    }
    if (out) out->view_changed = true;
}

void lk_tick(lk_t *lk, uint32_t now_ms, lk_output_t *out)
{
    if (lk->press_tracking && !lk->long_fired) {
        if ((uint32_t)(now_ms - lk->last_press_report_ms) > LK_PRESS_STALE_MS) {
            /* No press reports: the knob link is down. Cancel the press and
             * wait for a fresh release before the next press counts. */
            lk->press_tracking = false;
            lk->press_armed = false;
        } else if ((uint32_t)(now_ms - lk->press_start_ms) >= LK_LONG_PRESS_MS) {
            lk->long_fired = true;
            if (lk->active) long_press(lk, now_ms, out);
        }
    }

    if (lk->pending && (uint32_t)(now_ms - lk->last_send_ms) >= LK_SEND_INTERVAL_MS) {
        send_pending(lk, now_ms, out);
    }
}

void lk_set_online(lk_t *lk, bool online, uint32_t now_ms, lk_output_t *out)
{
    (void)now_ms;
    if (lk->online == online) return;
    lk->online = online;
    if (!online) {
        lk->pending = false;   /* never send a stale value after reconnecting */
        lk->sent_any = false;
    }
    if (out) out->view_changed = true;
}

void lk_haptic_profile(const lk_t *lk, lk_haptic_t *out)
{
    const lk_light_t *l = lk_selected(lk);
    memset(out, 0, sizeof(*out));
    if (lk->mode == LK_MODE_ADJUST && l && lk->param == LK_PARAM_BRIGHTNESS) {
        out->kind = LK_HAPTIC_BRIGHTNESS;
        out->position = l->brightness_pct;
        out->min_position = 0;
        out->max_position = 100;
    } else if (lk->mode == LK_MODE_ADJUST && l) {
        out->kind = LK_HAPTIC_HUE;
        out->position = 0;
        out->min_position = -1;
        out->max_position = -1;
    } else {
        out->kind = LK_HAPTIC_SELECT;
        out->position = lk->selected < 0 ? 0 : lk->selected;
        out->min_position = 0;
        out->max_position = lk->count > 0 ? lk->count - 1 : 0;
    }
}
