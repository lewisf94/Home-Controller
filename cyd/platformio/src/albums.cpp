/*
 * GENERATED FILE -- do not edit by hand.
 *
 * Source of truth: spotify-albums-list.txt (repo root, gitignored).
 * Regenerate with:  python scripts/gen_albums.py
 *
 * Sorted by artist then title (leading "The"/"A"/"An" ignored).
 */

#include "albums.h"

static const album_entry_t s_albums[] = {
    { "Tranquility Base Hotel & Casino",               "Arctic Monkeys",    "spotify:album:7v6FNgLDS8KmaWA1amUtqe" },
    { "Whatever People Say I Am, That's What I'm Not", "Arctic Monkeys",    "spotify:album:50Zz8CkIhATKUlQMbHO3k1" },
    { "Social Cues",                                   "Cage the Elephant", "spotify:album:2VuZJsJBPLwg9BeQFQle8G" },
    { "Dance, No One's Watching",                      "Ezra Collective",   "spotify:album:2BwKd9lWotQIhROHSWQ78h" },
    { "O",                                             "Fcukers",           "spotify:album:4RrsgnUbZIFTw42Apa8lXO" },
    { "What Went Down",                                "Foals",             "spotify:album:0RyCpIKlCV0kgEuzrmp73O" },
    { "Skinty Fia",                                    "Fontaines D.C.",    "spotify:album:0aVN6rMKthfuAdDXn8RTXf" },
    { "Gorillaz",                                      "Gorillaz",          "spotify:album:4tUxQkrduOE8sfgwJ5BI2F" },
    { "Plastic Beach",                                 "Gorillaz",          "spotify:album:2dIGnmEIy1WZIcZCFSj6i8" },
    { "Chaos for the Fly",                             "Grian Chatten",     "spotify:album:7IO2RSWhcIt8Tliya8fCZL" },
    { "To Pimp a Butterfly",                           "Kendrick Lamar",    "spotify:album:7ycBtnsMtyVbbwTfJwRjSP" },
    { "Aha Shake Heartbreak",                          "Kings of Leon",     "spotify:album:3cqdpAqjVjv5IB8HTruJkj" },
    { "hugo",                                          "Loyle Carner",      "spotify:album:3McfY0EGNjsrVdYa9ZnoSH" },
    { "Imaginal Disk",                                 "Magdalena Bay",     "spotify:album:2Htq1sHgmdGffojIBM6Q1s" },
    { "The Best of Sade",                              "Sade",              "spotify:album:3uSWaQxJAdm5MWKQkQJNoK" },
    { "Is This It",                                    "The Strokes",       "spotify:album:2yNaksHgeMQM9Quse463b5" },
    { "The New Abnormal",                              "The Strokes",       "spotify:album:2xkZV2Hl1Omi8rk2D7t5lN" },
};

const album_entry_t *albums_get(size_t index)
{
    if (index >= sizeof(s_albums) / sizeof(s_albums[0])) return nullptr;
    return &s_albums[index];
}

size_t albums_count(void)
{
    return sizeof(s_albums) / sizeof(s_albums[0]);
}
