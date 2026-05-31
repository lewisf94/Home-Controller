/*
 * Resolve album-thumbnail pointers into the embedded RGB565 blob.
 *
 * album_thumbs_data.c provides the byte array (generated from
 * album_thumbs.bin). Every thumb is the same fixed size, so indexing is just
 * index * ALBUM_THUMB_BYTES with no offset table.
 */

#include "album_thumbs.h"

extern "C" {
extern const uint8_t       album_thumbs_bin[];
extern const unsigned long album_thumbs_bin_len;
}

size_t album_thumb_count(void)
{
    return (size_t)album_thumbs_bin_len / ALBUM_THUMB_BYTES;
}

const uint16_t *album_thumb_data(size_t index)
{
    if (index >= album_thumb_count()) return nullptr;
    return (const uint16_t *)(album_thumbs_bin + index * ALBUM_THUMB_BYTES);
}
