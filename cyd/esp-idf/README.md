# CYD ESP-IDF Direct-Spotify Build

This firmware uses ESP-IDF 6.0 and LVGL 9 on the CYD board. It connects directly
to the Spotify Web API.

The Home Assistant variant uses the same interface and input code. Both builds
link the [`cyd_shared`](../components/cyd_shared/README.md) component.

The Waveshare ESP32-P4 is now the primary development board. This CYD build
remains available for maintenance and verification.

## Status

The original checkpoint series passed on hardware:

| Checkpoint | Function | Status |
|---|---|---|
| 0 | Backlight and serial output | Verified |
| 1 | ILI9341 color test | Verified |
| 2 | LVGL display | Verified |
| 3 | XPT2046 touch | Verified |
| 4 | Wi-Fi station | Verified |
| 5 | Spotify authorization and player state | Verified |
| 6 | JPEG album art | Verified |
| 7 | Embedded album thumbnails | Verified |
| 8 | Carousel and album selection | Verified |
| 9 | MCP23017 and rotary encoder | Verified |
| 10 | Buttons, volume, and mute | Verified |
| 11 | Status indicators and overlays | Verified |

Newer reliability and interface changes require another hardware test. Refer to
[PENDING.md](../../docs/PENDING.md) and
[TESTING.md](../../docs/TESTING.md).

The unverified changes include:

- Persistent TLS for player-state requests.
- Adaptive player-state polling.
- Wi-Fi reconnect after initial retries fail.
- Spotify wake-on-play after a device becomes inactive.
- MCP23017 detection after a startup failure.
- Better queue-allocation and LittleFS error handling.
- Player-volume synchronization.
- Offline and command-failure messages.
- Automatic browser selection for the active album.

The ILI9341 tearing-effect pin is not connected. Browser movement can show
tearing because the firmware cannot synchronize to the display refresh.

## Requirements

- ESP-IDF 6.0.
- The ESP-IDF Python environment.
- A USB serial connection to the CYD.
- Spotify credentials for the direct backend.

Use the ESP-IDF 6.0 APIs. ESP-IDF 5 uses a different LCD color-order field.

## Build Procedure

1. Open an ESP-IDF 6.0 terminal.
2. Change to this folder:

   ```powershell
   cd "cyd\esp-idf"
   ```

3. Set the target during the first configuration:

   ```powershell
   idf.py set-target esp32
   ```

4. Build the firmware:

   ```powershell
   idf.py build
   ```

5. Connect the CYD.
6. Identify its COM port.
7. Flash and monitor the firmware:

   ```powershell
   idf.py -p COM5 flash monitor
   ```

Replace `COM5` with the correct port.

Press `Ctrl+]` to close the monitor.

## VS Code Procedure

1. Open the `cyd/esp-idf/` folder in VS Code.
2. Select the `esp32` target.
3. Select the CYD serial port.
4. Run the ESP-IDF build, flash, and monitor command.

Open this build folder as the workspace root. The extension can select an
incorrect project when you open the repository root.

## Project Layout

| Path | Function |
|---|---|
| `main/main.c` | Board initialization, tasks, and command queue |
| `main/spotify.c` | Spotify Web API client |
| `main/albums.c` | Generated album metadata |
| `main/album_thumbs.c` | Embedded browser thumbnails |
| `main/idf_component.yml` | Managed component requirements |
| `sdkconfig.defaults` | Permanent configuration defaults |
| `sdkconfig` | Generated active configuration |
| `../components/cyd_shared/` | Shared interface, input, art, and storage code |

Do not edit generated album files manually. Use the album scripts from the
repository root.

## Managed Components

The component manifest contains these principal requirements:

```yaml
dependencies:
  idf: ">=5.3.0"
  espressif/esp_lcd_ili9341: "^2.0.0"
  lvgl/lvgl: "^9.2.0"
  espressif/esp_lvgl_port: "^2.4.0"
  atanisoft/esp_lcd_touch_xpt2046: "*"
```

The XPT2046 package is a community driver. It implements the standard
`esp_lcd_touch` interface.

After a manifest change, run:

```powershell
idf.py reconfigure
```

## Configuration Defaults

The project uses these important defaults:

```text
CONFIG_FREERTOS_HZ=1000
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_LV_COLOR_DEPTH_16=y
CONFIG_SPI_FLASH_SUPPORT_BOYA_CHIP=y
```

Changes to `sdkconfig.defaults` do not replace values in an existing
`sdkconfig`. Use `idf.py menuconfig` or create a new active configuration.

## Troubleshooting

### Source Changes Do Not Appear

1. Save all editor files.
2. Check the application compile time in the serial output.
3. Run a clean build if the compile time is old.

An open editor can write stale content over a file. Close or reload stale
editor tabs before you save.

### Manifest Changes Do Not Appear

1. Run `idf.py reconfigure`.
2. Remove stale build output if the problem remains.
3. Remove a stale dependency lock file if necessary.
4. Build again.

### Touch Does Not Respond

Use polling for the XPT2046. Set its interrupt GPIO to `GPIO_NUM_NC`.

The interrupt path can stop on this board. Refer to
[PORT-NOTES.md](../../docs/PORT-NOTES.md) for details.

### The Screen Moves During a Drag

Remove the scrollable flag from the active screen:

```c
lv_obj_remove_flag(lv_screen_active(), LV_OBJ_FLAG_SCROLLABLE);
```

### A BOYA Flash Warning Appears

Enable `CONFIG_SPI_FLASH_SUPPORT_BOYA_CHIP=y`. Reconfigure the project after you
change the default.

## Related Documents

- [Project memory](../../CLAUDE.md)
- [Roadmap](../../docs/ROADMAP.md)
- [Port notes](../../docs/PORT-NOTES.md)
- [Hardware tests](../../docs/TESTING.md)
