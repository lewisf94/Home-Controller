# Project Memory

This file contains the stable hardware and architecture facts for Home
Controller. Read it before a non-trivial change.

Follow the work rules in [AGENTS.md](AGENTS.md). Follow the writing rules in
[WRITING-STANDARD.md](docs/WRITING-STANDARD.md).

## Product

Home Controller is a handheld music controller. It provides an album browser,
now-playing information, and playback controls.

The primary hardware is the Waveshare ESP32-P4. The repository also contains
CYD firmware and an RP2040 haptic-knob controller.

## Build Matrix

| Build | Backend | Toolchain | Status |
|---|---|---|---|
| `waveshare/esp-idf/` | Spotify and Sonos | ESP-IDF 5.5.x | Hardware-verified |
| `waveshare/esp-idf-ha/` | Home Assistant | ESP-IDF 5.5.x | Hardware-verified daily build |
| `cyd/esp-idf/` | Spotify | ESP-IDF 6.0 | Requires a new hardware check |
| `cyd/esp-idf-ha/` | Home Assistant | ESP-IDF 6.0 | Not hardware-tested |
| `cyd/platformio/` | Spotify | PlatformIO | Maintenance mode |
| `rp2040/` | Haptic knob | PlatformIO | Build-verified |
| `rp2040/bringup/` | Hardware tests | Pico SDK 2.3.0 | Checkpoints 1 and 2 verified |

## Waveshare Hardware

The primary board is the Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3.

| Item | Configuration |
|---|---|
| Microcontroller | ESP32-P4 dual-core RISC-V |
| Flash | 32 MB |
| PSRAM | 32 MB on the verified board |
| Display | ST7701 MIPI-DSI |
| Native display orientation | 480 x 800 |
| Application orientation | 800 x 480 landscape |
| Touch controller | GT911 |
| Network controller | ESP32-C6 through SDIO |
| Audio codec | ES8311 |
| Toolchain | ESP-IDF 5.5.x |

Do not use ESP-IDF 5.4 or 6.0 for the Waveshare build. The board-support
components require ESP-IDF 5.5.x.

## Waveshare Architecture

The two Waveshare builds share principal application components.

### `p4_shared`

This component contains:

- LVGL interface.
- Audio effects.
- Album metadata and runtime album catalog.
- Album-art decoding.
- Embedded fonts.
- Credential overrides.
- RP2040 knob protocol and input adapter.
- Shared player-state contract.

Keep interface and album changes in this shared component when both backends
need them.

### `app_core`

This component contains:

- Wi-Fi connection and background reconnect.
- Double-buffered album-art storage.
- Crash reports.
- Heap and allocation checks.
- Network memory gates.
- Over-the-air update support.

The direct and Home Assistant command types are different. Do not merge their
command queues only to remove similar code.

### Direct Spotify Build

The direct build connects to Spotify through HTTPS. It also controls Sonos
speakers through UPnP and SOAP.

The build uses persistent HTTP clients for player-state requests and commands.
One task owns each persistent client.

### Home Assistant Build

The Home Assistant build connects through a WebSocket. It is the verified daily
build.

The build provides:

- Music Assistant devices.
- Spotify Connect sources.
- Home Assistant lights.
- Music Assistant queue.
- Album and song search.
- Sendspin playback through the ES8311 speaker.

Sendspin starts after the initial Home Assistant state load. This sequence
reduces pressure on the SDIO receive pool.

## Waveshare Interface Constraints

The display uses direct rendering, rotation, and dirty-region tracking. An
object transform can create an intermediate layer that the display path does
not composite correctly.

Do not apply object-level scale or opacity to album cards. Apply a safe image
transform to the child image when necessary.

Do not enable matrix drawing. Matrix drawing caused invalid coordinates and a
processor fault.

Do not enable runtime Tiny TTF. The runtime rasterizer caused memory corruption
and a rasterizer assertion.

Use compiled fonts. Keep one LVGL software draw unit because two draw units
conflict with PPA transactions.

PPA display acceleration is enabled in the verified build. Do not list this
work as pending.

## Waveshare Themes And Developer Controls

The interface has six modes:

- BASIC.
- GLYPH.
- PIXEL.
- PAPER.
- BOLD.
- HIFI.

Each mode has a dark and a light face.

BOLD uses compiled Jost Bold fonts and geometric interface shapes.

HIFI is a neo broadcast-console theme. It uses static hairline grids,
registration marks, framed controls, full-colour album art, and a selectable
compiled heading font. The heading font choices are Terminal Grotesque,
GTL001, Space Mono, and Bebas Neue. HIFI uses BASIC layout defaults until a
user saves a HIFI-specific developer override.

The BOLD theme is not build-verified or hardware-verified.

The HIFI theme is not build-verified or hardware-verified. HIFI draws a grid,
a frame, and registration marks on each screen. These parts use more internal
memory than the other modes. Check the `theme ... ready:` log line against the
reliability budget before you mark HIFI as verified.

Settings has DISPLAY, SOUND, SETUP, and DEVELOPER tabs.

The DEVELOPER tab has 65 live controls in six pages:

- TYPE.
- COLOUR.
- SHAPE.
- LAYOUT.
- BROWSER.
- ART.

Developer overrides are specific to each mode.

Ground and ink overrides are also specific to each face.

The accent selection is specific to each mode.

The device stores overrides in a versioned NVS blob.

Zero is a valid value. Use a separate flag to identify an override.

Apply a control value after release, not during each slider step.

Use EXPORT TO SERIAL to create values for the tuning header.

The browser design bench is `tools/theme-bench.html`.

The bench is approximate. The hardware display is the final reference.

## P4 Reliability Gate

Internal DMA-capable memory is the critical resource. Total PSRAM does not show
the available ESP-Hosted transport memory.

Run:

```powershell
python scripts/check_p4_reliability.py both
```

After a build, run:

```powershell
python scripts/check_p4_reliability.py both --post-build
```

Application workers and queues must use PSRAM-capable FreeRTOS creation
functions where applicable.

Keep large Home Assistant snapshots cached, single-flight, rate-limited, and
memory-gated.

## CYD Hardware

The CYD is an ESP32-2432S028R or a compatible board.

### Display Pins

| Function | GPIO |
|---|---|
| MISO | 12 |
| MOSI | 13 |
| SCLK | 14 |
| CS | 15 |
| DC | 2 |
| Reset | Tied on the board |
| Backlight | 21 |

The application uses a 320 x 240 landscape orientation.

### Touch Pins

| Function | GPIO |
|---|---|
| IRQ | 36 |
| MOSI | 32 |
| MISO | 39 |
| CLK | 25 |
| CS | 33 |

The XPT2046 uses a separate SPI bus.

### SD Card

The SD card chip-select pin is GPIO 5. Older firmware can use the SD card for
album metadata, thumbnails, and fallback album art.

### MCP23017 Bus

| Function | GPIO or value |
|---|---|
| SDA | 27 |
| SCL | 22 |
| INTA | 35 |
| Address | `0x20` |

Fit 4.7 kOhm pull-up resistors from SDA and SCL to 3.3 V.

### MCP23017 Inputs

| Input | MCP23017 pin | Browser function | Now-playing function |
|---|---|---|---|
| SW1 | GPA0 | Previous album | Previous track |
| SW2 | GPA1 | Select album | Play or pause |
| SW3 | GPA2 | Next album | Next track |
| SW4 | GPA3 | Change view | Change view or shuffle |
| Encoder CLK | GPA4 | Scroll | Volume |
| Encoder DT | GPA5 | Scroll | Volume |
| Encoder switch | GPA6 | Select album | Mute |

The driver uses a gray-code state machine and a 30 ms button debounce.

The button event is a consume-on-read latch. Do not clear it during each poll.

## CYD Architecture

The CYD ESP-IDF builds use three principal execution contexts:

```text
LVGL task: Render and manage LVGL objects
Input task: Poll the MCP23017 and dispatch input
Backend task: Run Spotify or Home Assistant network operations
```

The input task sends typed commands through a queue. It does not call a network
backend directly.

The two ESP-IDF builds use `cyd/components/cyd_shared/` for interface, input,
album-art, and LittleFS code.

The Arduino build uses equivalent tasks and an LVGL lock. It remains in
maintenance mode.

## RP2040 Haptic Knob

The RP2040 performs motor control, angle measurement, press detection, button
input, and LED control.

The ESP32-P4 sends haptic configurations. The RP2040 sends position and button
events.

### RP2040 Pins

| Function | Pins | Constraint |
|---|---|---|
| Motor six-PWM | GPIO0/1, GPIO2/3, GPIO4/5 | Keep each pair on one PWM slice |
| Motor enable | GPIO6 | Digital output |
| UART1 to P4 | TX GPIO8, RX GPIO9 | Do not use UART0 pins |
| HX711 | DOUT GPIO10, CLK GPIO11 | Bit-banged interface |
| Four buttons | GPIO12 through GPIO15 | Active LOW |
| MT6701 SSI | MISO GPIO16, CS GPIO17, SCK GPIO18 | Use SPI0 mode 2 |
| RGBW ring | GPIO20 | SK6812 |
| Button LEDs | GPIO21 | SK6812 |
| I2C1 | SDA GPIO26, SCL GPIO27 | VEML7700 and MAX17048 |

The VEML7700 address is `0x10`. The MAX17048 address is `0x36`.

### ESP32-P4 UART Pins

| Function | GPIO |
|---|---|
| TX to RP2040 | 32 |
| RX from RP2040 | 46 |

GPIO33 is not available on the applicable J3 header.

Avoid GPIO14 through GPIO19 because the C6 SDIO connection uses them. Also avoid
board-support and strapping pins.

### UART Protocol

The protocol uses 921600 baud.

The frame sequence is:

1. Encode the protobuf message with nanopb.
2. Add a four-byte CRC32.
3. Apply COBS encoding.
4. Add a `0x00` delimiter.

The ESP32-P4 retries a command every 250 ms. The RP2040 acknowledges the command
with the same nonce.

Both implementations produce CRC32 `0xCBF43926` for `123456789`.

### Motor-Control Facts

- Use `BLDCDriver6PWM`.
- TMC6300 inputs are active HIGH.
- Use `MagneticSensorMT6701SSI`.
- The MT6701 SSI interface uses SPI mode 2.
- The default sensor clock is 1 MHz.
- Call `motor_shared_init()` first from core 0.
- Keep time-critical motor code in SRAM.
- A 5 kHz motor-control loop is practical on RP2040 core 1.

### Native Bring-Up

The native Pico SDK harness is in `rp2040/bringup/`.

Checkpoint 1 verified USB serial. Checkpoint 2 verified MT6701 I2C measurement
at 1000 reads per second.

The tested clone requires a USB-A to USB-C cable. Its USB-C connector does not
provide correct configuration-channel pull-down resistors.

Checkpoint 3 is a fail-safe TMC6300 motor test. It is build-verified but not
hardware-verified.

## Album Data

`spotify-albums-list.txt` is the source for all generated album lists.

Run `scripts/add_albums.py` to add Spotify albums. The tool can get metadata,
download art, generate source, and rebuild thumbnail data.

Do not edit generated album source files manually. Thumbnail order must match
album order.

## Threading Rules

LVGL is single-threaded. Use the applicable LVGL lock for every cross-task
access.

Do not hold the LVGL lock during a network operation.

Only the backend task can modify backend-owned player state. Physical input
tasks must send commands through a queue.

## Credential Rules

Keep real credentials only in the private build folder. Never commit a Wi-Fi,
Spotify, Sonos, or Home Assistant secret.

Do not print a secret in a log.

## Git Rules

Commit and push from the normal repository. Never push from the private build
folder.

Use this author:

```text
Lewis <lewisf94@users.noreply.github.com>
```

Push to `main` for normal solo work. Do not force-push without an explicit
instruction.

## Related Documents

- [Root README](README.md)
- [Roadmap](docs/ROADMAP.md)
- [Pending verification](docs/PENDING.md)
- [Hardware tests](docs/TESTING.md)
- [Port notes](docs/PORT-NOTES.md)
- [Home Assistant setup](docs/HA-SETUP.md)
- [Hardware design notes](docs/DESIGN_NOTES.md)
