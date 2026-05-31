# cyd_shared — shared CYD-IDF component

A single ESP-IDF component holding the source files both `cyd/esp-idf/` (direct
Spotify) and `cyd/esp-idf-ha/` (Home Assistant backend) link against. Extracted
from the two builds' `main/` folders in commits `2f7accd` + `1731a6a` so a fix
to `ui.c` (or `input.c`, `mcp_input.c`, `album_art.cpp`, `littlefs.c`) lands
once in both builds instead of needing hand-syncing.

## Layout

```
cyd/components/cyd_shared/
  CMakeLists.txt        Component manifest (REQUIRES + SRCS)
  ui.c                  LVGL UI (browser, now-playing, HUDs)
  input.c               High-level input dispatcher (buttons, encoder -> commands)
  mcp_input.c           Low-level MCP23017 I2C driver
  album_art.cpp         JPEGDEC decode of now-playing art -> RGB565
  littlefs.c            Internal-flash storage mount for album art
  include/
    ui.h                Public UI API (the build's main.c implements ui_request_*)
    input.h
    mcp_input.h
    album_art.h
    littlefs.h
    albums.h            Per-build albums.c (generated) implements this
    album_thumbs.h      Per-build album_thumbs.c (EMBED_FILES) implements this
    player.h            Backend-neutral track-info struct (spotify_track_t)
```

## What's per-build vs shared

| Concern | Where it lives |
|---|---|
| LCD/touch bring-up, NVS, WiFi event handler, command queue, `ui_request_*` posters, `spotify_task` / `ha_task` | per-build `main/main.c` |
| Spotify Web API client | `cyd/esp-idf/main/spotify.c` |
| Home Assistant WebSocket client | `cyd/esp-idf-ha/main/ha_client.c` |
| `albums.c` (generated from `spotify-albums-list.txt`), `album_thumbs.c` + `album_thumbs.bin` (per-build `EMBED_FILES`) | per-build `main/` |
| `secrets.h` (gitignored real creds), `spotify.h` (thin wrapper that includes `player.h`) | per-build `main/` |
| UI / input dispatcher / MCP driver / album-art decode / LittleFS mount | **`cyd/components/cyd_shared/`** ← here |

## How each build picks it up

Each build's top-level `CMakeLists.txt` adds the parent `components/` directory
to the IDF component search path **before** the `project()` call:

```cmake
cmake_minimum_required(VERSION 3.16)
list(APPEND EXTRA_COMPONENT_DIRS ../components)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(music_controller)
```

Each build's `main/CMakeLists.txt` then lists `cyd_shared` in `REQUIRES`:

```cmake
idf_component_register(
    SRCS "main.c" "spotify.c" "album_thumbs.c" "albums.c"
    INCLUDE_DIRS "." "../include"
    EMBED_FILES "album_thumbs.bin"
    REQUIRES cyd_shared
    PRIV_REQUIRES esp_http_client esp-tls mbedtls esp_timer nvs_flash ...
)
```

`main` no longer lists `ui.c` / `input.c` / `mcp_input.c` / `album_art.cpp` /
`littlefs.c` in `SRCS` — those come in via the shared component.

## Backend contract (the seam)

The UI doesn't know which backend it's running against. Each build's backend:

- **publishes** state via `ui_set_track_info(const spotify_track_t *)` — the
  struct's type lives in shared `player.h` for a single source of truth;
- **subscribes** to user actions via the `ui_request_*` callbacks the UI calls
  (implemented in each build's `main.c`, post commands onto `s_cmd_queue`).

That's all. Anything backend-specific stays on the backend side.

## What's NOT in here

The per-build `main.c` still hand-copies the WiFi state machine + the command
queue scaffold + the `ui_request_*` posters. Pulling those out into a second
shared component (e.g. `app_core`) is a deferred architecture follow-up — the
plan is captured in the project memory (see `project_arch-followups-shared-scaffold`).
Don't start it until the existing CYD verifications clear.
