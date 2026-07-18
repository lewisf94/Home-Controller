#pragma once

#include <stddef.h>
#include <stdint.h>

#define ALBUM_THUMB_W 4
#define ALBUM_THUMB_H 4

const uint16_t *album_thumb_data(size_t index);
