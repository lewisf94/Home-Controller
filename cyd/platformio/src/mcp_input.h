#pragma once
#include <stdint.h>

// Initialise the CJMCU-2317 (MCP23017) IO expander.
// Call once in setup() after Serial.begin(), before ui_init().
void mcp_input_init();

// Poll encoders and buttons.  Call every loop() iteration (replaces encoder_poll()).
void mcp_input_update();

// Accumulated RE1 (main scroll) delta since last call.  Resets to 0 on read.
int32_t re1_get_delta();

// Accumulated RE2 (volume) delta since last call.  Resets to 0 on read.
int32_t re2_get_delta();

// True for exactly one mcp_input_update() frame on confirmed press of SW1-SW4.
// btn_index: 0=SW1, 1=SW2, 2=SW3, 3=SW4
bool btn_get_event(uint8_t btn_index);

// True while SW1-SW4 is held (stable, debounced).
bool btn_is_held(uint8_t btn_index);

// True for exactly one frame on confirmed press of the RE1 push-switch.
bool re1_sw_get_event();

// True for exactly one frame on confirmed press of the RE2 push-switch.
bool re2_sw_get_event();
