# waveshare/esp-idf — ESP32-P4, direct Spotify (no HA)

Music Controller on the **Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3** (ESP32-P4
RISC-V, 4.3" 480×800 IPS, **ST7701** MIPI-DSI, **GT911** touch, WiFi6/BLE5 via an
onboard **ESP32-C6** over SDIO, PSRAM, 32 MB flash). Talks **directly to the
Spotify Web API**. A future `waveshare/esp-idf-ha/` will swap the backend to
Home Assistant, exactly like the CYD split.

> **STATUS: checkpoint-1 skeleton (display bring-up only). Hardware-verified:
> "Music Controller P4 / checkpoint 1: display OK" label renders at 800×480
> landscape on the physical board.** Built incrementally — see the checkpoint
> roadmap below. Most app logic (Spotify client, album data, art decode, LittleFS)
> is copied unchanged from `../../cyd/esp-idf/`; the UI and input come later.

## Reference
Waveshare official component + demos:
https://github.com/waveshareteam/ESP32-P4-WIFI6-Touch-LCD-4.3
- LVGL bring-up copied from its `08_lvgl_demo_v9` (`bsp_display_start_with_config`,
  `bsp_display_lock/unlock`, rotation flag, triple-partial tear-avoid).
- WiFi config from its `04_wifistation` (`esp_wifi_remote` + `esp_hosted`).

## ONE-TIME SETUP (do this before building)

1. **Step 0 — validate the board (fresh out of box).** Clone the Waveshare repo
   and flash a demo to confirm the toolchain + display + C6 WiFi work:
   `idf.py set-target esp32p4` (installs the RISC-V toolchain), then build/flash
   `08_lvgl_demo_v9` (display) and `04_wifistation` (WiFi). If WiFi fails,
   re-flash the ESP32-C6 ESP-Hosted slave to match the IDF `esp_wifi_remote`/
   `esp_hosted` versions (see the repo / Espressif `esp-hosted-mcu` docs).
   **Don't proceed until both demos run.**

2. **Vendor the BSP** (it is *not* on the component registry). Copy the board
   component into this project:
   `cp -r <waveshare-repo>/examples/esp-idf/08_lvgl_demo_v9/components/esp32_p4_wifi6_touch_lcd_4_3  waveshare/esp-idf/components/`
   (Audio `bsp_extra` is not needed — this is a remote, not a player.)
   The BSP is also checked in at `components/esp32_p4_wifi6_touch_lcd_4_3/`.

3. **Secrets:** `cp include/secrets.h.example include/secrets.h` and fill in WiFi
   + Spotify credentials (gitignored).

4. Build: `idf.py set-target esp32p4` then `idf.py build flash monitor`.

## Board facts (confirmed from the repo manifests)
- Panel **ST7701** MIPI-DSI, 480×800 native; **rotated to 800×480 landscape** via
  the adapter `rotation` flag in `main.c`.
- Touch **GT911** (I2C). After rotating the display, `touch_flags`
  (swap_xy/mirror_x/mirror_y) in `main.c` may need adjusting so touch aligns.
- LVGL **9.4** via **`esp_lvgl_adapter`** (NOT `esp_lvgl_port` like the CYD) —
  the lock is `bsp_display_lock(timeout)` / `bsp_display_unlock()`. When porting
  `ui.c`, map `lvgl_port_lock(0)` → `bsp_display_lock(0)`.
- WiFi via `esp_wifi_remote` `0.14.*` + `esp_hosted` `1.4.*`, slave = esp32c6,
  SDIO. The `esp_wifi_*` API is routed to the C6, so `wifi_init_sta` ports nearly
  unchanged from the CYD build.
- PSRAM on. **Use ESP-IDF 5.5.x — NOT 5.4, NOT 6.0** (verified by build). Two
  constraints pin it to 5.5:
  - The BSP uses the USB-host API (`usb/usb_host.h`, for `bsp_usb_host_start`),
    provided by the `usb` component. **6.0 removed that component**, so 6.0.x
    fails configure with `Failed to resolve component 'usb'`.
  - The BSP's `esp_lvgl_adapter` dependency requires **IDF ≥5.5**, so **5.4.x is
    too old** (`no versions of idf match >=5.5.0`).
  - 5.5.x is the only line that satisfies both. Install via EIM
    (`eim install -i v5.5.4 -t esp32p4 -n true`) or the VS Code extension
    (Configure → Express → v5.5.x). Keep 6.0.x for the CYD builds.
- **SRAM budget:** the full app stack (display + WiFi + TLS + art) overflows
  internal SRAM by ~451 B with the naive config. Sources and deps are therefore
  staged per checkpoint (see `main/CMakeLists.txt` comments); DRAM budgeting
  happens deliberately at cp2/cp3 with the linker map.

## Checkpoint roadmap
1. **Display skeleton** — `bsp_display_start_with_config` + hello label. *(hardware-verified)*
2. **WiFi** — add `esp_wifi_remote`; port `wifi_init_sta`; log the IP.
3. **Spotify** — Spotify task + `scmd_t` command queue (port the structure from
   `cyd/esp-idf/main.c`); log the track title.
4. **UI** — port `cyd/esp-idf/main/ui.c`; `lvgl_port_lock` → `bsp_display_lock`;
   re-lay-out constants for 800×480; touch scroll + tap-to-play.
5. **Assets** — regen `album_thumbs.bin` larger via `scripts/embed_albums_idf.py`;
   bigger now-playing art (album_art.cpp) + fonts.
6. **Touch controls** — on-screen prev/play-pause/next + volume slider → `ui_request_*()`.
7. **Parity** — WiFi indicator, volume HUD, progress bar, view toggle; leave an
   `input.c` seam for optional physical buttons/encoder later.

## Already in this folder
- Copied board-agnostic, unchanged: `spotify.c/.h`, `albums.c/.h`,
  `album_art.cpp/.h`, `littlefs.c/.h`, `album_thumbs.c/.h/.bin`.
- New: `main.c` (skeleton), `sdkconfig.defaults`, `partitions.csv`, `CMakeLists.txt`,
  `main/CMakeLists.txt`, `main/idf_component.yml`, `include/secrets.h.example`.
- **Vendored BSP:** `components/esp32_p4_wifi6_touch_lcd_4_3/` (checked in).
