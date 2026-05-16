# Music Controller

Handheld Spotify controller — ESP32-based, with a touchscreen album browser and a custom hardware panel (two rotary encoders + four push buttons via MCP23017 I2C expander). Drives playback through the Spotify Web API today, with planned migration to Home Assistant.

## Pick a build

| Board | Folder | Framework | Status |
|---|---|---|---|
| **CYD** (ESP32-WROOM, 2.8" ILI9341) | [`cyd/`](cyd/) | — | active |
| ↳ Arduino build | [`cyd/platformio/`](cyd/platformio/) | PlatformIO + Arduino | Phase 1 complete, maintenance only |
| ↳ ESP-IDF build | [`cyd/esp-idf/`](cyd/esp-idf/) | ESP-IDF 5.x + LVGL | Phase 2 in progress |
| **Waveshare ESP32-P4** (4.3" MIPI-DSI) | [`waveshare/`](waveshare/) | ESP-IDF 5.x + LVGL | planned, board not yet arrived |

Each board folder is self-contained — open it directly in your IDE of choice. Build instructions live in each folder's own README.

## Repository layout

```
cyd/                      CYD board (ESP32-WROOM, 2.8" ILI9341)
  platformio/             Arduino build via PlatformIO
  esp-idf/                Native ESP-IDF build
waveshare/                Waveshare ESP32-P4 build (planned)
docs/                     Cross-project docs (ROADMAP, TESTING, PORT-NOTES)
scripts/                  Album-art conversion tools (firmware-agnostic)
CLAUDE.md                 Full project memory for Claude Code sessions
SPOTIFY_SETUP.md          OAuth token setup instructions
```

## Shared tools

[`scripts/`](scripts/) produces the SD-card payload (`metadata.csv` + RGB565 thumbnails) used by both CYD firmware builds. Album art is gitignored — sources stay local.

## Project memory

[`CLAUDE.md`](CLAUDE.md) is the authoritative reference for hardware pin mapping, architecture decisions, coding conventions, and phase status. Read it before making non-trivial changes.

The phased roadmap (Phase 1 bug fixes → Phase 2 ESP-IDF port → Phase 3 Home Assistant integration → future Waveshare port) is in [`docs/ROADMAP.md`](docs/ROADMAP.md).
