#pragma once

#include <stdbool.h>
#include <stdint.h>

void mcp_input_init(void);
void mcp_input_update(void);
int32_t re1_get_delta(void);
bool btn_get_event(uint8_t index);
bool btn_is_held(uint8_t index);
bool re1_sw_get_event(void);
bool mcp_input_has_pending(void);
