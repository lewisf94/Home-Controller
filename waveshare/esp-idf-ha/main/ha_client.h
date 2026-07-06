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

/* Open the WebSocket and begin the auth + subscribe handshake. Non-blocking;
 * state arrives asynchronously via the event handler. */
void ha_client_start(void);

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
 * new one, refresh now-playing). */
void ha_request_devices(void);
void ha_set_active_entity(const char *entity_id);

/* Lights (HA build only). ha_request_lights() asks HA for all light.*
 * entities and pushes them to the UI via ui_set_lights(). Toggle and
 * brightness are direct call_service commands, same fire-and-forget contract
 * as the playback commands above -- the row-tap/slider handler doesn't wait
 * for a reply; re-opening the screen (a fresh ha_request_lights()) picks up
 * whatever state actually landed. */
void ha_request_lights(void);
bool ha_light_toggle(const char *entity_id);
bool ha_light_set_brightness(const char *entity_id, int pct);

/* Album art: when a track change brings a new entity_picture, the event
 * handler stashes its relative URL. The ha task polls this (consume-once),
 * builds the absolute URL, downloads + decodes it off the WebSocket task.
 * Returns true and fills `rel_out` if a new picture is pending. */
bool ha_take_pending_art(char *rel_out, size_t out_len);

/* Build absolute "http://<host>:<port><rel>" for an entity_picture path. */
void ha_art_full_url(const char *rel, char *out, size_t out_len);

/* GET `url` into a file (LittleFS). Returns true on HTTP 200. */
bool ha_download_to_file(const char *url, const char *path, size_t *out_len);
