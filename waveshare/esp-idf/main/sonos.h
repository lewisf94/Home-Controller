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

/* Current master volume 0..100, or -1 on error (RenderingControl GetVolume). */
int  sonos_get_volume(const char *host);

/* Start a Spotify album on the speaker via UPnP (SetAVTransportURI of the album
 * cpcontainer URI + Play). `album_uri` is "spotify:album:<id>". This switches
 * the speaker into native Sonos queue playback, ending any Spotify-Connect
 * session on it. Returns false on bad URI or any transport/HTTP/SOAP error.
 * The metadata's Spotify service identity (cdudn) is built for the household's
 * service -- see SONOS_SP_STYPE in sonos.c if a different household 500s. */
bool sonos_play_spotify_album(const char *host, const char *album_uri);

/* True if the speaker's AVTransport reports CurrentTransportState PLAYING. Used
 * to choose which configured speaker to start an album on when Spotify's
 * /me/player can't name the active device (Sonos-native playback is invisible
 * to the Web API). */
bool sonos_is_playing(const char *host);

/* Current track on the speaker, parsed from UPnP GetPositionInfo. Lets the
 * controller show now-playing while audio is on the Sonos (Spotify's Web API
 * returns 204 for native Sonos playback). */
typedef struct {
    char     title[64];
    char     artist[64];
    char     album[64];
    uint32_t progress_ms;
    uint32_t duration_ms;
    bool     is_playing;     /* PLAYING vs PAUSED (from GetTransportInfo) */
    int      volume;         /* master volume 0..100, or -1 if unknown */
} sonos_np_t;

/* Fill *out from the speaker's GetPositionInfo. Returns true only if a track is
 * actually loaded (non-empty title) -- false when stopped, unreachable, or in
 * Spotify-Connect passthrough (which reports NOT_IMPLEMENTED metadata). */
bool sonos_fetch_now_playing(const char *host, sonos_np_t *out);

/* Debug: log the speaker's current GetMediaInfo + GetPositionInfo (URI + DIDL
 * metadata) to serial. Used to reverse-engineer the Spotify container URI and
 * service descriptor needed to start an album on the Sonos. */
bool sonos_log_diag(const char *host);
