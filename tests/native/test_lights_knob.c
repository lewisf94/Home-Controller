/* Host tests for the Lights knob state machine (waveshare/components/p4_shared/
 * lights_knob.c). The tests drive the module with a fake millisecond clock and
 * check the commands, haptic changes and feedback that it reports. They do not
 * test the RP2040, the motor, the strain gauge or Home Assistant. */

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "lights_knob.h"

static lk_light_t make_light(const char *id, bool dim, int bri, bool hue, int h)
{
    lk_light_t l;
    memset(&l, 0, sizeof l);
    snprintf(l.entity_id, sizeof l.entity_id, "%s", id);
    l.available = true;
    l.is_on = bri > 0;
    l.dimmable = dim;
    l.brightness_pct = bri;
    l.hue_supported = hue;
    l.hue_deg = h;
    l.sat_pct = 80;
    return l;
}

/* Three lights: dimmable + colour, dimmable only, on/off only. */
static lk_light_t g_lights[3];
static lk_t lk;
static uint32_t now;
static lk_output_t out;

static void reset_out(void) { memset(&out, 0, sizeof out); }

static void setup(void)
{
    g_lights[0] = make_light("light.lamp", true, 40, true, 30);
    g_lights[1] = make_light("light.desk", true, 70, false, 0);
    g_lights[2] = make_light("light.plug", false, 0, false, 0);
    lk_init(&lk);
    now = 1000;
    reset_out();
    lk_set_online(&lk, true, now, &out);
    lk_set_lights(&lk, g_lights, 3, &out);
    reset_out();
    lk_enter(&lk, false, now, &out);
    assert(out.haptic_changed);
    assert(lk.mode == LK_MODE_SELECT);
    assert(lk.selected == 0);
    reset_out();
}

/* Press down, hold for `ms` with ticks every 10 ms, release. */
static void press_for(uint32_t ms)
{
    lk_press_level(&lk, true, now, &out);
    for (uint32_t t = 0; t < ms; t += 10) {
        now += 10;
        lk_press_level(&lk, true, now, &out);   /* the knob keeps reporting */
        lk_tick(&lk, now, &out);
    }
    lk_press_level(&lk, false, now, &out);
}

static int count_cmds(lk_cmd_type_t type)
{
    int n = 0;
    for (int i = 0; i < out.cmd_count; i++) if (out.cmds[i].type == type) n++;
    return n;
}

/* Short press: fires once, on release, and toggles the selected light. */
static void short_press(void)
{
    setup();
    lk_press_level(&lk, true, now, &out);
    assert(out.cmd_count == 0);             /* nothing on press down */
    now += 120;
    lk_tick(&lk, now, &out);
    assert(out.cmd_count == 0);
    lk_press_level(&lk, false, now, &out);  /* release */
    assert(out.cmd_count == 1);
    assert(out.cmds[0].type == LK_CMD_TOGGLE);
    assert(strcmp(out.cmds[0].entity_id, "light.lamp") == 0);
    assert(lk.mode == LK_MODE_SELECT);
    assert(!lk_selected(&lk)->is_on);       /* optimistic toggle */
}

/* Long press: fires once at 500 ms, never also a short press. */
static void long_press(void)
{
    setup();
    lk_press_level(&lk, true, now, &out);
    now += 490;
    lk_press_level(&lk, true, now, &out);
    lk_tick(&lk, now, &out);
    assert(lk.mode == LK_MODE_SELECT);      /* 490 ms: not yet */
    now += 10;
    lk_press_level(&lk, true, now, &out);
    lk_tick(&lk, now, &out);
    assert(lk.mode == LK_MODE_ADJUST);      /* 500 ms: fired */
    assert(lk.param == LK_PARAM_BRIGHTNESS);
    assert(out.haptic_changed);
    reset_out();
    now += 1000;                            /* keep holding: fires only once */
    lk_press_level(&lk, true, now, &out);
    lk_tick(&lk, now, &out);
    assert(lk.mode == LK_MODE_ADJUST);
    lk_press_level(&lk, false, now, &out);  /* release: no short press */
    assert(out.cmd_count == 0);
    assert(lk.mode == LK_MODE_ADJUST);
    assert(lk.param == LK_PARAM_BRIGHTNESS);
}

/* A release that arrives after 500 ms without a tick is still a long press. */
static void late_release_is_long(void)
{
    setup();
    lk_press_level(&lk, true, now, &out);
    now += 600;
    lk_press_level(&lk, false, now, &out);
    assert(out.cmd_count == 0);
    assert(lk.mode == LK_MODE_ADJUST);
}

/* Mode switching: brightness <-> hue with short presses, back with long. */
static void mode_switching(void)
{
    setup();
    press_for(520);                          /* enter adjust */
    assert(lk.mode == LK_MODE_ADJUST && lk.param == LK_PARAM_BRIGHTNESS);
    reset_out();
    press_for(100);                          /* short: to hue */
    assert(lk.param == LK_PARAM_HUE);
    assert(out.haptic_changed);
    assert(out.cmd_count == 0);
    reset_out();
    press_for(100);                          /* short: back to brightness */
    assert(lk.param == LK_PARAM_BRIGHTNESS);
    reset_out();
    press_for(520);                          /* long: back to selection */
    assert(lk.mode == LK_MODE_SELECT);
    assert(lk.selected == 0);                /* same light kept */
    assert(strcmp(lk.selected_id, "light.lamp") == 0);
    assert(out.cmd_count == 0);              /* no command from mode changes */
}

/* Brightness: end stops at 0 and 100, rate-limited sends, final value sent. */
static void brightness_limits(void)
{
    setup();
    press_for(520);
    reset_out();
    now += 1000;
    lk_rotate(&lk, 5, now, &out);            /* 40 -> 45, sent at once */
    assert(count_cmds(LK_CMD_BRIGHTNESS) == 1);
    assert(out.cmds[0].value == 45);
    reset_out();
    for (int i = 0; i < 10; i++) {           /* fast turn: 10 detents in 50 ms */
        now += 5;
        lk_rotate(&lk, 1, now, &out);
    }
    assert(out.cmd_count == 0);              /* held back by the rate limit */
    now += LK_SEND_INTERVAL_MS;
    lk_tick(&lk, now, &out);
    assert(count_cmds(LK_CMD_BRIGHTNESS) == 1);
    assert(out.cmds[0].value == 55);         /* only the latest value */
    reset_out();
    now += 1000;
    lk_rotate(&lk, 200, now, &out);          /* clamp at 100 */
    assert(lk_selected(&lk)->brightness_pct == 100);
    assert(out.cmds[0].value == 100);
    reset_out();
    now += 1000;
    lk_rotate(&lk, 3, now, &out);            /* past the end stop: no command */
    assert(out.cmd_count == 0);
    now += 1000;
    lk_rotate(&lk, -300, now, &out);         /* clamp at 0 */
    assert(lk_selected(&lk)->brightness_pct == 0);
    assert(out.cmds[0].value == 0);
    lk_haptic_t h;
    lk_haptic_profile(&lk, &h);
    assert(h.kind == LK_HAPTIC_BRIGHTNESS && h.min_position == 0 && h.max_position == 100);

    /* A pending value is sent when the mode changes. */
    reset_out();
    now += 1000;
    lk_rotate(&lk, 10, now, &out);           /* sent at once (interval passed) */
    reset_out();
    now += 10;
    lk_rotate(&lk, 1, now, &out);            /* pending */
    assert(out.cmd_count == 0);
    press_for(520);                          /* long press back to selection */
    assert(count_cmds(LK_CMD_BRIGHTNESS) == 1);
    assert(out.cmds[0].value == 11);
    assert(count_cmds(LK_CMD_TOGGLE) == 0);
}

/* Hue: wraps around 0/360 and has no end stops. */
static void hue_wrapping(void)
{
    setup();
    press_for(520);
    press_for(100);                          /* to hue */
    assert(lk.param == LK_PARAM_HUE);
    lk_haptic_t h;
    lk_haptic_profile(&lk, &h);
    assert(h.kind == LK_HAPTIC_HUE && h.min_position == -1 && h.max_position == -1);
    reset_out();
    now += 1000;
    lk_rotate(&lk, -7, now, &out);           /* 30 - 35 = -5 -> 355 */
    assert(lk_selected(&lk)->hue_deg == 355);
    assert(out.cmds[0].type == LK_CMD_HUE && out.cmds[0].value == 355);
    assert(out.cmds[0].sat_pct == 80);
    reset_out();
    now += 1000;
    lk_rotate(&lk, 2, now, &out);            /* 355 + 10 = 365 -> 5 */
    assert(lk_selected(&lk)->hue_deg == 5);
    now += 1000;
    lk_rotate(&lk, 72 * 3, now, &out);       /* three full turns of hue */
    assert(lk_selected(&lk)->hue_deg == 5);
}

/* Selection: rotation moves between lights with end stops, refresh keeps the
 * selected entity even when the row order changes. */
static void change_selected_light(void)
{
    setup();
    lk_rotate(&lk, 1, now, &out);
    assert(lk.selected == 1 && strcmp(lk.selected_id, "light.desk") == 0);
    lk_rotate(&lk, 10, now, &out);
    assert(lk.selected == 2);
    lk_rotate(&lk, -10, now, &out);
    assert(lk.selected == 0);
    lk_rotate(&lk, 1, now, &out);            /* light.desk */
    lk_haptic_t h;
    lk_haptic_profile(&lk, &h);
    assert(h.kind == LK_HAPTIC_SELECT && h.max_position == 2 && h.position == 1);

    lk_light_t reordered[4] = {
        make_light("light.new", true, 10, false, 0),
        g_lights[2], g_lights[0], g_lights[1],
    };
    reset_out();
    lk_set_lights(&lk, reordered, 4, &out);
    assert(lk.selected == 3);
    assert(strcmp(lk.selected_id, "light.desk") == 0);
    assert(out.haptic_changed);              /* detent count changed */
    assert(out.cmd_count == 0);

    /* The selected light goes away: keep the same row position. */
    lk_light_t smaller[2] = { g_lights[0], g_lights[2] };
    lk_set_lights(&lk, smaller, 2, &out);
    assert(lk.selected == 1);

    /* Adjusting: a stale refresh does not move the local value back. */
    lk_rotate(&lk, -1, now, &out);           /* light.lamp */
    press_for(520);
    now += 1000;
    lk_rotate(&lk, 20, now, &out);           /* 40 -> 60 */
    lk_set_lights(&lk, smaller, 2, &out);    /* refresh still says 40 */
    assert(lk_selected(&lk)->brightness_pct == 60);
    assert(lk.mode == LK_MODE_ADJUST);
}

/* Lights without dimming or colour support. */
static void unsupported_controls(void)
{
    setup();
    lk_rotate(&lk, 1, now, &out);            /* light.desk: brightness only */
    press_for(520);
    assert(lk.mode == LK_MODE_ADJUST && lk.param == LK_PARAM_BRIGHTNESS);
    reset_out();
    press_for(100);                          /* no hue: stays on brightness */
    assert(lk.param == LK_PARAM_BRIGHTNESS);
    assert(out.feedback == LK_FEEDBACK_SINGLE_PARAM);
    press_for(520);                          /* back to selection */

    lk_rotate(&lk, 1, now, &out);            /* light.plug: on/off only */
    reset_out();
    press_for(520);
    assert(lk.mode == LK_MODE_SELECT);       /* no adjustment mode */
    assert(out.feedback == LK_FEEDBACK_NOT_ADJUSTABLE);
    assert(out.cmd_count == 0);
    reset_out();
    press_for(100);                          /* short press still toggles */
    assert(count_cmds(LK_CMD_TOGGLE) == 1);

    /* Colour only: adjustment starts on hue. */
    lk_light_t colour_only[1] = { make_light("light.strip", false, 0, true, 200) };
    lk_set_lights(&lk, colour_only, 1, &out);
    press_for(520);
    assert(lk.mode == LK_MODE_ADJUST && lk.param == LK_PARAM_HUE);

    /* The light loses its colour support while adjusting: back to selection. */
    colour_only[0].hue_supported = false;
    reset_out();
    lk_set_lights(&lk, colour_only, 1, &out);
    assert(lk.mode == LK_MODE_SELECT);

    /* Unavailable light: no toggle. */
    lk_light_t gone[1] = { make_light("light.gone", true, 50, false, 0) };
    gone[0].available = false;
    lk_set_lights(&lk, gone, 1, &out);
    reset_out();
    press_for(100);
    assert(out.cmd_count == 0 && out.feedback == LK_FEEDBACK_UNAVAILABLE);
    reset_out();
    press_for(520);
    assert(lk.mode == LK_MODE_SELECT && out.feedback == LK_FEEDBACK_UNAVAILABLE);

    /* Empty list. */
    lk_set_lights(&lk, NULL, 0, &out);
    reset_out();
    press_for(100);
    lk_rotate(&lk, 3, now, &out);
    assert(out.cmd_count == 0 && out.feedback == LK_FEEDBACK_NO_LIGHTS);
}

/* Network loss and reconnection: menu state kept, no stale commands. */
static void disconnect_reconnect(void)
{
    setup();
    press_for(520);
    now += 1000;
    lk_rotate(&lk, 5, now, &out);            /* sent */
    reset_out();
    now += 10;
    lk_rotate(&lk, 5, now, &out);            /* pending */
    lk_set_online(&lk, false, now, &out);
    assert(lk.mode == LK_MODE_ADJUST);       /* mode kept */
    assert(strcmp(lk.selected_id, "light.lamp") == 0);
    now += 1000;
    lk_tick(&lk, now, &out);
    assert(out.cmd_count == 0);              /* pending value dropped */
    lk_rotate(&lk, 5, now, &out);
    assert(out.cmd_count == 0 && out.feedback == LK_FEEDBACK_OFFLINE);
    reset_out();
    press_for(520);                          /* long press: mode change only */
    assert(lk.mode == LK_MODE_SELECT);
    reset_out();
    press_for(100);                          /* short press: refused, no toggle */
    assert(out.cmd_count == 0 && out.feedback == LK_FEEDBACK_OFFLINE);
    lk_rotate(&lk, 1, now, &out);            /* selection still works offline */
    assert(lk.selected == 1);

    reset_out();
    lk_set_online(&lk, true, now, &out);
    lk_set_lights(&lk, g_lights, 3, &out);   /* fresh list after reconnect */
    assert(lk.selected == 1);
    assert(out.cmd_count == 0);              /* reconnecting sends nothing */
    press_for(100);
    assert(count_cmds(LK_CMD_TOGGLE) == 1);
    assert(strcmp(out.cmds[0].entity_id, "light.desk") == 0);
}

/* Changing modes or menus does not send an unintended command. */
static void no_unintended_commands(void)
{
    /* A press that is down when the Lights screen opens is ignored. */
    lk_init(&lk);
    now = 5000;
    reset_out();
    g_lights[0] = make_light("light.lamp", true, 40, true, 30);
    g_lights[1] = make_light("light.desk", true, 70, false, 0);
    g_lights[2] = make_light("light.plug", false, 0, false, 0);
    lk_set_online(&lk, true, now, &out);
    lk_set_lights(&lk, g_lights, 3, &out);
    lk_enter(&lk, true, now, &out);
    reset_out();
    now += 800;
    lk_press_level(&lk, true, now, &out);
    lk_tick(&lk, now, &out);                 /* no long press from that hold */
    lk_press_level(&lk, false, now, &out);   /* no short press on its release */
    assert(out.cmd_count == 0 && lk.mode == LK_MODE_SELECT);
    press_for(100);                          /* the next press works */
    assert(count_cmds(LK_CMD_TOGGLE) == 1);

    /* Link loss while the press is down: no long press from stale data. */
    setup();
    lk_press_level(&lk, true, now, &out);
    now += LK_PRESS_STALE_MS + 50;           /* no reports arrive */
    lk_tick(&lk, now, &out);
    now += 500;
    lk_tick(&lk, now, &out);
    assert(lk.mode == LK_MODE_SELECT && out.cmd_count == 0);
    lk_press_level(&lk, false, now, &out);   /* release ends the guard only */
    assert(out.cmd_count == 0);
    press_for(100);
    assert(count_cmds(LK_CMD_TOGGLE) == 1);

    /* Leaving the menu sends only the pending final value. */
    setup();
    press_for(520);
    now += 1000;
    lk_rotate(&lk, 1, now, &out);
    reset_out();
    now += 10;
    lk_rotate(&lk, 1, now, &out);
    lk_leave(&lk, now, &out);
    assert(out.cmd_count == 1 && out.cmds[0].type == LK_CMD_BRIGHTNESS);
    assert(out.cmds[0].value == 42);
    reset_out();
    press_for(100);                          /* inactive: presses do nothing */
    lk_rotate(&lk, 4, now, &out);
    assert(out.cmd_count == 0);

    /* The same value is not sent twice. */
    setup();
    press_for(520);
    now += 1000;
    lk_rotate(&lk, 1, now, &out);            /* 41 sent */
    reset_out();
    now += 10;
    lk_rotate(&lk, 1, now, &out);            /* 42 pending */
    now += 10;
    lk_rotate(&lk, -1, now, &out);           /* back to 41 pending */
    now += LK_SEND_INTERVAL_MS;
    lk_tick(&lk, now, &out);
    assert(out.cmd_count == 0);

    /* Older RP2040 firmware: a press event is a short press only. */
    setup();
    lk_press_event(&lk, now, &out);
    assert(count_cmds(LK_CMD_TOGGLE) == 1 && lk.mode == LK_MODE_SELECT);
}

int main(int argc, char **argv)
{
    const char *s = argc > 1 ? argv[1] : "all";
    bool all = strcmp(s, "all") == 0;
    if (all || !strcmp(s, "short"))       short_press();
    if (all || !strcmp(s, "long"))        { long_press(); late_release_is_long(); }
    if (all || !strcmp(s, "modes"))       mode_switching();
    if (all || !strcmp(s, "brightness"))  brightness_limits();
    if (all || !strcmp(s, "hue"))         hue_wrapping();
    if (all || !strcmp(s, "selection"))   change_selected_light();
    if (all || !strcmp(s, "unsupported")) unsupported_controls();
    if (all || !strcmp(s, "network"))     disconnect_reconnect();
    if (all || !strcmp(s, "unintended"))  no_unintended_commands();
    printf("lights knob tests passed (%s)\n", s);
    return 0;
}
