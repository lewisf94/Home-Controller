#pragma once
#include <stdint.h>

// Initialise the CJMCU-2317 (MCP23017) IO expander.
// Call once in setup() after Serial.begin(), before ui_init().
void mcp_input_init();

// Poll encoders and buttons. Runs in the dedicated mcp_input_task (core 0);
// self-throttles to one I2C read per ~2 ms.
void mcp_input_update();

// Accumulated RE1 (main scroll) delta since last call.  Resets to 0 on read.
int32_t re1_get_delta();

// Accumulated RE2 (volume) delta since last call.  Resets to 0 on read.
int32_t re2_get_delta();

// Latched press event for SW1-SW4: set on a confirmed press edge, stays set
// until this read consumes it. btn_index: 0=SW1, 1=SW2, 2=SW3, 3=SW4
bool btn_get_event(uint8_t btn_index);

// True while SW1-SW4 is held (stable, debounced).
bool btn_is_held(uint8_t btn_index);

// Latched press event for the RE1 push-switch (consume-on-read).
bool re1_sw_get_event();

// Latched press event for the RE2 push-switch (consume-on-read).
bool re2_sw_get_event();
