# cyd/esp-idf — ESP-IDF build (direct Spotify, lead build)

Native ESP-IDF port of the Music Controller targeting the same CYD hardware as `../platformio/`. The display layer is LVGL 9 via `esp_lvgl_port`. The Home Assistant variant in [`../esp-idf-ha/`](../esp-idf-ha/) shares its UI / input / MCP / album-art / LittleFS code with this build through the [`../components/cyd_shared/`](../components/cyd_shared/) component — only the backend file differs (`spotify.c` here vs `ha_client.c` there).

**This is the lead build.** It is a full feature port of the Arduino code and was originally **verified smooth on hardware** (cp0-11 below). It has since absorbed a substantial backlog of perf, reliability, and UX work from the 05-24 / 05-26 / 05-27 / 05-28 / 05-30 daily reviews. None of those have been re-flashed (CYD board has been unavailable). See [`../../docs/PENDING.md`](../../docs/PENDING.md) for the full verify-pending list and [`../../docs/TESTING.md`](../../docs/TESTING.md) for the sanity-check menu next time the board is back.

Input runs in its own 2 ms FreeRTOS task and posts commands to the Spotify task via a queue, so controls stay smooth during blocking HTTPS calls.

---

## Status

| Step | Goal | State |
|---|---|---|
| 0 | Backlight blink, serial log | verified |
| 1 | esp_lcd + ILI9341, full-screen R/G/B colour cycle | verified |
| 2 | LVGL init + centred "Hello CYD" label | verified |
| 3 | XPT2046 touch + LVGL input device, draggable square | verified |
| 4 | WiFi STA, IP logged | verified |
| 5 | HTTPS to Spotify, token refresh, GET player state | verified |
| 6 | Now-playing album art via `lv_image` (JPEGDEC → LittleFS) | verified |
| 7 | Album browser from embedded RGB565 thumbnails | verified |
| 8 | LVGL carousel + centre-snap + tap/encoder-to-play | verified |
| 9 | MCP23017 I2C driver + RE1 encoder input | verified |
| 10 | Button dispatch, volume debounce, mute toggle | verified |
| 11 | Feature parity: WiFi indicator, mute badge, play-pause flash, volume HUD | verified |

### Since the cp0-11 verification — committed, **not yet re-flashed**

| Area | What landed |
|---|---|
| Perf | TLS keep-alive on the `/me/player` poll (persistent `s_poll_client`); adaptive 5 s/15 s backoff. |
| Code quality | Removed unused functions / dead-code stubs; `MAX_CARDS` truncation log; mcp_input timing `#define`s; stale "16 KB" `RESP_MAX_CAP` comment fixed; backported `err != ESP_OK` close to keep-alive client. |
| Reliability | WiFi background reconnect (`esp_timer`, 20 s) after fast retries exhaust; 404 wake-on-play (cached `s_last_device_id` + transfer-and-play); MCP re-probe every 5 s if missing at boot (`_configure_mcp` split out); `xQueueCreate` failure halts cleanly; `esp_littlefs_info` return checked; named `ESP_LOGW` on each failed dispatcher command; per-tick `event_pending` clear removed so presses during LVGL-lock timeouts aren't dropped. |
| Volume | `device.volume_percent` parsed; published to UI under the LVGL lock; `input_update` adopts edge-triggered (volume HUD no longer starts at 50 %). |
| Spotify command path | 401 mirrors the poll's token-clear (no more silent press loss right after token expiry). |
| Architecture | `ui.c` / `input.c` / `mcp_input.c` / `album_art.cpp` / `littlefs.c` extracted into [`../components/cyd_shared/`](../components/cyd_shared/) so the HA build picks them up for free. `spotify_track_t` lives in the backend-neutral `player.h` there. |
| UX | Empty album list message (no more blank carousel); volume HUD gated until first poll; JPEG SOI marker check (rejects non-JPEG bodies); on-screen MAX_CARDS warning; OFFLINE title on WiFi drop; `ui_show_toast` + "No active Spotify device" toast on play failure; auto-snap browser to playing album + accent border. |

Album art is stored in a 256 KB LittleFS partition on internal flash (avoids SD/SPI/DMA conflicts). **Known hardware limit:** browser scroll tearing — the CYD's ILI9341 TE pin isn't wired, so there's no vsync to sync redraws to; accepted as unfixable without hardware TE wiring. **Deferred:** auto-dim/sleep (needs LEDC PWM init on BL pin — see [`../../docs/PENDING.md`](../../docs/PENDING.md)). Next active work: Phase 3 (Home Assistant — see [`../esp-idf-ha/`](../esp-idf-ha/)).

---

## Requirements

- **ESP-IDF 6.0** installed and on PATH. Tested with the official Espressif Windows installer at `C:\Espressif\` / `C:\esp\v6.0\esp-idf`. Older 5.x might work but the API rename `rgb_endian` → `rgb_ele_order` (see `PORT-NOTES.md`) will need to be undone if you go back to 5.x.
- **Python** is provided by the IDF venv (`C:\Espressif\tools\python\v6.0\venv` in the official installer). You don't need a separate system Python.
- **VS Code ESP-IDF extension** is the easiest day-to-day driver, but everything below also works from the IDF terminal.

---

## Build / flash / monitor

### From VS Code (ESP-IDF extension)

1. `File → Open Folder` → select this `cyd/esp-idf/` directory (not the repo root — the extension wants the IDF project at the workspace root).
2. Set target to `esp32` via `Ctrl+Shift+P → ESP-IDF: Set Espressif device target`.
3. Plug in the CYD over USB, then `Ctrl+Shift+P → ESP-IDF: Select Port to use` → pick the COM port (usually COM3..COM7 on Windows).
4. Click the **flame icon** in the bottom status bar to build + flash + monitor in one shot. Press `Ctrl+]` in the monitor to exit.

### From the IDF PowerShell

```powershell
cd "cyd\esp-idf"
idf.py set-target esp32           # first time only
idf.py build
idf.py -p COM5 flash monitor      # replace COM5 with your port
```

After editing `main/idf_component.yml` (adding or changing managed components), run `idf.py reconfigure` so the component manager re-resolves dependencies. If the component manager seems to be ignoring a change, the cached `build/` is the usual culprit — delete it and rebuild.

---

## Project layout

```
main/
  CMakeLists.txt          SRCS: main.c, spotify.c, albums.c, album_thumbs.c.
                          REQUIRES cyd_shared (the shared component). EMBED_FILES
                          for album_thumbs.bin.
  idf_component.yml       Managed dependencies (LVGL, esp_lvgl_port, ILI9341, XPT2046)
  main.c                  App entry: WiFi + NVS + LCD/touch init + spotify_task +
                          input_task + command-queue scaffold (scmd_t, _post_cmd,
                          ui_request_* posters).
  spotify.c / spotify.h   Spotify Web API client. spotify.h is a thin wrapper that
                          #includes "player.h" from cyd_shared so the struct
                          definition stays single-sourced.
  albums.c                Generated from spotify-albums-list.txt by gen_albums.py.
  album_thumbs.{c,h,bin}  Per-build EMBED_FILES blob of 120x120 RGB565 thumbs.
sdkconfig.defaults        Project Kconfig defaults
sdkconfig                 Generated by IDF; do not edit by hand for permanent changes
CMakeLists.txt            Top-level project file -- adds
                          list(APPEND EXTRA_COMPONENT_DIRS ../components)
                          BEFORE the project() call so cyd_shared is discovered.
managed_components/       Auto-downloaded components (gitignored)
build/                    Generated; gitignored
```

The UI / input / MCP / album-art / LittleFS code lives in [`../components/cyd_shared/`](../components/cyd_shared/) — both this build and `cyd/esp-idf-ha/` link against it.

### Managed dependencies (`main/idf_component.yml`)

```yaml
dependencies:
  idf: ">=5.3.0"
  espressif/esp_lcd_ili9341: "^2.0.0"
  lvgl/lvgl: "^9.2.0"
  espressif/esp_lvgl_port: "^2.4.0"
  atanisoft/esp_lcd_touch_xpt2046: "*"
```

The XPT2046 driver is `atanisoft/...`, not `espressif/...` — Espressif maintains drivers for GT911 / FT5x06 / TT21100 but not for the XPT2046. The atanisoft package is the de-facto community driver and integrates with the standard `esp_lcd_touch` interface.

### Why the manifest is in `main/`, not at the project root

The IDF component manager looks for `idf_component.yml` **inside each component's directory**. A manifest at the project root is silently ignored. The `main/` folder IS a component, so its manifest lives there. This is the standard pattern; you'll only ever have a project-root manifest if you're using component-manager features at the project level (we're not).

### Why `sdkconfig.defaults` matters

Edits to `sdkconfig.defaults` only take effect if there's no existing `sdkconfig` (the active config) — the defaults are merged in at first configure. After editing `sdkconfig.defaults`, either delete `sdkconfig` and rebuild, or apply the changes interactively via `idf.py menuconfig`. Our current defaults:

```
CONFIG_FREERTOS_HZ=1000
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_LV_COLOR_DEPTH_16=y                # LVGL needs to know we're 16-bit
CONFIG_SPI_FLASH_SUPPORT_BOYA_CHIP=y      # CYD ships with BOYA flash on some revisions
```

---

## Troubleshooting

### "Build seems to ignore my source edits"

VS Code can silently overwrite on-disk edits if you have the same file open in an editor tab — Ctrl+S writes the (stale) tab contents back to disk. Close the file's tab while iterating, or `File → Revert File` before saving. Boot-time canary strings (e.g. `ESP_LOGI(TAG, "step N (some-tag)")`) are the cheapest way to confirm a fresh binary is on the chip — also check the app `Compile time:` line in the monitor output.

### "Manifest changes don't take effect"

Either the cached `build/` has the previous CMake state, or there's a stale `dependencies.lock` at the project root. Delete `build/` and `dependencies.lock` and re-run a full build. `idf.py fullclean` also works.

### "Touch doesn't fire any events"

The IRQ-driven path through `esp_lvgl_port` can wedge on this board. Set `int_gpio_num = GPIO_NUM_NC` in the touch config so the driver polls the chip over SPI on every LVGL tick. See `PORT-NOTES.md` for the full story.

### "Square / widget drifts under my finger"

LVGL 9 active screens are scrollable by default — press-and-drag scrolls the screen, which offsets every `lv_indev_get_point()`. Remove the flag once at setup:

```c
lv_obj_remove_flag(lv_screen_active(), LV_OBJ_FLAG_SCROLLABLE);
```

### "BOYA flash" warning at boot

Enable `CONFIG_SPI_FLASH_SUPPORT_BOYA_CHIP=y` in `sdkconfig.defaults` (and regenerate `sdkconfig`). The generic driver works, but the BOYA-specific one is faster and gets sleep-mode behaviour right.

---

## Project memory

- Hardware pin map, I2C addresses, architecture, coding conventions: [`../../CLAUDE.md`](../../CLAUDE.md)
- Full phase plan (incl. Phase 3 Home Assistant integration that builds on top of this): [`../../docs/ROADMAP.md`](../../docs/ROADMAP.md)
- IDF-port gotchas with root cause + fix per entry: [`../../docs/PORT-NOTES.md`](../../docs/PORT-NOTES.md)
- Hardware test checklist: [`../../docs/TESTING.md`](../../docs/TESTING.md)
