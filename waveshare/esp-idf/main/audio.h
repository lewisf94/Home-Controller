/*
 * UI sound effects for the Waveshare P4 (onboard ES8311 speaker).
 *
 * Sounds are SYNTHESISED at runtime (sine / square / triangle + envelope) -- no
 * audio assets. A dedicated FreeRTOS task drains a small queue and writes the
 * synthesised PCM to the codec, so callers (UI / input) never block on the I2S
 * write.
 *
 * Usage:  audio_init();  ... audio_play(AUDIO_SFX_TICK);
 */
#pragma once

#include <stdbool.h>

typedef enum {
    AUDIO_SFX_TICK = 0,   /* short blip -- carousel detent / scroll step */
    AUDIO_SFX_SELECT,     /* rising two-tone -- album selected / played   */
    AUDIO_SFX_BACK,       /* soft low blip  -- view change / back / option */
    AUDIO_SFX_CONNECT,    /* multi-note chime -- WiFi/Spotify connected   */
    AUDIO_SFX_COUNT
} audio_sfx_t;

/* Sound character hint, derived from the active visual MODE. When the user
 * leaves the sound set on AUTO, this picks the matching set. */
typedef enum {
    AUDIO_THEME_MODERN = 0,   /* clean sine set                          */
    AUDIO_THEME_PIXEL,        /* chiptune square set                     */
    AUDIO_THEME_AMBIENT,      /* soft, lower, longer-decay set           */
    AUDIO_THEME_COUNT
} audio_theme_t;

/* Bring up I2S + the ES8311 speaker codec and start the audio task. Safe to call
 * once at boot; logs and no-ops the playback path if the codec fails to init. */
void audio_init(void);

/* Queue a sound for playback (non-blocking; dropped if sound is off or the queue
 * is full). Safe to call from any task. */
void audio_play(audio_sfx_t sfx);

/* Master enable for UI sounds (persisted by the caller via NVS). */
void audio_set_enabled(bool enabled);
bool audio_is_enabled(void);

/* Playback volume, 0..100 (persisted by the caller via NVS). Applied as a
 * perceptual (square-law) software gain so the quiet end has fine control. */
void audio_set_volume(int vol);
int  audio_get_volume(void);

/* MODE-derived default palette, used when the sound set is on AUTO. */
void audio_set_theme(audio_theme_t theme);

/* Sound-set selection. The UI presents a list: index 0 = AUTO (follow MODE),
 * then the named sets below. audio_set_set(-1) selects AUTO; >=0 forces a set.
 * audio_get_set() returns the forced index or -1 for AUTO. */
void        audio_set_set(int set);
int         audio_get_set(void);
int         audio_set_count(void);      /* number of named (non-AUTO) sets */
const char *audio_set_name(int i);      /* name of named set i (0..count-1) */
