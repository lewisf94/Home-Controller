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

/* The track-info struct itself lives in the shared component so the HA build
 * (which doesn't link this Spotify Web API client) can use the same struct via
 * its own thin spotify.h wrapper. */
#include "spotify_track.h"

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
