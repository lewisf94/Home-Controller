/*
 * Spotify Web API client (ESP-IDF).
 *
 * Read path:
 *   spotify_refresh_access_token()  - rotate OAuth token via refresh token
 *   spotify_fetch_player(&info)     - full player state into spotify_track_t
 *   spotify_fetch_now_playing(buf)  - legacy title-only helper
 *
 * Write path:
 *   spotify_play_album(uri)         - PUT /me/player/play with context_uri
 *
 * Threading: not thread-safe. Call from a single FreeRTOS task. Every
 * call blocks on HTTPS I/O for 0.5..2 s on a typical home connection.
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
    char      album_uri[64];      /* "spotify:album:..." -- empty if unknown; used to auto-snap the carousel */
} spotify_track_t;

void spotify_init(const char *client_id,
                  const char *client_secret,
                  const char *refresh_token);

bool spotify_refresh_access_token(void);

/* Populates *info with the current player state. Returns true on success.
 * Returns false if no active playback (HTTP 204) or on any error. The
 * album_art_url is cached internally; spotify_get_album_art_url() still
 * returns the same string. */
bool spotify_fetch_player(spotify_track_t *info);

/* Legacy helper retained for the title-only Step 5 path. Equivalent to
 * spotify_fetch_player() but copies just the title. */
bool spotify_fetch_now_playing(char *title_out, size_t title_len);

const char *spotify_get_album_art_url(void);

unsigned char *spotify_download_bytes(const char *url, size_t *out_len);
bool spotify_download_to_file(const char *url, const char *path, size_t *out_len);

/* PUT /v1/me/player/play with body {"context_uri": context_uri}.
 * Returns true on HTTP 2xx. */
bool spotify_play_album(const char *context_uri);

/* Playback controls -- all blocking HTTPS, call from the Spotify task. */
bool spotify_toggle_play_pause(void);
bool spotify_prev_track(void);
bool spotify_next_track(void);
bool spotify_seek_position(uint32_t position_ms);
bool spotify_set_volume(int pct);
bool spotify_toggle_shuffle(void);

/* A Spotify Connect device from GET /me/player/devices. */
typedef struct {
    char id[64];
    char name[40];
    char type[20];      /* "Computer", "Smartphone", "Speaker", ... */
    bool is_active;
} spotify_device_t;

/* List available Spotify Connect devices into out[0..max-1]; *count gets the
 * number written. Returns true on HTTP 200. Blocking HTTPS -- Spotify task. */
bool spotify_get_devices(spotify_device_t *out, int max, int *count);

/* Transfer playback to device_id (PUT /me/player, play=true). Blocking HTTPS. */
bool spotify_transfer_playback(const char *device_id);
