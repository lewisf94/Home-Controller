/*
 * Album-thumbnail blob accessor.
 *
 * The browser carousel needs a 220x220 RGB565 image per album (the P4 panel is
 * 800x480; the CYD build uses 120x120). We pre-
 * convert the source artwork on the dev machine (see
 * scripts/embed_albums_idf.py), concatenate all 14 thumbs into
 * album_thumbs.bin, and pull that into firmware via `EMBED_FILES` in
 * the component's CMakeLists.txt. Bytes are little-endian RGB565 so they
 * land in LVGL's pixel buffer without any byte-swap on our side -- the
 * panel write path already swaps once via display_cfg.flags.swap_bytes.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#define ALBUM_THUMB_W      220
#define ALBUM_THUMB_H      220
#define ALBUM_THUMB_BYTES  (ALBUM_THUMB_W * ALBUM_THUMB_H * 2)

/* Pointer to album `index`'s RGB565 pixels, or NULL if out of range. */
const uint16_t *album_thumb_data(size_t index);

/* How many thumbs are baked into the firmware. Should equal albums_count(). */
size_t album_thumb_count(void);
