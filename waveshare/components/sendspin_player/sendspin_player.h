#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start the native Music Assistant Sendspin player. Safe to call once after
 * WiFi and the shared ES8311 audio service have been initialized. */
bool sendspin_player_start(void);

#ifdef __cplusplus
}
#endif

