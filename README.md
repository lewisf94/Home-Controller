# Music Controller

Handheld Spotify controller built around the **CYD** (Cheap Yellow Display) ESP32 board. Browse album thumbnails on a 2.8" touchscreen, tap to play, and use a custom hardware panel of two rotary encoders and four push buttons (routed through an MCP23017 I2C expander) to drive playback. The lead firmware is now the ESP-IDF build (feature-complete, verified on hardware); it talks to the Spotify Web API directly, with a Home Assistant backend variant in progress and a port to the larger Waveshare ESP32-P4 board underway.

This repo holds the firmware. Album art conversion tools live alongside in [`scripts/`](scripts/). Album metadata and pre-converted RGB565 thumbnails are stored on an SD card on the device.

---

## At a glance

| Build | Folder | Framework | Status |
|---|---|---|---|
| **CYD ESP-IDF** (direct Spotify) | [`cyd/esp-idf/`](cyd/esp-idf/) | ESP-IDF 6.0 + LVGL 9 | Lead build — feature-complete and hardware-verified originally; recent perf + reliability + UX batches (today's work across 05-24/26/27/28/30 reviews) need a CYD re-flash. See [`docs/PENDING.md`](docs/PENDING.md). |
| **CYD ESP-IDF — HA** | [`cyd/esp-idf-ha/`](cyd/esp-idf-ha/) | ESP-IDF 6.0 + LVGL 9 | Shares UI/input/etc with the direct-Spotify build via [`cyd/components/cyd_shared/`](cyd/components/cyd_shared/) — only the backend is HA-specific (`ha_client.c`). Never hardware-tested. |
| **CYD Arduino** | [`cyd/platformio/`](cyd/platformio/) | PlatformIO + Arduino | Phase 1 + 1.5 shipped; LVGL port committed; perf + reliability fixes from the 05-24 / 05-26 reviews landed (TLS keep-alive, filtered JSON parse, adaptive poll backoff). Needs hardware re-verify. |
| **Waveshare ESP32-P4** (direct Spotify) | [`waveshare/esp-idf/`](waveshare/esp-idf/) | ESP-IDF 5.5 + LVGL 9.4 | Checkpoints 1–3 hardware-verified. UI + Sonos + brightness + reliability/UX batches committed — needs hardware verify (board in hand). |

The CYD ESP-IDF build is the lead firmware — feature-complete and (originally) verified smooth on device. The two CYD IDF builds now share UI/input/MCP/album-art/littlefs via the [`cyd_shared`](cyd/components/cyd_shared/) ESP-IDF component, so a fix to either build benefits both. The Arduino build is in maintenance mode (perf + reliability fixes welcome, no new features). The Waveshare ESP32-P4 build has grown into the most feature-rich variant (Sonos integration, settings UI with brightness/themes/Cover Flow, auto-dim, etc.) — board is in hand, awaiting a verification flash for the recent batches.

---

## What it does

Shared across all builds:

- **Album browser** — fixed list of albums (generated from `spotify-albums-list.txt` via `scripts/gen_albums.py`), thumbnails embedded into the app binary (CYD) or built per-board (waveshare, 220×220), scroll + tap-to-play.
- **Now-playing screen** — title / artist / album / progress bar / album art (Spotify CDN → JPEG decode → RGB565). Local progress bar simulation between polls so the bar moves smoothly.
- **Spotify Web API** — OAuth refresh-token flow, player-state poll with TLS keep-alive + adaptive backoff (5 s playing, 15 s paused), play / pause / next / prev / seek / volume. 404 wake-on-play (idle phone gets transferred back to and started). 401 token refresh on the command path.
- **WiFi resilience** — fast retries plus an `esp_timer` background reconnect (no more "router blip = power-cycle the device").
- **UX honesty** — OFFLINE indicator on the title when WiFi drops; toast on the now-playing screen when a play attempt fails; on-screen warning when the album list exceeds the cap; "no albums configured" message instead of a blank carousel; volume HUD gated until the first poll lands so a first nudge doesn't jump the speaker from a guessed 50 %.
- **Auto-snap browser** — opening the browser lands on whatever's currently playing, with the card border accented.

CYD build (Arduino + IDF):

- **MCP23017 panel** — 4 push buttons + RE1 encoder + RE1 push-switch, via I2C, with INTA on GPIO 35. Encoder uses a gray-code state machine; buttons are 30 ms debounced; `event_pending` is a consume-on-read latch so a press during an LVGL-lock timeout isn't dropped. MCP re-probes itself every 5 s if it was missing at boot, so a transient I2C glitch doesn't disable physical controls permanently.
- **HUD overlays** — volume bar (auto-hide), persistent mute badge, play/pause flash, WiFi signal indicator.

Waveshare ESP32-P4 build:

- **Touch-first UI** — GT911 capacitive touch on an 800×480 IPS panel. On-screen transport keys + volume slider on now-playing; gear button opens a Settings screen.
- **Settings** — Menu Transition (Over / Move / Fade / None), Mode (Dark / Black / Light), Colour accent (Orange / Red / Green / Purple), Browser Style (Carousel / Focus / Cover Flow), Selection Line on/off, Backlight Brightness (10–100 % slider, live-dimmed). All persisted to NVS.
- **Auto-dim / sleep** — backlight ramps to 30 % at 1 min idle, 10 % at 5 min, restores on touch.
- **Sonos** — direct UPnP/SOAP control of a Sonos speaker on the LAN: transport, volume, and full album-start (enqueue cpcontainer + point transport at queue + Play). Device selector merges Spotify Connect transfer targets with configured Sonos speakers; now-playing fallback reads UPnP `GetPositionInfo` when Spotify's `/me/player` can't see the speaker.
- **Three browser styles** — flat Carousel, scale-and-dim Focus, and iPod-style Cover Flow (with image-direct scale to avoid the rotated-DSI layer-snapshot artefact).

For the rolling list of what's been written but not yet hardware-verified, see [`docs/PENDING.md`](docs/PENDING.md).

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
  components/
    cyd_shared/            Shared ESP-IDF component used by both IDF builds:
                           ui.c, input.c, mcp_input.c, album_art.cpp, littlefs.c,
                           + their headers and the backend-neutral player.h struct
  platformio/              Arduino build via PlatformIO (LVGL port, needs re-verify)
    include/               LVGL config, pin defines, gitignored secrets.h
    src/                   main.cpp, ui.cpp, input.cpp, mcp_input.cpp, spotify.cpp, ...
    platformio.ini         Board / framework / libs
  esp-idf/                 Native ESP-IDF build, direct Spotify (lead build)
    main/                  main.c + spotify.c + albums.c + album_thumbs.{c,bin}
    sdkconfig.defaults     Project Kconfig defaults (target esp32, LVGL 16bpp, etc.)
    CMakeLists.txt         Top-level (adds EXTRA_COMPONENT_DIRS for cyd_shared)
  esp-idf-ha/              ESP-IDF build, Home Assistant backend (never flashed)
    main/                  main.c + ha_client.{c,h} + albums.c + album_thumbs.{c,bin}
    CMakeLists.txt         Same EXTRA_COMPONENT_DIRS pattern
waveshare/                 Waveshare ESP32-P4 board
  esp-idf/                 ESP-IDF 5.5 build, direct Spotify + Sonos
    components/            vendored board-support package (BSP)
    main/                  app source; has its own ui.c (laid out for 800x480)
docs/
  ROADMAP.md               Per-phase plan + P4 + HA notes
  TESTING.md               Hardware verification checklist (per build)
  PORT-NOTES.md            IDF port gotchas discovered on hardware
  P4-TODO.md               Waveshare-specific backlog (open items + shipped one-liners)
  PENDING.md               Rolling list of what's committed but not yet flashed
scripts/                   Album-art + symbol generation (gen_albums.py, embed_albums_idf.py, ...)
CLAUDE.md                  Project memory: hardware, architecture, conventions
SPOTIFY_SETUP.md           One-time OAuth setup instructions
spotify-albums-list.txt    Source of truth for the album list (gitignored, personal)
previous-reviews.md        Daily-review log appended by the cloud-review routine
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
