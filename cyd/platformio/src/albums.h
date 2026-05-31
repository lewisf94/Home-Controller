/*
 * Hardcoded album list for the browser carousel.
 *
 * Ported from the ESP-IDF build (cyd/esp-idf/main/albums.c) so both builds
 * share the same album set. Stored as a static array so the browser does not
 * depend on SD-card metadata; URIs are passed straight to spotify_play_album().
 */

#pragma once

#include <stddef.h>

typedef struct {
    const char *title;
    const char *artist;
    const char *uri;       /* spotify:album:... */
} album_entry_t;

const album_entry_t *albums_get(size_t index);
size_t               albums_count(void);
