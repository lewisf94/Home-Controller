#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Exclusive PCM stream access to the ES8311 used by network music players.
 * UI sound effects are suppressed from begin() until end(). */
bool   audio_stream_begin(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample);
size_t audio_stream_write(const uint8_t *data, size_t length, uint32_t timeout_ms);
void   audio_stream_end(void);
void   audio_stream_set_volume(uint8_t volume);
void   audio_stream_set_muted(bool muted);
bool   audio_stream_is_active(void);

#ifdef __cplusplus
}
#endif
