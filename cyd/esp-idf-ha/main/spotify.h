/*
 * Thin wrapper kept so main.c / ha_client.c can `#include "spotify.h"` without
 * caring that this build has no Spotify Web API client (ha_client.c fills the
 * same struct from Home Assistant state). The struct definition lives in the
 * shared component as player.h (backend-neutral name) so it stays in sync with
 * the non-HA build.
 */
#pragma once

#include "player.h"
