/*
 * Shared track-info struct (HA build).
 *
 * This build has no Spotify Web API client -- ha_client.c fills this struct
 * from Home Assistant state and pushes it to the UI via ui_set_track_info().
 * The file keeps its original name so ui.c's `#include "spotify.h"` is
 * unchanged from the non-HA build.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool      is_playing;
    char      title[64];
    char      artist[64];
    char      album[64];
    uint32_t  progress_ms;
    uint32_t  duration_ms;
    char      album_art_url[256];
} spotify_track_t;
