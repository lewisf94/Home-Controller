# Music Controller

Handheld Spotify controller built around the CYD (Cheap Yellow Display — ESP32-WROOM with ILI9341 2.8" touchscreen). Custom hardware panel with two rotary encoders and four push buttons via MCP23017 I2C expander.

## Builds

| Directory | Framework | Status |
|---|---|---|
| `cyd-arduino/` | PlatformIO + Arduino | Phase 1 complete, maintenance only |
| `cyd-idf/` | ESP-IDF 5.x + LVGL | Phase 2 in progress |

## Repository layout

```
cyd-arduino/    Arduino build (PlatformIO)
cyd-idf/        ESP-IDF build (idf.py)
docs/           ROADMAP, TESTING, PORT-NOTES
scripts/        Album art conversion tools (firmware-agnostic)
CLAUDE.md       Full project memory for Claude Code sessions
SPOTIFY_SETUP.md  OAuth token setup instructions
```

## Shared tools

Album art conversion scripts in `scripts/` produce the SD-card payload (`metadata.csv` + `*.bin` thumbnails) used by both firmware builds.

## Project memory

`CLAUDE.md` at the repo root is the authoritative source for hardware pin mapping, architecture decisions, coding conventions, and phase status. Read it before making changes.
