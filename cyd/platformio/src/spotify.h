#ifndef _SPOTIFY_H_
#define _SPOTIFY_H_

#include <Arduino.h>
#include <vector>

void spotify_init(const char* ssid, const char* password, const char* clientId,
                  const char* clientSecret, const char* refreshToken);

// Call this in loop to periodically refresh the token and poll player state
void spotify_update();

// Play a specific album URI (e.g., "spotify:album:123456")
bool spotify_play_album(const char* album_uri);

struct SpotifyTrackInfo {
    bool     is_playing;
    char     title[64];
    char     artist[64];
    char     album[64];
    uint32_t progress_ms;
    uint32_t duration_ms;
    char     album_art_url[128];
    int      local_album_idx;
    bool     shuffle_state;
    int      volume_pct;
};

extern SpotifyTrackInfo current_track_info;
extern bool             track_info_updated;

// Last known volume (0-100); updated from player state poll.
// Also written by spotify_set_volume() for immediate UI feedback.
extern int current_volume_pct;

// Playback state fetch (called internally by spotify_update)
void spotify_fetch_player_state();

// ── Playback control commands ─────────────────────────────────────────────
bool spotify_next_track();
bool spotify_prev_track();
bool spotify_toggle_play_pause();
bool spotify_toggle_shuffle();
bool spotify_set_volume(int pct);           // absolute 0-100
bool spotify_seek_position(int32_t pos_ms); // absolute seek in ms

#endif // _SPOTIFY_H_
