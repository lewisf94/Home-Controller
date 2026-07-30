# Home Controller

Handheld music controller with an album browser, now-playing screen, and hardware controls. The primary build runs on a **Waveshare ESP32-P4** (4.3" 800×480 IPS, capacitive touch, Sonos + Spotify, settings UI). A smaller **CYD** (ESP32, 2.8" resistive touch) board has three firmware variants and a custom PCB daughterboard in progress. All builds talk to the Spotify Web API directly or via Home Assistant.

> **New to the project (or to embedded C)?** Start with [`docs/CODE-TOUR.md`](docs/CODE-TOUR.md) — a plain-language walkthrough of how the firmware works and where to start reading.

---

## Builds at a glance

| Build | Folder | Framework | Status |
|---|---|---|---|
| **Waveshare ESP32-P4 — Home Assistant** | [`waveshare/esp-idf-ha/`](waveshare/esp-idf-ha/) | ESP-IDF 5.5 + LVGL 9 | **Daily-driver flash — hardware-verified (2026-07-13).** HA WebSocket backend + Music Assistant: devices, lights (colour/presets), queue, on-device album add/search, native Sendspin playback through the board's own speaker, OTA updates, crash reporting. |
| **Waveshare ESP32-P4** (direct Spotify + Sonos) | [`waveshare/esp-idf/`](waveshare/esp-idf/) | ESP-IDF 5.5 + LVGL 9 | **Hardware-verified end to end.** Same shared UI (Cover Flow, four dark/light themes, 24-swatch accents, Settings, sounds); talks straight to the Spotify Web API, drives Sonos over UPnP. |
| **CYD ESP-IDF** (direct Spotify) | [`cyd/esp-idf/`](cyd/esp-idf/) | ESP-IDF 6.0 + LVGL 9 | Feature-complete + originally hardware-verified. Recent perf/reliability/UX batches committed, needs a CYD re-flash. |
| **CYD ESP-IDF — Home Assistant** | [`cyd/esp-idf-ha/`](cyd/esp-idf-ha/) | ESP-IDF 6.0 + LVGL 9 | Shares UI/input/etc with the direct-Spotify CYD build. Backend replaced by a WebSocket HA client. Never hardware-tested. |
| **CYD Arduino** | [`cyd/platformio/`](cyd/platformio/) | PlatformIO + Arduino | LVGL port committed; maintenance mode. Needs hardware re-verify. |

The two Waveshare builds share all UI/audio/album/font code via
[`waveshare/components/p4_shared/`](waveshare/components/) plus app scaffolding
(WiFi reconnect, art buffers, crash reporting, OTA) in `app_core`.

The two CYD IDF builds share UI/input/MCP/album-art/LittleFS code via the [`cyd/components/cyd_shared/`](cyd/components/cyd_shared/) ESP-IDF component. The Waveshare build has its own `ui.c` laid out for 800×480 and is not part of that shared component.

---

## What it does

**All builds:**

- **Album browser** — fixed list of albums (generated from `spotify-albums-list.txt` via `scripts/gen_albums.py`), thumbnails embedded into the app binary, scroll + tap-to-play.
- **Now-playing screen** — title / artist / album / progress bar / album art (JPEG → RGB565). Local progress bar simulation between polls so the bar moves smoothly.
- **Spotify Web API** — OAuth refresh-token flow, player-state poll with TLS keep-alive + adaptive backoff (5 s playing, 15 s paused), play / pause / next / prev / seek / volume / shuffle. 404 wake-on-play, 401 token-refresh on the command path.
- **WiFi resilience** — fast retries plus a background reconnect timer; OFFLINE indicator on the title when the connection drops.
- **UX honesty** — toast on play failure, on-screen warning when album list exceeds cap, "nothing playing" initial state, volume HUD gated until first poll.
- **Auto-snap browser** — opening the browser lands on the currently playing album with an accent border.

**Waveshare ESP32-P4 (lead builds) — additional features:**

- **Touch-first UI** — GT911 capacitive touch; a vertical swipe stack of pages (**Albums / Now Playing / Queue / Lights (HA) / Settings**) with a right-edge rail indicator. On-screen transport, a volume fader with +/- step keys and a permanent volume readout. Long titles scroll horizontally (radio-style marquee); short ones stay centred.
- **Three browser styles** — Carousel, Focus, iPod-style Cover Flow (true 3D perspective via PSRAM column rasteriser — trapezoid foreshortening, correct z-order, no LVGL transform paths). A centre-tap plays the centred album; an off-centre tap scrolls that cover in.
- **Settings screen** — organised into **DISPLAY**, **SOUND** and **SETUP** tabs. DISPLAY: Appearance (Dark/Light), Mode (BASIC/GLYPH/PIXEL/PAPER), Theme Album Art (on/off), Colour accent (8-hue × 3-variant 24-swatch grid), Browser Style, Font, Selection Line, Brightness, FPS, Menu Transition. SOUND: sound on/off, volume, sound set. SETUP: on-device WiFi/Spotify credential entry (NVS overrides the flashed secrets), firmware version + **UPDATE FIRMWARE** (OTA over WiFi from a URL). All NVS-persisted.
- **Add albums from the device** — search Spotify as you type (or browse the saved library / HA media trees), tap to add; runtime albums persist in NVS, fetch their real cover art, and sort alphabetically into the browser. No laptop or reflash needed.
- **Reliability** — crash coredump-to-flash with a decoded report on next boot, task-watchdog auto-reset, WebSocket heartbeat + auto-reconnect (HA), low-memory early warnings, dual-slot OTA partitions.
- **Theme modes** — four design languages, each with a **dark and light face**: **BASIC** (clean charcoal / light), **GLYPH** (Nothing-OS-style — dot-matrix headings over clean type, hairline-outlined pills, ink instrument chrome), **PIXEL** (retro CRT — Press Start 2P pixel font, Bayer-dithered art), and **PAPER** (teletype / data-brutalist — cream + ink, mono fonts, 1-bit dithered art, printed-form frames, typewriter sounds). An 8-hue × 3-variant accent grid drives selection highlights and the progress bar.
- **UI sound effects** — synthesised tones through the onboard ES8311 speaker (scroll / select / back / connect), selectable sound sets, adjustable volume. All off the render path on a dedicated audio task.
- **Auto-dim / sleep** — backlight ramps to 30 % at 1 min idle, 10 % at 5 min, restores on touch.
- **Sonos** — direct UPnP/SOAP control of a Sonos speaker on the LAN: transport, volume, and full album-start. Device selector merges Spotify Connect targets with configured Sonos speakers.
- **Persistent TLS** — poll and command paths each reuse a keep-alive HTTP client to avoid per-call TLS handshakes.
- **Haptic knob (firmware compiles, hardware pending)** — driver for a custom RP2040 SmartKnob-style daughterboard (FOC gimbal motor, strain-gauge press, 4 MX buttons, LED ring, ambient + battery sensors) over UART. Context-aware detent profiles per menu (albums / volume / scrub). RP2040 firmware builds green (`pio run`, flashable uf2); enable the P4 side with `idf.py build -DKNOB_ENABLED=1` (default off, knob-less builds unaffected). Wiring + bring-up order in [`docs/KNOB-NOTES.md`](docs/KNOB-NOTES.md).

**CYD builds — additional features:**

- **MCP23017 panel** — 4 push-buttons (SW1–SW4) + RE1 rotary encoder + push-switch via I2C, INTA on GPIO 35. Gray-code state machine, 30 ms debounce, consume-on-read event latches, re-probe every 5 s if missing at boot.
- **Home Assistant variant** — WebSocket client to a Music Assistant `media_player` entity. Real-time push state (no polling), HA-proxied album art, works with any MA music source. See [`docs/HA-SETUP.md`](docs/HA-SETUP.md) for Pi 5 setup.

**Waveshare HA build — additional features:**

- **Devices** — output picker split into SPEAKERS and SPOTIFY CONNECT sections (Music Assistant players + Spotify Connect sources); transfer playback to a phone, Echo, or the controller's own speaker.
- **Sendspin native playback** — the controller registers itself with Music Assistant as a network player and decodes FLAC/Opus/PCM through its ES8311 speaker.
- **Lights** — full HA `light.*` control: toggle, brightness/colour scrubbing, presets, swatches, colour temperature.
- **Queue** — upcoming-tracks list with add-album / search-songs / clear actions.

---

## Hardware

### Waveshare ESP32-P4 (primary board)

**Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3**

- **MCU:** ESP32-P4 dual-core RISC-V @ 400 MHz, 32 MB flash, PSRAM
- **Display:** 4.3" IPS, 480×800 native (rotated to 800×480 landscape), ST7701 MIPI-DSI
- **Touch:** GT911 capacitive
- **WiFi/BLE:** onboard ESP32-C6 over SDIO (`esp_wifi_remote` + `esp_hosted`)
- **Toolchain:** ESP-IDF 5.5.x (NOT 5.4 / NOT 6.0 — BSP constraint)

### CYD (secondary board)

**ESP32-2432S028R** ("Cheap Yellow Display")

- **MCU:** ESP32-WROOM dual-core Xtensa @ 240 MHz, 4 MB flash, no PSRAM
- **Display:** 2.8" ILI9341 SPI, 320×240
- **Touch:** XPT2046 resistive, separate SPI bus
- **Toolchain:** ESP-IDF 6.0 (IDF builds) / PlatformIO Arduino (platformio build)

**Custom daughterboard (PCB in progress — [`pcb/`](pcb/)):**

- MCP23017 IO expander (CJMCU-2317, address `0x20`, INTA on GPIO 35)
- 4 push-buttons (SW1–SW4) + RE1 rotary encoder + push-switch on Port A
- I2C: SDA = GPIO 27, SCL = GPIO 22, external 4.7 kΩ pull-ups to 3.3 V

Full pin tables, I2C addresses, and architecture details: [`CLAUDE.md`](CLAUDE.md).

---

## Repository layout

```
waveshare/
  components/
    p4_shared/           Shared P4 component: ui.c, audio.c, albums.c, creds,
                         runtime album catalogue, fonts, knob protocol + headers
    app_core/            Shared scaffolding: WiFi reconnect, art buffers,
                         crash-report/heap reliability, OTA updater
    sendspin_player/     Music Assistant network-player (HA build only)
  esp-idf/               ESP32-P4, direct Spotify + Sonos
    components/          Vendored Waveshare BSP (+ nanopb, shared to HA build)
    main/                main.c, spotify.c, sonos.c (UI/albums come from p4_shared)
    include/             secrets.h.example
  esp-idf-ha/            ESP32-P4 Home Assistant build (main.c, ha_client.c) —
    include/             the daily-driver flash; secrets.h.example

cyd/
  components/
    cyd_shared/          Shared ESP-IDF component: ui.c, input.c, mcp_input.c,
                         album_art.cpp, littlefs.c, player.h + headers
  esp-idf/               CYD direct-Spotify build (ESP-IDF 6.0)
  esp-idf-ha/            CYD Home Assistant build (ESP-IDF 6.0)
  platformio/            CYD Arduino build (PlatformIO)

rp2040/                  Firmware for the RP2040 haptic-knob co-MCU (PlatformIO)
  src/                   motor_task (FOC), interface_task (UART/sensors/LEDs)
  lib/nanopb/            Vendored nanopb runtime

proto/                   Shared UART protocol schema (home_controller.proto)
                         + pre-generated nanopb .pb.h/.pb.c

pcb/
  home-controller-daughterboard/   KiCad project — MCP23017 + encoder PCB

docs/
  ROADMAP.md             Per-phase plan + P4 + HA notes
  KNOB-NOTES.md          RP2040 haptic-knob hardware + protocol + bring-up guide
  HA-SETUP.md            Full Pi 5 / Home Assistant setup guide
  P4-RELIABILITY.md      Memory budgets, reliability gates, soak acceptance test
  QUEUE-DESIGN.md        Queue screen design notes
  TESTING.md             Hardware verification checklist (per build)
  PORT-NOTES.md          IDF port gotchas discovered on hardware
  P4-TODO.md             Waveshare-specific backlog
  PENDING.md             Rolling list of what's committed but not yet flashed
  DESIGN_NOTES.md        Knob daughterboard hardware design decisions
  Datasheets/            Component datasheets
  Schematics/            Board schematics
  Symbols & Footprints/  KiCad symbols/footprints for custom parts

scripts/                 Album-art + metadata pipeline (gen_albums.py, add_albums.py, ...)
CLAUDE.md                Project memory: hardware, architecture, conventions
previous-reviews.md      Daily code-review log
```

---

## Quick start

### Waveshare ESP32-P4 (lead build)

```bash
cd waveshare/esp-idf
cp include/secrets.h.example include/secrets.h   # fill in WiFi + Spotify creds
idf.py set-target esp32p4                         # first time — installs RISC-V toolchain
idf.py build flash monitor
```

Requires **ESP-IDF 5.5.x**. See [`waveshare/esp-idf/README.md`](waveshare/esp-idf/README.md) for the one-time BSP vendor step and full checkpoint status.

### CYD — direct Spotify (ESP-IDF)

```bash
cd cyd/esp-idf
cp include/secrets.h.example include/secrets.h
idf.py set-target esp32
idf.py build flash monitor
```

Requires **ESP-IDF 6.0**. See [`cyd/esp-idf/README.md`](cyd/esp-idf/README.md).

### CYD — Home Assistant backend

```bash
cd cyd/esp-idf-ha
cp include/secrets.h.example include/secrets.h   # fill in HA_HOST/TOKEN/ENTITY
idf.py set-target esp32
idf.py build flash monitor
```

See [`docs/HA-SETUP.md`](docs/HA-SETUP.md) for the full Pi 5 + Music Assistant setup guide.

### CYD — Arduino (PlatformIO)

```bash
cd cyd/platformio
pio run -t upload
pio device monitor -b 115200
```

---

## Album library

Albums are compiled into the binary at build time. To add or change albums:

```bash
# From repo root — resolves metadata + downloads cover art via Spotify API
python scripts/add_albums.py spotify:album:<ID>

# Then rebuild whichever build(s) you want updated
cd waveshare/esp-idf && idf.py build flash
```

The source of truth is `spotify-albums-list.txt` (repo root, gitignored). `gen_albums.py` regenerates `albums.c` for all builds. `embed_albums_idf.py` rebakes the thumbnail blob. See [`CLAUDE.md`](CLAUDE.md) (Coding conventions) for the full pipeline.

---

## Roadmap

Detail in [`docs/ROADMAP.md`](docs/ROADMAP.md).

1. **Phase 1 — CYD Arduino bug fixes:** JPEG re-decode, encoder responsiveness, poll cadence. Done.
2. **Phase 2 — CYD ESP-IDF port:** ESP-IDF 6.0 + LVGL 9. Feature-complete, hardware-verified.
3. **Phase 3 — Home Assistant integration:** WebSocket client to HA on a Pi 5. Done on the Waveshare P4 (`waveshare/esp-idf-ha/` — the daily-driver flash, hardware-verified 2026-07-13); the CYD HA build exists (`cyd/esp-idf-ha/`) but is untested.
4. **Waveshare ESP32-P4:** Both builds hardware-verified. Current focus: long-soak reliability (crash reporting, watchdog, heartbeat, memory budgets), OTA updates, on-device album management.
5. **RP2040 haptic knob:** SmartKnob-style daughterboard for the P4. Firmware compiles (`rp2040/`, flashable uf2) + P4-side driver gated behind `KNOB_ENABLED` — both builds green at `=1`; awaiting the PCB + motor/driver parts. See [`docs/KNOB-NOTES.md`](docs/KNOB-NOTES.md) and [`docs/DESIGN_NOTES.md`](docs/DESIGN_NOTES.md).
6. **Custom PCB:** MCP23017 daughterboard KiCad project in [`pcb/`](pcb/).

---

## Development

- **Project memory:** [`CLAUDE.md`](CLAUDE.md) — hardware pins, architecture, coding conventions. Read before non-trivial changes.
- **Writing standard:** All new or changed technical text must follow [ASD-STE100 Simplified Technical English](docs/WRITING-STANDARD.md), Issue 9.
- **What's pending verify:** [`docs/PENDING.md`](docs/PENDING.md).
- **Port gotchas:** [`docs/PORT-NOTES.md`](docs/PORT-NOTES.md).
- **Test plan:** [`docs/TESTING.md`](docs/TESTING.md).
- **Conventions:** no emojis in code or commits; no `// removed`-style stub comments; magic-number layout values gathered at the top of `ui.c` using `NP_*` / `BROWSER_*` naming.

---

## Licence

Licensed under the **Apache License, Version 2.0** — see [`LICENSE`](LICENSE).

Portions are derived from third-party Apache 2.0 / MIT / zlib software (SmartKnob,
SimpleFOC + its drivers, nanopb); attributions and the files affected are listed
in [`NOTICE`](NOTICE).
