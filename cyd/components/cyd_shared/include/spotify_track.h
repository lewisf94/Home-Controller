/*
 * Shared track-info struct.
 *
 * The UI consumes this struct via ui_set_track_info(); each build's backend
 * fills it: spotify.c (cyd/esp-idf, Spotify Web API) or ha_client.c
 * (cyd/esp-idf-ha, Home Assistant WebSocket). Per-build "spotify.h" headers
 * include this file so the struct stays defined exactly once.
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
    int       volume_pct;         /* active device volume 0..100, or -1 if unknown */
} spotify_track_t;
