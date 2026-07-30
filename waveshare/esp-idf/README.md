# Waveshare ESP32-P4 Direct Build

This firmware runs on the Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3. It connects
directly to Spotify and controls Sonos speakers.

The established build is hardware-verified. The latest theme-development batch
still needs a build and hardware test.

The Home Assistant build uses the same shared interface and audio components.

## Hardware

| Item | Configuration |
|---|---|
| Microcontroller | ESP32-P4 dual-core RISC-V |
| Display | ST7701 MIPI-DSI |
| Display orientation | 800 x 480 landscape |
| Touch | GT911 |
| Network controller | ESP32-C6 through SDIO |
| Flash | 32 MB |
| Audio | ES8311 |
| Toolchain | ESP-IDF 5.5.x |

> **CAUTION:** Use ESP-IDF 5.5.x. ESP-IDF 5.4 is too old, and ESP-IDF 6.0
> removed a required component.

The build uses `esp_lvgl_adapter`. Use `bsp_display_lock()` and
`bsp_display_unlock()` for LVGL access.

## Verified Functions

- Display and touch.
- Wi-Fi through the ESP32-C6.
- Spotify token refresh and player-state requests.
- Album browser and now-playing view.
- Carousel, Focus, and Cover Flow.
- BASIC, GLYPH, PIXEL, PAPER, and BOLD themes.
- Dark and light theme variants.
- User-interface sounds.
- Settings and credential overrides.
- Backlight dim and sleep levels.
- Direct Sonos transport and volume.
- Sonos album start.
- Spotify Connect and Sonos device selection.
- Wi-Fi reconnect.
- Offline status and failure messages.
- Automatic selection of the active album.

## Unverified Theme Batch

The BOLD theme uses compiled Jost Bold fonts.

The DEVELOPER tab has 65 live controls in six pages.

The pages control type, color, shape, layout, browser, and art values.

The browser design bench is `tools/theme-bench.html`.

Complete a clean build and an on-device visual test before release.

## Shared Components

`waveshare/components/p4_shared/` contains the shared interface, audio, album,
font, credential, and knob code.

`waveshare/components/app_core/` contains Wi-Fi, art-buffer, reliability, and
update code.

Keep a change in the shared component when both Waveshare backends require it.

## First Setup

1. Install ESP-IDF 5.5.x.
2. Open an ESP-IDF 5.5 terminal.
3. Change to this folder.
4. Create the private credential file from its example.
5. Add Wi-Fi and Spotify credentials.
6. Set the target:

   ```powershell
   idf.py set-target esp32p4
   ```

7. Build:

   ```powershell
   idf.py build
   ```

8. Connect the board.
9. Identify its serial port.
10. Flash and monitor:

   ```powershell
   idf.py -p COM3 flash monitor
   ```

Replace `COM3` with the correct port.

## Board-Support Package

The Waveshare board-support package is vendored in this project. Do not replace
it with a registry component of a different board.

The original Waveshare examples are:

- `08_lvgl_demo_v9` for display and touch.
- `04_wifistation` for the ESP32-C6 network path.

Use the official examples to isolate a board or toolchain problem.

## Memory Rules

The ESP32-P4 has a limited internal DMA-capable memory pool. Large buffers must
use PSRAM where their access requirements permit it.

Keep network response buffers and image pools out of internal memory. Keep
small DMA objects in internal memory.

Run the reliability check before and after a build:

```powershell
python scripts/check_p4_reliability.py both
python scripts/check_p4_reliability.py both --post-build
```

## Cover Flow

Cover Flow uses a PSRAM column renderer. It produces one final image for LVGL.

The center cover is flat, largest, and above the side covers.

Each side cover faces the center:

- A left cover has its left edge nearest the viewer.
- A right cover has its right edge nearest the viewer.
- The inner edge is shorter and behind the more central cover.

Draw the center cover first in the visual stack. Draw each outer cover below
the cover that is nearer the center.

`CF_MAX_SIDE` limits work during each scroll event. Do not remove this limit
without a measured performance test.

Use these constants for geometry adjustments:

- `CF_FAN_SPREAD`
- `CF_FAN_RATE`
- `CF_WIDTH_SHRINK`
- `CF_HEIGHT_SHRINK`
- `CF_MAX_SIDE`
- `CF_CARD_SCALE`

## LVGL Restrictions

Do not apply an object-level scale or opacity transform to an album card. The
display path can create an incorrect intermediate layer.

Do not enable `LV_USE_MATRIX`. The matrix path caused invalid coordinates.

Do not enable runtime Tiny TTF. Use the compiled fonts.

Keep `CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT=1`. A second software draw unit conflicts
with the PPA transaction queue.

PPA display acceleration is already enabled.

## Spotify and Sonos

The Spotify player-state path uses a persistent HTTPS client. The command path
also reuses its client.

The request interval is 5 seconds during playback and 15 seconds while paused.
The code observes Spotify rate-limit delays.

Sonos control uses UPnP and SOAP on the local network. Album start sets the Sonos
queue URI before it sends Play.

## Checkpoint History

| Checkpoint | Function | Status |
|---|---|---|
| 1 | Display | Verified |
| 2 | Wi-Fi | Verified |
| 3 | Spotify | Verified |
| 4 | Interface | Verified |
| 5 | Assets and fonts | Verified |
| 6 | Touch controls | Verified |
| 7 | Interface parity | Verified |
| 8 | Sonos | Verified |
| 9 | Reliability and interface messages | Verified |
| 10 | PIXEL theme | Verified |
| 11 | Code-quality batch | Build-verified |
| 12 | GLYPH theme | Verified |
| 13 | Audio and Settings tabs | Verified |
| 14 | PAPER theme | Verified |
| 15 | Theme structure and tuning controls | Verified |
| 16 | BOLD theme and Developer controls | Not build-verified |

## Optional RP2040 Knob

The build can use the RP2040 haptic-knob controller. The option is disabled by
default.

Enable it with:

```powershell
idf.py build -DKNOB_ENABLED=1
```

Disable it again with:

```powershell
idf.py build -DKNOB_ENABLED=0
```

The CMake value remains in the build cache. Omitting the option does not change
the stored value.

The ESP32-P4 uses GPIO32 for TX and GPIO46 for RX. Refer to
[CLAUDE.md](../../CLAUDE.md) for the complete protocol and pin assignments.

Use [rp2040/bringup/README.md](../../rp2040/bringup/README.md) for the safe
hardware-test sequence.

## Related Documents

- [Project memory](../../CLAUDE.md)
- [P4 backlog](../../docs/P4-TODO.md)
- [Pending verification](../../docs/PENDING.md)
- [Hardware tests](../../docs/TESTING.md)
- [Port notes](../../docs/PORT-NOTES.md)
- [Theme bench](../../tools/README.md)
