/*
 * app_core_art -- shared double-buffered PSRAM album-art buffer, used by both
 * waveshare/esp-idf and waveshare/esp-idf-ha.
 *
 * The decode/download step differs per build (RAM vs LittleFS-file source,
 * Spotify vs HA transport) and stays in each build's own main.c. What's
 * identical is the buffer lifecycle: decode into whichever slot ISN'T the one
 * currently on screen, then swap -- so the render task's current buffer is
 * never overwritten mid-draw. See CLAUDE.md's Waveshare section for why a
 * single shared buffer tore the on-screen cover.
 *
 * Usage:
 *   static art_buffer_t s_art;
 *   art_buffer_alloc(&s_art, ART_W * ART_H * 2);
 *   ...
 *   uint8_t *buf = art_buffer_idle(&s_art);
 *   if (buf && decode_into(buf, &w, &h)) art_buffer_publish(&s_art, w, h);
 */
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *buf[2];
    int      idx;
} art_buffer_t;

/* Allocates both buffers in PSRAM. Logs and returns false if either fails
 * (ab->buf[] stays whatever heap_caps_malloc returned -- check before use). */
bool art_buffer_alloc(art_buffer_t *ab, size_t bytes);

/* Returns the buffer NOT currently published (safe to decode into), or NULL
 * if allocation failed. */
uint8_t *art_buffer_idle(art_buffer_t *ab);

/* Call after a successful decode into the pointer art_buffer_idle() returned.
 * Publishes it to the UI (ui_art_refresh) and flips the idle/live index. */
void art_buffer_publish(art_buffer_t *ab, uint16_t w, uint16_t h);

#ifdef __cplusplus
}
#endif
