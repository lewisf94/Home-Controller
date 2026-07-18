#pragma once

#include <stdbool.h>

void input_init(void);
void input_update(void);
bool input_is_muted(void);
bool input_needs_tick(void);
