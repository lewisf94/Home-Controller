/*
 * Backend-neutral player track-info contract for the shared UI.
 *
 * The UI consumes this struct via ui_set_track_info() and never includes any
 * backend-specific header. Each build's backend fills the struct from its own
 * source: spotify.c (cyd/esp-idf, Spotify Web API) or ha_client.c
 * (cyd/esp-idf-ha, Home Assistant WebSocket). The per-build "spotify.h" files
 * (one a real Spotify client header, the other a thin wrapper) re-export this
 * via #include so existing backend code that says #include "spotify.h" keeps
 * working.
 *
 * The struct's type name (spotify_track_t) is kept for historical reasons;
 * renaming it ripples through every backend call site -- a future cleanup if
 * the broader backend-contract refactor (see project_arch-followups-shared-
 * scaffold memory) lands. The header name is neutral because the UI doesn't
 * care which backend produced the data.
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
    bool      shuffle_state;      /* true when shuffle is active */
    char      album_uri[64];      /* "spotify:album:..." -- empty if unknown; used to auto-snap the carousel */
} spotify_track_t;
