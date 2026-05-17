/*
 * Spotify Web API client (ESP-IDF, Phase 2 Step 5).
 *
 * Minimal slice for the Step 5 milestone: refresh an OAuth access token
 * and fetch the current track title from /v1/me/player. Subsequent steps
 * will extend this with /play, /pause, /next, /previous, /seek, /volume
 * and the now-playing image URL.
 *
 * Threading: not thread-safe. Call from a single FreeRTOS task (the
 * one spawned in app_main()). Every call blocks on HTTPS I/O for
 * 0.5..2 s on a typical home connection.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

void spotify_init(const char *client_id,
                  const char *client_secret,
                  const char *refresh_token);

/* Rotates the access token using the refresh token. Returns true on
 * success. The token is cached internally and reused until ~1 minute
 * before its expiry, at which point the next API call refreshes it. */
bool spotify_refresh_access_token(void);

/* Calls GET /v1/me/player. On success writes the current track title
 * (UTF-8, NUL-terminated, truncated to title_len-1 chars) and returns
 * true. Returns false if nothing is currently playing (HTTP 204) or
 * the request fails. Caller supplies the buffer. */
bool spotify_fetch_now_playing(char *title_out, size_t title_len);
