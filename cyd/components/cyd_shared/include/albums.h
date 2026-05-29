/*
 * Hardcoded album list for the browser carousel.
 *
 * Mirrors scripts/sd_card_albums/metadata.csv from the PlatformIO build.
 * Stored as a static C array so we don't need SD card / filesystem
 * access; URIs are passed straight to spotify_play_album().
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
