/*
 * Backend-neutral player track-info contract for the shared P4 UI.
 *
 * The UI (ui.c) consumes this struct via ui_set_track_info() and never
 * includes any backend-specific header. Each per-build backend fills the
 * struct from its own source:
 *   spotify.c  -- waveshare/esp-idf     (direct Spotify Web API)
 *   ha_client.c -- waveshare/esp-idf-ha (Home Assistant WebSocket)
 *
 * The per-build spotify.h / ha_client.h re-export this struct via
 * #include "player.h" so existing backend code that says
 * #include "spotify.h" keeps compiling unchanged.
 *
 * The struct type name (spotify_track_t) is kept for historical reasons;
 * renaming it would ripple through every backend call site. The header
 * name is neutral because the UI doesn't care which backend produced the
 * data.
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
    bool      device_restricted;  /* active device won't accept Spotify Web API control (e.g. Sonos) */
    char      device_name[64];    /* active device name, for routing/UX */
    int       volume_pct;         /* active device volume 0..100, or -1 if unknown */
    bool      shuffle_state;      /* true when shuffle is active */
    char      album_uri[64];      /* "spotify:album:..." -- empty if unknown; auto-snaps the carousel */
} spotify_track_t;
