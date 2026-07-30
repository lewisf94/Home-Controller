# CYD Shared Component

The `cyd_shared` ESP-IDF component supplies common code to these builds:

- `cyd/esp-idf/`
- `cyd/esp-idf-ha/`

A shared fix changes both builds. Do not make a second copy of shared source
code in a build folder.

## Contents

| File | Function |
|---|---|
| `ui.c` | LVGL browser, now-playing view, and overlays |
| `input.c` | High-level input dispatcher |
| `mcp_input.c` | MCP23017 driver and debounce logic |
| `album_art.cpp` | JPEG album-art decoder |
| `littlefs.c` | Internal flash storage mount |

Public headers define the shared interfaces. Each build supplies its backend
and generated album data.

## Build-Specific Code

| Function | Location |
|---|---|
| Board initialization and command queue | Each build's `main/main.c` |
| Spotify Web API client | `cyd/esp-idf/main/spotify.c` |
| Home Assistant WebSocket client | `cyd/esp-idf-ha/main/ha_client.c` |
| Generated album list | Each build's `main/albums.c` |
| Embedded album thumbnails | Each build's `main/album_thumbs.c` |
| Credentials | Each build's private credential file |

## CMake Configuration

Each build adds the parent component folder before the `project()` command:

```cmake
cmake_minimum_required(VERSION 3.16)
list(APPEND EXTRA_COMPONENT_DIRS ../components)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(music_controller)
```

Each `main` component lists `cyd_shared` as a requirement:

```cmake
idf_component_register(
    SRCS "main.c" "spotify.c" "album_thumbs.c" "albums.c"
    INCLUDE_DIRS "." "../include"
    EMBED_FILES "album_thumbs.bin"
    REQUIRES cyd_shared
)
```

Do not list the shared source files again in the build-specific source list.
ESP-IDF gets them from this component.

## Backend Contract

The user interface does not call a network backend directly. Each backend
publishes player state with `ui_set_track_info()`.

The user interface sends actions through the `ui_request_*()` functions. Each
build implements these functions in `main.c`.

The request functions put commands in `s_cmd_queue`. A network task removes and
executes the commands.

Keep backend-specific data and functions outside this component.

## Deferred Work

The CYD builds still have separate Wi-Fi state machines and command-queue
scaffolds. Do not extract this code until the pending hardware checks are
complete.
