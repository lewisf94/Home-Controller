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

/* spotify_track_t is defined in player.h (the backend-neutral contract shared
 * between this build and the HA build). Re-exported here so existing call
 * sites that say #include "spotify.h" keep working unchanged. */
#include "player.h"

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

/* HTTP status of the most recent playback command (-1 on a pre-request failure).
 * Lets the dispatcher tell a 403 "restricted device" from other failures. */
int  spotify_last_cmd_status(void);

/* Diagnostics: seconds until token refresh (-1 if unknown), and seconds left on
 * a 429 poll backoff (0 if none). */
int  spotify_token_expiry_seconds(void);
int  spotify_poll_holdoff_seconds(void);

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

/* One Spotify album candidate for the on-device Add Albums flow.
 * LAYOUT CONTRACT: must stay byte-identical to ui.c's private
 * ui_album_candidate_t and ha_client.c's ha_album_candidate_t -- the list is
 * passed to ui_set_album_candidates() through a void* seam while the runtime
 * catalogue is a prototype. Change all three together (or promote the struct
 * to a shared header when this graduates). */
typedef struct {
    char title[80];
    char artist[56];
    char uri[64];        /* spotify:album:... */
    char image_url[100]; /* cover art URL (images[0]); "" if none */
} spotify_album_candidate_t;

/* First page of the user's saved albums. Requires the OAuth refresh token to
 * include Spotify's user-library-read scope (mint one with
 * get_spotify_token.py, repo root). Returns true on HTTP 200. On failure fills
 * `err` (when non-NULL) with a short user-facing reason for the add screen. */
bool spotify_get_saved_albums(spotify_album_candidate_t *out, int max, int *count,
                              char *err, size_t err_len);

/* Album search (GET /v1/search?type=album&q=...). Uses the same user access
 * token as everything else, but /v1/search needs NO user-library scope, so it
 * works with any valid token -- unlike spotify_get_saved_albums. Returns true
 * on HTTP 200; on failure fills `err` (when non-NULL) with a short reason. */
bool spotify_search_albums(const char *query, spotify_album_candidate_t *out,
                           int max, int *count, char *err, size_t err_len);
