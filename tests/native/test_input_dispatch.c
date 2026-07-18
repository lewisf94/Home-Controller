#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int64_t g_now_us;
static bool g_now_playing;
static bool g_button_events[4];
static bool g_button_held[4];
static bool g_encoder_switch;
static int32_t g_encoder_delta;
static uint32_t g_progress_ms;
static int g_device_volume = -1;

static int g_toggle_view;
static int g_play_centered;
static int32_t g_scroll;
static int g_toggle_play;
static int g_prev;
static int g_next;
static int g_seek_calls;
static uint32_t g_seek_ms;
static int g_volume_calls;
static int g_volume;
static int g_shuffle;
static int g_hud_calls;
static int g_hud_volume;
static bool g_hud_muted;

int64_t esp_timer_get_time(void) { return g_now_us; }
bool re1_sw_get_event(void) { bool v = g_encoder_switch; g_encoder_switch = false; return v; }
bool btn_get_event(uint8_t index)
{
    if (index >= 4) return false;
    bool v = g_button_events[index];
    g_button_events[index] = false;
    return v;
}
bool btn_is_held(uint8_t index) { return index < 4 && g_button_held[index]; }
int32_t re1_get_delta(void) { int32_t v = g_encoder_delta; g_encoder_delta = 0; return v; }
void mcp_input_init(void) {}
void mcp_input_update(void) {}
bool mcp_input_has_pending(void) { return false; }

bool ui_is_now_playing(void) { return g_now_playing; }
void ui_toggle_view(void) { g_toggle_view++; }
void ui_play_centered_album(void) { g_play_centered++; }
void ui_scroll_browser(int32_t delta) { g_scroll += delta; }
uint32_t ui_get_progress_ms(void) { return g_progress_ms; }
int ui_get_device_volume(void) { return g_device_volume; }
void ui_show_volume_hud(int pct, bool muted)
{
    g_hud_calls++; g_hud_volume = pct; g_hud_muted = muted;
}
void ui_show_toast(const char *msg, uint32_t duration_ms) { (void)msg; (void)duration_ms; }
void ui_request_toggle_play(void) { g_toggle_play++; }
void ui_request_prev(void) { g_prev++; }
void ui_request_next(void) { g_next++; }
void ui_request_seek(uint32_t ms) { g_seek_calls++; g_seek_ms = ms; }
void ui_request_volume(int pct) { g_volume_calls++; g_volume = pct; }
void ui_request_shuffle(void) { g_shuffle++; }
void ui_art_refresh(const uint8_t *data, uint16_t w, uint16_t h)
{ (void)data; (void)w; (void)h; }

#include "../../../cyd/components/cyd_shared/input.c"

static void at_ms(uint32_t ms) { g_now_us = (int64_t)ms * 1000; }

static void scenario_browser(void)
{
    g_encoder_switch = true;
    g_button_events[0] = true;
    g_button_events[1] = true;
    g_button_events[2] = true;
    g_encoder_delta = 2;
    input_update();
    assert(g_play_centered == 2);
    assert(g_scroll == 2);
    assert(g_toggle_play == 0);
}

static void scenario_transport(void)
{
    g_now_playing = true;
    g_progress_ms = 6000;
    g_button_events[0] = true;
    g_button_events[1] = true;
    g_button_events[2] = true;
    input_update();
    assert(g_seek_calls == 1 && g_seek_ms == 0);
    assert(g_toggle_play == 1 && g_next == 1 && g_prev == 0);

    g_progress_ms = 5000;
    g_button_events[0] = true;
    input_update();
    assert(g_prev == 1);
}

static void scenario_volume(void)
{
    g_now_playing = true;
    g_device_volume = -1;
    g_encoder_delta = -1;
    input_update();
    assert(g_hud_calls == 0 && g_volume_calls == 0);

    g_device_volume = 40;
    input_update();
    g_encoder_delta = -1;
    at_ms(10);
    input_update();
    assert(g_hud_calls == 1 && g_hud_volume == 45);
    assert(g_volume_calls == 0);
    at_ms(310);
    input_update();
    assert(g_volume_calls == 0);
    at_ms(311);
    input_update();
    assert(g_volume_calls == 1 && g_volume == 45);
}

static void scenario_mute(void)
{
    g_now_playing = true;
    g_device_volume = 30;
    input_update();
    g_encoder_switch = true;
    input_update();
    assert(input_is_muted());
    assert(g_volume_calls == 1 && g_volume == 0 && g_hud_muted);
    g_encoder_switch = true;
    input_update();
    assert(!input_is_muted());
    assert(g_volume_calls == 2 && g_volume == 30 && !g_hud_muted);
}

static void scenario_short_press(void)
{
    g_button_events[3] = true;
    g_button_held[3] = true;
    at_ms(100);
    input_update();
    assert(input_needs_tick());
    g_button_held[3] = false;
    at_ms(200);
    input_update();
    assert(g_toggle_view == 1 && g_shuffle == 0);
}

static void scenario_long_press(void)
{
    g_now_playing = true;
    g_button_events[3] = true;
    g_button_held[3] = true;
    at_ms(100);
    input_update();
    at_ms(601);
    input_update();
    assert(g_shuffle == 1);
    g_button_held[3] = false;
    at_ms(700);
    input_update();
    assert(g_toggle_view == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (strcmp(argv[1], "browser") == 0) scenario_browser();
    else if (strcmp(argv[1], "transport") == 0) scenario_transport();
    else if (strcmp(argv[1], "volume") == 0) scenario_volume();
    else if (strcmp(argv[1], "mute") == 0) scenario_mute();
    else if (strcmp(argv[1], "short") == 0) scenario_short_press();
    else if (strcmp(argv[1], "long") == 0) scenario_long_press();
    else return 2;
    return 0;
}
