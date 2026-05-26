/*
 * Minimal Sonos local control (UPnP/SOAP over the LAN, port 1400).
 *
 * Spotify marks Sonos as a restricted device, so the Spotify Web API refuses
 * transport commands targeting it. Sonos itself exposes an (undocumented but
 * long-stable) UPnP/SOAP API on http://<ip>:1400 that controls whatever is
 * currently playing on the speaker -- regardless of source. We use it to drive
 * play/pause/next/prev/seek/volume when the active Spotify device is the Sonos.
 *
 * These control the CURRENT queue/playback only. Starting a specific Spotify
 * album on the Sonos (SetAVTransportURI with a spotify: container URI + DIDL
 * metadata) needs the household's Spotify service id + account serial and is a
 * separate, fiddlier job -- not implemented here.
 *
 * `host` is the Sonos speaker's LAN IP (e.g. "192.168.1.50"). All calls are
 * blocking HTTP and return false on empty host or any transport/HTTP error.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

bool sonos_play(const char *host);
bool sonos_pause(const char *host);
bool sonos_next(const char *host);
bool sonos_previous(const char *host);
bool sonos_seek_ms(const char *host, uint32_t position_ms);
bool sonos_set_volume(const char *host, int pct);
