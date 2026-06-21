/*
 * Album-art JPEG decoder facade.
 *
 * C API in front of bitbank2's JPEGDEC (which is C++). Lives in its
 * own translation unit so main.c can stay C while we use the C++
 * library underneath.
 *
 * We swapped to JPEGDEC from espressif/esp_jpeg (TJpgDec) because
 * Spotify's CDN serves JPEGs that TJpgDec refuses with JDR_FMT1
 * (data format error) -- TJpgDec is fussy about progressive scans,
 * extra APPn segments, ICC profiles, etc. JPEGDEC is what the
 * working Arduino build uses; see docs/PORT-NOTES.md Step 6.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decodes the JPEG bytes at `jpeg` into RGB565 pixels written to
 * `out_rgb`. The decoder auto-picks a downscale factor (1, 1/2, or
 * 1/4, 1/8) so the output fits in `out_max_pixels` and is closest to
 * 320x320 -- our now-playing art size on the 800x480 panel.
 *
 * On success, returns true and writes the decoded dimensions to
 * *out_w / *out_h. The buffer is in native byte order (matches
 * LVGL with swap_bytes=true in the display config). */
bool album_art_decode(const uint8_t *jpeg, size_t jpeg_len,
                      uint16_t *out_rgb, size_t out_max_pixels,
                      uint16_t *out_w, uint16_t *out_h);

/* Same contract, but reads the JPEG via JPEGDEC's POSIX-backed file
 * path instead of from a RAM buffer. This matches what the Arduino
 * build does (the openRAM path appears to mis-handle Spotify's
 * mozjpeg-encoded streams in JPEGDEC 1.6.2). The file at `path` must
 * exist and contain a complete JPEG. */
bool album_art_decode_file(const char *path,
                           uint16_t *out_rgb, size_t out_max_pixels,
                           uint16_t *out_w, uint16_t *out_h);

#ifdef __cplusplus
}
#endif
