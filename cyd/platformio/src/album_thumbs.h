/*
 * Album-thumbnail blob accessor (Arduino build).
 *
 * The browser carousel needs a 120x120 RGB565 image per album. The same
 * blob the ESP-IDF build embeds via EMBED_FILES is compiled in here as a C
 * array (album_thumbs_data.c, generated from album_thumbs.bin). Bytes are
 * little-endian RGB565.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#define ALBUM_THUMB_W      120
#define ALBUM_THUMB_H      120
#define ALBUM_THUMB_BYTES  (ALBUM_THUMB_W * ALBUM_THUMB_H * 2)

#ifdef __cplusplus
extern "C" {
#endif

/* Pointer to album `index`'s RGB565 pixels, or NULL if out of range. */
const uint16_t *album_thumb_data(size_t index);

/* How many thumbs are baked into the firmware. Should equal albums_count(). */
size_t album_thumb_count(void);

#ifdef __cplusplus
}
#endif
