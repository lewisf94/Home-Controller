#pragma once
#include <stdbool.h>

/* Call once at startup. */
void input_init(void);

/* Call from inside lvgl_port_lock -- may invoke LVGL APIs and ui_request_*
 * callbacks (which post to the Spotify command queue, safe from any task). */
void input_update(void);

bool input_is_muted(void);

/* True if input_update() needs to be called this tick for timing-sensitive
 * work (volume debounce, SW4 long-press) even when mcp_input_has_pending()
 * returns false. Used to gate the LVGL lock in input_task. */
bool input_needs_tick(void);
