/*
 * Home Assistant client (WebSocket) -- HA build backend.
 *
 * Replaces spotify.c. Connects to the HA WebSocket API, authenticates with a
 * long-lived token, subscribes to a Music Assistant media_player entity, and
 * maps its state into the shared spotify_track_t (so ui.c / input.c are
 * unchanged). Playback commands are HA service calls.
 *
 * Threading: the esp_websocket_client runs its own task and delivers inbound
 * frames to an event handler that parses state and pushes it to ui.c. Outbound
 * commands and album-art download run from the caller's task (main.c's ha task);
 * esp_websocket_client_send_text is safe to call from any task.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Store config (pointers must stay valid for the program's lifetime --
 * normally string literals from secrets.h). */
void ha_client_init(const char *host, int port,
                    const char *token, const char *entity);

/* Optional Spotify Web API credentials used by the HA build's Add Albums
 * search. Pointers must stay valid for the program's lifetime. The refresh
 * token is retained for future account-library flows; public album search only
 * needs client_id + client_secret via Spotify's client-credentials flow. */
void ha_spotify_init(const char *client_id, const char *client_secret,
                     const char *refresh_token);

/* Open the WebSocket and begin the auth + subscribe handshake. Non-blocking;
 * state arrives asynchronously via the event handler. */
void ha_client_start(void);

/* True after HA has accepted the long-lived token for the current WebSocket. */
bool ha_client_is_authenticated(void);

/* True once authentication and the initial get_states response are complete. */
bool ha_client_is_ready(void);

/* Periodic housekeeping from the HA task: catches album browse requests that
 * HA never answers or whose response was too large to reassemble. */
void ha_client_tick(void);

/* Playback commands -- each builds a call_service frame and sends it.
 * Returns true if the WebSocket send succeeded (does not confirm HA executed it). */
bool ha_toggle_play_pause(void);
bool ha_prev_track(void);
bool ha_next_track(void);
bool ha_toggle_shuffle(void);
bool ha_seek_position(uint32_t position_ms);
bool ha_set_volume(int pct);
bool ha_play_album(const char *spotify_uri);   /* "spotify:album:ID" */

/* Device list / switching (HA build only).
 * ha_request_devices() asks HA for all media_player entities and pushes them to
 * the UI via ui_set_devices(). ha_set_active_entity() re-points the controller
 * at a different media_player at runtime (unsubscribe old trigger, subscribe the
 * new one, refresh now-playing).
 *
 * If the HA core Spotify integration is installed, its account entity's
 * source_list (the Spotify Connect devices: phone, laptop, ...) is expanded
 * into extra rows tagged SPOTIFY CONNECT; tapping one transfers playback via
 * media_player.select_source and follows the account entity for state. Album
 * starts against that entity use media_player.play_media (spotify: URI), since
 * it is not a Music Assistant player. */
void ha_request_devices(void);
void ha_set_active_entity(const char *entity_id);

/* Album candidates for the shared Add Albums screen. A non-empty query searches
 * Spotify's catalogue directly. An empty query keeps the older HA media-browser
 * discovery fallback, so catalogue adding is not tied to the currently selected
 * speaker being online. Results are pushed asynchronously to
 * ui_set_album_candidates(). */
void ha_request_album_candidates(const char *query);

/* Music Assistant queue controls. The active output must be a Music Assistant
 * player; Spotify Connect and ordinary HA renderer rows intentionally report a
 * short explanation instead of issuing a service call they cannot handle. */
void ha_request_queue(void);
bool ha_queue_add(const char *spotify_uri, bool play_next);
bool ha_queue_clear(void);
void ha_search_queue_tracks(const char *query);

/* Lights (HA build only). ha_request_lights() asks HA for all light.*
 * entities and pushes them to the UI via ui_set_lights(). Toggle and
 * brightness are direct call_service commands, same fire-and-forget contract
 * as the playback commands above -- the row-tap/slider handler doesn't wait
 * for a reply; re-opening the screen (a fresh ha_request_lights()) picks up
 * whatever state actually landed. */
void ha_request_lights(void);
/* Post-command settle refresh. Non-blocking: arms a coalesced deadline that
 * ha_client_tick() honours ~0.7 s after the LAST light command, skipping the
 * stale-cache push so a just-toggled row is not repainted with pre-command
 * state. The eventual snapshot bypasses the 15 s inventory cooldown but runs
 * under stricter guards than a normal fetch: a 5 s floor between forced
 * snapshots, the larger 64/32 KB internal reserve, and full suppression while
 * music is streaming (the UI's optimistic state stands until playback ends or
 * the next natural refresh). */
void ha_request_lights_fresh(void);
bool ha_light_toggle(const char *entity_id);
bool ha_light_set_brightness(const char *entity_id, int pct);
bool ha_light_set_hs(const char *entity_id, int hue_deg, int sat_pct);

/* Album art: when a track change brings a new entity_picture, the event
 * handler stashes its relative URL. The ha task polls this (consume-once),
 * builds the absolute URL, downloads + decodes it off the WebSocket task.
 * Returns true and fills `rel_out` if a new picture is pending. */
bool ha_take_pending_art(char *rel_out, size_t out_len);

/* Build absolute "http://<host>:<port><rel>" for an entity_picture path. */
void ha_art_full_url(const char *rel, char *out, size_t out_len);

/* GET `url` into a file (LittleFS). Returns true on HTTP 200. Always attaches
 * the TLS cert bundle so https URLs (e.g. Spotify cover art) work; set
 * `send_ha_auth` true only for HA-host endpoints (media_player_proxy needs the
 * Bearer token) -- false for external URLs so the HA token isn't leaked to a
 * third party. */
bool ha_download_to_file(const char *url, const char *path, size_t *out_len,
                         bool send_ha_auth);
