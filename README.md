# Home Controller

Home Controller is a handheld music controller. It has an album browser,
now-playing information, touch controls, and optional physical controls.

The Waveshare ESP32-P4 is the primary platform. CYD firmware variants remain
available for maintenance and tests.

## Build Matrix

| Build | Folder | Framework | Status |
|---|---|---|---|
| Waveshare Home Assistant | `waveshare/esp-idf-ha/` | ESP-IDF 5.5 and LVGL 9 | Verified baseline; latest theme batch needs a test |
| Waveshare direct Spotify and Sonos | `waveshare/esp-idf/` | ESP-IDF 5.5 and LVGL 9 | Verified baseline; latest theme batch needs a test |
| CYD direct Spotify | `cyd/esp-idf/` | ESP-IDF 6.0 and LVGL 9 | Requires a new hardware check |
| CYD Home Assistant | `cyd/esp-idf-ha/` | ESP-IDF 6.0 and LVGL 9 | Not hardware-tested |
| CYD Arduino | `cyd/platformio/` | PlatformIO and Arduino | Maintenance mode |

The two Waveshare builds use shared interface, audio, album, and font
components. They also use common reliability and update scaffolds.

The two CYD ESP-IDF builds use the `cyd_shared` component. The Arduino build has
its own source files.

## Common Functions

All current builds provide these principal functions:

- Album browser with embedded thumbnails.
- Now-playing view with album art.
- Play, pause, previous, next, seek, volume, and shuffle controls.
- Spotify authorization or Home Assistant authorization.
- Wi-Fi reconnect after a connection failure.
- Offline status.
- Automatic selection of the active album.

The player-state request uses a persistent TLS connection where applicable.
The request interval increases when playback is paused.

## Waveshare Functions

The Waveshare interface has these additional functions:

- Capacitive touch.
- Albums, Now Playing, Queue, Lights, and Settings pages.
- Carousel, Focus, and Cover Flow browser styles.
- BASIC, GLYPH, PIXEL, PAPER, and BOLD themes.
- Dark and light theme variants.
- On-device developer controls for type, color, shape, layout, browser, and art values.
- On-device album search and addition.
- User-interface sounds through the ES8311 speaker.
- Backlight dim and sleep levels.
- Direct Sonos control in the Spotify build.
- Device, queue, light, and Sendspin functions in the Home Assistant build.
- Crash reports, watchdog recovery, and over-the-air updates.

The optional RP2040 haptic-knob controller communicates through UART. Its
firmware and hardware tests are in `rp2040/`.

## Hardware

### Waveshare ESP32-P4

| Item | Configuration |
|---|---|
| Board | ESP32-P4-WIFI6-Touch-LCD-4.3 |
| Microcontroller | ESP32-P4 dual-core RISC-V |
| Flash | 32 MB |
| Display | ST7701 MIPI-DSI, 800 x 480 landscape |
| Touch | GT911 capacitive |
| Network controller | ESP32-C6 through SDIO |
| Toolchain | ESP-IDF 5.5.x |

Do not use ESP-IDF 5.4 or 6.0 for this board. The vendored board-support package
requires ESP-IDF 5.5.x.

### CYD

| Item | Configuration |
|---|---|
| Board | ESP32-2432S028R |
| Microcontroller | ESP32-WROOM |
| Flash | 4 MB |
| Display | ILI9341, 320 x 240 |
| Touch | XPT2046 resistive |
| Toolchain | ESP-IDF 6.0 or PlatformIO |

The external MCP23017 connects four switches and one rotary encoder. Refer to
[CLAUDE.md](CLAUDE.md) for the complete pin assignments.

## Repository Layout

```text
waveshare/
  components/           Shared P4 application components
  esp-idf/              Direct Spotify and Sonos build
  esp-idf-ha/           Home Assistant build

cyd/
  components/           Shared CYD ESP-IDF component
  esp-idf/              Direct Spotify build
  esp-idf-ha/           Home Assistant build
  platformio/           Arduino build

rp2040/                 Haptic-knob firmware and native SDK tests
proto/                  Shared UART protocol
pcb/                    KiCad projects
docs/                   Plans, tests, and technical notes
scripts/                Album metadata and image tools
tools/                  Browser interface design tools
```

## Waveshare Quick Start

> **CAUTION:** Use ESP-IDF 5.5.x. A different version can make the build fail.

1. Open an ESP-IDF 5.5 terminal.
2. Change to the applicable Waveshare build folder.
3. Create the private credential file from its example.
4. Add the required credentials.
5. Set the target:

   ```powershell
   idf.py set-target esp32p4
   ```

6. Build, flash, and monitor:

   ```powershell
   idf.py build flash monitor
   ```

Refer to the build README for its one-time setup and verification state.

## CYD ESP-IDF Quick Start

1. Open an ESP-IDF 6.0 terminal.
2. Change to `cyd/esp-idf/`.
3. Create the private credential file from its example.
4. Set the target:

   ```powershell
   idf.py set-target esp32
   ```

5. Build, flash, and monitor:

   ```powershell
   idf.py build flash monitor
   ```

## CYD Arduino Quick Start

1. Change to `cyd/platformio/`.
2. Upload the firmware:

   ```powershell
   pio run -t upload
   ```

3. Open the serial monitor:

   ```powershell
   pio device monitor -b 115200
   ```

## Album Library

`spotify-albums-list.txt` is the source for the compiled album list. The file is
not tracked because it contains the personal album selection.

To add an album, run:

```powershell
python scripts/add_albums.py spotify:album:<ID>
```

The tool gets metadata, gets cover art, regenerates album source files, and
rebuilds the thumbnail data.

Do not edit generated album source files manually.

## Project State

1. The CYD Arduino fixes are complete.
2. The CYD ESP-IDF port is complete.
3. The Waveshare Home Assistant baseline is hardware-verified.
4. The Waveshare direct baseline is hardware-verified.
5. The RP2040 sensor tests are hardware-verified.
6. The RP2040 motor test is build-verified.
7. The BOLD theme and Developer controls need a build and hardware test.
8. The custom PCB remains in development.

Refer to [ROADMAP.md](docs/ROADMAP.md) for planned work.
Refer to [PENDING.md](docs/PENDING.md) for verification debt.
Refer to [TESTING.md](docs/TESTING.md) for hardware checks.

## Development Rules

- Read [CLAUDE.md](CLAUDE.md) before a non-trivial change.
- Follow [ASD-STE100 Simplified Technical English](docs/WRITING-STANDARD.md).
- Keep credentials out of tracked files.
- Do not edit generated album files manually.
- Do not add emojis to code or commit messages.
- Do not use dead-code comments.
- Keep blocking network calls outside interface and input tasks.

## License

This project uses the Apache License 2.0. Refer to [LICENSE](LICENSE).

Refer to [NOTICE](NOTICE) for third-party attribution.
