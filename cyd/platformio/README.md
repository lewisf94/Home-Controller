# CYD Arduino and PlatformIO Build

This folder contains the original CYD firmware. The current interface uses
LVGL 9.5 with TFT_eSPI as its display driver.

The build compiles, but the LVGL port requires a new hardware verification.
Use the ESP-IDF build for primary CYD development.

## Main Functions

- Embedded album browser.
- Now-playing view.
- Spotify Web API control.
- MCP23017 buttons and rotary encoder.
- Volume overlay.
- Wi-Fi signal indicator.
- Adaptive player-state polling.
- Persistent TLS connection for player-state requests.

The firmware uses separate tasks for the network, input, and MCP23017 polling.
Do not run a blocking Spotify request in an input task.

## Hardware

The target is an `ESP32-2432S028R` or a compatible CYD:

| Item | Configuration |
|---|---|
| Display | ILI9341, 320 x 240 |
| Touch | XPT2046 |
| SD card chip select | GPIO 5 |
| Input expander | MCP23017 at address `0x20` |

Use [CLAUDE.md](../../CLAUDE.md) as the source for complete pin assignments.

## Build Procedure

1. Open a terminal in this folder.
2. Build:

   ```powershell
   pio run
   ```

3. Connect the CYD.
4. Upload:

   ```powershell
   pio run -t upload
   ```

5. Open the serial monitor:

   ```powershell
   pio device monitor -b 115200
   ```

To remove generated build output, run:

```powershell
pio run -t clean
```

## First Setup

1. Install PlatformIO.
2. Create the private credential file from its example.
3. Add the Wi-Fi and Spotify credentials.
4. Prepare the SD card if the selected code path requires it.
5. Connect the CYD.
6. Identify its serial port.
7. Upload the firmware.

Refer to [SPOTIFY_SETUP.md](../../SPOTIFY_SETUP.md) for Spotify authorization.

## Project Layout

| Path | Function |
|---|---|
| `src/main.cpp` | Setup, loop, display, and touch |
| `src/ui.cpp` | LVGL interface |
| `src/input.cpp` | Input dispatcher |
| `src/mcp_input.cpp` | MCP23017 driver |
| `src/spotify.cpp` | Spotify Web API client |
| `src/albums.cpp` | Generated album metadata |
| `src/album_thumbs.cpp` | Embedded album thumbnails |
| `platformio.ini` | Board and library configuration |

Do not edit generated album files manually.

## Known Limitations

### Album Art After a Second Visit

The older JPEG path can fail after it reuses one decoder object. Create a new
decoder for each operation if this failure returns.

### Mobile Volume

Spotify can ignore volume requests for Android and iOS devices. Desktop and
Spotify Connect devices can accept the same request.

### Display Tearing

The display does not expose a usable tearing-effect signal. Fast browser
movement can show tearing.

## Development Rules

- Use LVGL for all interface drawing.
- Hold the LVGL lock when a task accesses an LVGL object.
- Send Spotify commands through the command queue.
- Keep the MCP polling task independent from player state.
- Put layout constants with the existing interface constants.
- Do not add dead-code comments.
- Follow [WRITING-STANDARD.md](../../docs/WRITING-STANDARD.md).

## Related Documents

- [Project memory](../../CLAUDE.md)
- [Roadmap](../../docs/ROADMAP.md)
- [Pending tests](../../docs/PENDING.md)
