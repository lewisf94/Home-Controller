# CYD Firmware

This folder contains firmware for the Cheap Yellow Display (CYD). The common
board name is `ESP32-2432S028R`.

The Waveshare ESP32-P4 is the primary development board. The CYD builds remain
available for maintenance and tests.

## Builds

| Folder | Backend | Framework | Status |
|---|---|---|---|
| `esp-idf/` | Spotify | ESP-IDF 6.0 | Feature-complete; requires a new hardware check |
| `esp-idf-ha/` | Home Assistant | ESP-IDF 6.0 | Not hardware-tested |
| `platformio/` | Spotify | Arduino and PlatformIO | Maintenance mode; requires a new hardware check |
| `components/cyd_shared/` | Not applicable | ESP-IDF component | Shared code for both ESP-IDF builds |

The two ESP-IDF builds use the same user interface and input code. The
`cyd_shared` component contains this shared code.

The Home Assistant build replaces the Spotify client with a WebSocket client.
Refer to [HA-SETUP.md](../docs/HA-SETUP.md) for the server procedure.

## Hardware

| Item | Configuration |
|---|---|
| Microcontroller | ESP32-WROOM, dual-core Xtensa, 240 MHz |
| Flash | 4 MB on the verified board |
| PSRAM | Not enabled |
| Display | ILI9341, 2.8 inch, 320 x 240 |
| Touch controller | XPT2046 |
| Backlight | GPIO 21 |
| SD card chip select | GPIO 5 |
| Serial speed | 115200 baud |

The external CJMCU-2317 board contains an MCP23017 input expander. It connects
four buttons and one rotary encoder to the ESP32.

The MCP23017 uses GPIO 27 for SDA and GPIO 22 for SCL. Its address is `0x20`.
Its INTA output connects to GPIO 35.

Fit 4.7 kOhm pull-up resistors from SDA and SCL to 3.3 V. The internal pull-up
resistors can be too weak for reliable operation.

## MCP23017 Inputs

All fitted inputs use MCP23017 Port A:

| Input | MCP23017 pin |
|---|---|
| SW1 | GPA0 |
| SW2 | GPA1 |
| SW3 | GPA2 |
| SW4 | GPA3 |
| Encoder CLK | GPA4 |
| Encoder DT | GPA5 |
| Encoder switch | GPA6 |

INTA is active LOW. An input change causes an interrupt. A periodic poll keeps
the debounce timers active.

## Select a Build

Use `esp-idf/` for the native direct-Spotify firmware. It has the complete CYD
user interface and physical controls.

Use `esp-idf-ha/` to test the Home Assistant backend. This build requires a
Home Assistant server and a Music Assistant player.

Use `platformio/` only for Arduino maintenance work. Its LVGL port requires a
new hardware verification.

Flashing one build replaces the installed application. Flash another build to
change the active firmware.

## Project Information

Use [CLAUDE.md](../CLAUDE.md) for pin assignments and architecture information.
Use [ROADMAP.md](../docs/ROADMAP.md) for planned work.
Use [PORT-NOTES.md](../docs/PORT-NOTES.md) for hardware and toolchain problems.
