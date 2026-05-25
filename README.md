# Music Controller

Handheld Spotify controller built around the **CYD** (Cheap Yellow Display) ESP32 board. Browse album thumbnails on a 2.8" touchscreen, tap to play, and use a custom hardware panel of two rotary encoders and four push buttons (routed through an MCP23017 I2C expander) to drive playback. The lead firmware is now the ESP-IDF build (feature-complete, verified on hardware); it talks to the Spotify Web API directly, with a Home Assistant backend variant in progress and a port to the larger Waveshare ESP32-P4 board underway.

This repo holds the firmware. Album art conversion tools live alongside in [`scripts/`](scripts/). Album metadata and pre-converted RGB565 thumbnails are stored on an SD card on the device.

---

## At a glance

| Build | Folder | Framework | Status |
|---|---|---|---|
| **CYD ESP-IDF** (direct Spotify) | [`cyd/esp-idf/`](cyd/esp-idf/) | ESP-IDF 6.0 + LVGL 9 | **Feature-complete, fully verified on hardware** — the lead build |
| **CYD ESP-IDF — HA** | [`cyd/esp-idf-ha/`](cyd/esp-idf-ha/) | ESP-IDF 6.0 + LVGL 9 | Home Assistant backend added; not yet hardware-tested |
| **CYD Arduino** | [`cyd/platformio/`](cyd/platformio/) | PlatformIO + Arduino | Phase 1 + 1.5 shipped; LVGL port committed, not yet re-verified on hardware |
| **Waveshare ESP32-P4** (direct Spotify) | [`waveshare/esp-idf/`](waveshare/esp-idf/) | ESP-IDF 5.5 + LVGL 9.4 | **Checkpoints 1–3 (display/WiFi/Spotify) hardware-verified**; UI (browser, now-playing, settings, Cover Flow, colour themes) committed — needs hardware verify |

The CYD ESP-IDF build is now the lead firmware: feature-complete and verified smooth on device, and the foundation for Phase 3 (Home Assistant integration) and the Waveshare ESP32-P4 port. The Arduino build was the original working product and has since been ported to LVGL to match (that port still needs a hardware pass). The Waveshare ESP32-P4 board has arrived and its own ESP-IDF build is being brought up checkpoint by checkpoint.

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
2. **Phase 2 — ESP-IDF port (CYD):** rebuilt on ESP-IDF 6.x + LVGL 9. Same hardware, same features. ← **done — feature-complete and verified on hardware**
3. **Phase 3 — Home Assistant integration:** swap the direct Spotify HTTP calls for a WebSocket client talking to Home Assistant on a Pi 5. Fixes the Android/iOS volume limitation, removes the OAuth refresh dance, and gets real-time push state updates. A first HA build lives in [`cyd/esp-idf-ha/`](cyd/esp-idf-ha/) (not yet hardware-tested).
4. **Waveshare ESP32-P4 port:** 800×480 MIPI-DSI (ST7701), GT911 capacitive touch, ESP-Hosted WiFi via an onboard ESP32-C6. ← **active** — [`waveshare/esp-idf/`](waveshare/esp-idf/) has checkpoints 1–3 (display / WiFi / Spotify) hardware-verified; the full UI (browser, now-playing, settings, three browser styles, colour accents) is committed and awaiting a hardware verification pass.

---

## Repository layout

```
cyd/                       CYD board (ESP32-WROOM, 2.8" ILI9341)
  platformio/              Arduino build via PlatformIO (LVGL port, needs re-verify)
    include/               LVGL config, pin defines
    src/                   main.cpp, ui.cpp, input.cpp, spotify.cpp, ...
    platformio.ini         Board / framework / libs
  esp-idf/                 Native ESP-IDF build, direct Spotify (lead build, verified)
    main/                  app source (main.c, CMakeLists, idf_component.yml)
    sdkconfig.defaults     Project Kconfig defaults (target esp32, LVGL 16bpp, etc.)
    CMakeLists.txt         Top-level project file
  esp-idf-ha/              ESP-IDF build, Home Assistant backend (Phase 3, untested)
waveshare/                 Waveshare ESP32-P4 board
  esp-idf/                 ESP-IDF 5.5 build, direct Spotify (cp1-3 verified; UI committed, needs verify)
    components/            vendored board-support package (BSP)
    main/                  app source; sources copied from cyd/esp-idf/
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

- Lead build (CYD, direct Spotify, verified): [`cyd/esp-idf/`](cyd/esp-idf/README.md)
- Home Assistant backend variant (CYD, untested): [`cyd/esp-idf-ha/`](cyd/esp-idf-ha/README.md)
- Original Arduino build (LVGL port, needs re-verify): [`cyd/platformio/`](cyd/platformio/README.md)
- Waveshare ESP32-P4 (direct Spotify, cp1-3 verified; UI committed, needs verify): [`waveshare/esp-idf/`](waveshare/esp-idf/README.md)

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
