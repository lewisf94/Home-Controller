#pragma once

void input_init();
void input_update();

// State queries used by ui.cpp for overlays
bool    input_is_muted();
bool    input_sw4_seek_active();
int32_t input_sw4_seek_offset_ms();
