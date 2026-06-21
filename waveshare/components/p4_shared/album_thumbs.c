/*
 * Resolve album-thumbnail pointers into the embedded RGB565 blob.
 *
 * `album_thumbs.bin` is linked in via `EMBED_FILES` in CMakeLists.txt.
 * The IDF toolchain exposes the start/end symbols below; we index into
 * the buffer at index * ALBUM_THUMB_BYTES (every thumb is the same
 * fixed size, so no offset table is needed).
 */

#include "album_thumbs.h"

extern const uint8_t album_thumbs_bin_start[] asm("_binary_album_thumbs_bin_start");
extern const uint8_t album_thumbs_bin_end[]   asm("_binary_album_thumbs_bin_end");

size_t album_thumb_count(void)
{
    return (size_t)(album_thumbs_bin_end - album_thumbs_bin_start) / ALBUM_THUMB_BYTES;
}

const uint16_t *album_thumb_data(size_t index)
{
    if (index >= album_thumb_count()) return NULL;
    return (const uint16_t *)(album_thumbs_bin_start + index * ALBUM_THUMB_BYTES);
}
