#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool ui_is_now_playing(void);
void ui_toggle_view(void);
void ui_play_centered_album(void);
void ui_scroll_browser(int32_t delta);
uint32_t ui_get_progress_ms(void);
int ui_get_device_volume(void);
void ui_show_volume_hud(int pct, bool muted);
void ui_show_toast(const char *msg, uint32_t duration_ms);
void ui_request_toggle_play(void);
void ui_request_prev(void);
void ui_request_next(void);
void ui_request_seek(uint32_t ms);
void ui_request_volume(int pct);
void ui_request_shuffle(void);
void ui_art_refresh(const uint8_t *rgb_data, uint16_t w, uint16_t h);
