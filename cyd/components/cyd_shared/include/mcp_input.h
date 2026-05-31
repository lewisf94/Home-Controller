#pragma once
#include <stdint.h>
#include <stdbool.h>

void    mcp_input_init(void);
void    mcp_input_update(void);

/* Accumulated RE1 (main scroll/volume) delta since last call. Resets to 0. */
int32_t re1_get_delta(void);

/* True for exactly one mcp_input_update() call on confirmed press.
 * btn_index: 0=SW1 (prev), 1=SW2 (play/pause), 2=SW3 (next), 3=SW4 (view) */
bool    btn_get_event(uint8_t btn_index);

/* True while btn is held (stable, debounced). */
bool    btn_is_held(uint8_t btn_index);

/* True for exactly one call on confirmed RE1 push-switch press. */
bool    re1_sw_get_event(void);
