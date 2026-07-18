#pragma once

#include <stddef.h>

typedef struct {
    const char *title;
    const char *artist;
    const char *uri;
} album_entry_t;

const album_entry_t *albums_get(size_t index);
size_t albums_count(void);
