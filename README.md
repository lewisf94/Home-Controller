# Music Controller

Handheld Spotify controller built around the **CYD** (Cheap Yellow Display) ESP32 board. Browse album thumbnails on a 2.8" touchscreen, tap to play, and use a custom hardware panel of two rotary encoders and four push buttons (routed through an MCP23017 I2C expander) to drive playback. Backed today by the Spotify Web API; will migrate to Home Assistant (with the Spotify integration) once the ESP-IDF port is feature-complete.

This repo holds the firmware. Album art conversion tools live alongside in [`scripts/`](scripts/). Album metadata and pre-converted RGB565 thumbnails are stored on an SD card on the device.

---

## At a glance

| Build | Folder | Framework | Status |
|---|---|---|---|
| **CYD Arduino** | [`cyd/platformio/`](cyd/platformio/) | PlatformIO + Arduino | Phase 1 + 1.5 shipped, feature-complete, maintenance only |
| **CYD ESP-IDF** | [`cyd/esp-idf/`](cyd/esp-idf/) | ESP-IDF 6.0 + LVGL 9 | Phase 2 in progress — Steps 0..3 verified on hardware |
| **Waveshare ESP32-P4** | [`waveshare/`](waveshare/) | ESP-IDF 6.0 + LVGL 9 | Planned — board not yet acquired |

The Arduino build is the working firmware today. The ESP-IDF build is being brought up in parallel as the foundation for Phase 3 (Home Assistant integration) and for the eventual move to the Waveshare ESP32-P4 board.

---

## What it does (feature list — Phase 1.5)

Implemented and running on the Arduino build:

- **Album browser**: SD-card-driven grid of 80×80 RGB565 thumbnails, scrolled by RE1, tap any tile to play.
- **Now playing screen**: title / artist / album / progress bar; album art rendered from JPEG (`nowplaying.jpg` fallback on SD) with a vinyl-style square/round toggle.
- **Spotify Web API**: OAuth refresh-token flow, player-state polling every 2 s, play / pause / next / prev / seek / shuffle / volume endpoints.
- **MCP23017 input**: gray-code decoder for RE1, debounced buttons SW1..SW4 with single-press / hold detection.
- **HUD overlays**: volume bar (auto-hides after 2 s), persistent mute badge, play / pause flash, WiFi signal indicator.
- **SW4 manual scrub**: hold SW4 + turn RE1 to seek anywhere in the current track.

Known issues open at end of Phase 1.5 are tracked in [`docs/ROADMAP.md`](docs/ROADMAP.md) Phase 1.

---

## Hardware

The CYD ("Cheap Yellow Display") is an ESP32-WROOM dev board with a 2.8" ILI9341 SPI TFT and an XPT2046 resistive touch controller, both wired to the ESP32 over two separate SPI buses. Around it we add:

- **MCP23017 I2C IO expander** (CJMCU-2317 breakout, address `0x20`) — 4 push-buttons (SW1..SW4) and 1 rotary encoder (RE1 CLK / DT / SW) consolidated on Port A, with INT routed to GPIO 35 for low-latency event delivery.
- **SD card** on GPIO 5 (default VSPI) — holds `metadata.csv`, RGB565 thumbnails, and the JPEG fallback art.
- **External I2C pull-ups** recommended (4.7 kΩ to 3.3 V on SDA/SCL).
- A second rotary encoder + mute switch (RE2) is reserved in software but not currently fitted.

Full pin tables, I2C addresses, and architecture details live in [`CLAUDE.md`](CLAUDE.md). Don't duplicate them here — that file is the source of truth.

---

## Phased roadmap

Three active phases plus a future board port. Detail in [`docs/ROADMAP.md`](docs/ROADMAP.md).

1. **Phase 1 — Bug fixes (Arduino):** small known-cause bugs (JPEG re-decode, encoder responsiveness, Spotify poll cadence). Done concurrently with the IDF port.
2. **Phase 2 — ESP-IDF port (CYD):** rebuild on ESP-IDF 6.x + LVGL 9. Same hardware, same features. Foundation for Phase 3 and for the future P4 board. ← **active**
3. **Phase 3 — Home Assistant integration:** swap the direct Spotify HTTP calls for a WebSocket client talking to Home Assistant on a Pi 5. Fixes the Android/iOS volume limitation, removes the OAuth refresh dance, and gets real-time push state updates.
4. **Future — Waveshare ESP32-P4 port:** 480×800 MIPI-DSI, capacitive touch, ESP-Hosted WiFi. The Phase 3 HA client carries over unchanged.

---

## Repository layout

```
cyd/                       CYD board (ESP32-WROOM, 2.8" ILI9341)
  platformio/              Arduino build via PlatformIO (Phase 1 firmware)
    include/               TFT_eSPI User_Setup, pin defines
    src/                   main.cpp, ui.cpp, input.cpp, spotify.cpp, ...
    platformio.ini         Board / framework / libs
  esp-idf/                 Native ESP-IDF build (Phase 2 active)
    main/                  app source (main.c, CMakeLists, idf_component.yml)
    sdkconfig.defaults     Project Kconfig defaults (target esp32, LVGL 16bpp, etc.)
    CMakeLists.txt         Top-level project file
waveshare/                 Waveshare ESP32-P4 build (planned, empty)
docs/
  ROADMAP.md               Three-phase plan + future P4 plan
  TESTING.md               Hardware test checklist
  PORT-NOTES.md            IDF port gotchas discovered on hardware
scripts/                   Album-art conversion (JPEG -> RGB565 bin)
CLAUDE.md                  Project memory: hardware, architecture, conventions
SPOTIFY_SETUP.md           One-time OAuth setup instructions
```

Each board folder is self-contained — open it directly in your IDE and follow its own README for build steps. `cyd/platformio/` is a PlatformIO project; `cyd/esp-idf/` is an ESP-IDF project. Don't mix them.

---

## Quick start

Pick the build that matches what you want to do. Each one has its own README with full build / flash / monitor instructions:

- Working firmware right now: [`cyd/platformio/`](cyd/platformio/README.md)
- Ongoing port: [`cyd/esp-idf/`](cyd/esp-idf/README.md)
- Future board: [`waveshare/`](waveshare/README.md)

If you want to populate the SD card with your own album library, the conversion pipeline is in [`scripts/`](scripts/) — output a `metadata.csv` plus one 12,800-byte RGB565 `.bin` per album cover (80×80 px each).

OAuth setup for the Spotify Web API is a one-time process documented in [`SPOTIFY_SETUP.md`](SPOTIFY_SETUP.md). The resulting refresh token plus your WiFi credentials go into `cyd/platformio/include/secrets.h` (gitignored) for the Arduino build.

---

## Development

- **Project memory:** [`CLAUDE.md`](CLAUDE.md) is the authoritative reference for hardware pins, I2C addresses, architecture, and coding conventions. Read it before non-trivial changes.
- **Status & plans:** [`docs/ROADMAP.md`](docs/ROADMAP.md) — three phases, what's shipped, what's next, plus open questions.
- **Port gotchas:** [`docs/PORT-NOTES.md`](docs/PORT-NOTES.md) — IDF surprises caught on hardware, with root cause and fix per entry. Saves you re-discovering them.
- **Test plan:** [`docs/TESTING.md`](docs/TESTING.md) — what still needs verifying on the bench.
- **Conventions:** no emojis in code or commit messages; no `// removed`-style stub comments; all UI magic numbers gathered at the top of `ui.cpp` (Arduino) using `NP_*` / `BROWSER_*` naming.

---

## Licence

No licence specified — treat as all rights reserved unless you ask.
