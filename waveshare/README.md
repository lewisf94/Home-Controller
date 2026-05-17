# waveshare — ESP32-P4 build (planned)

Future build targeting the **Waveshare ESP32-P4-WIFI6** dev board: a 4.3" 480×800 MIPI-DSI TFT with GT911 capacitive touch, driven by the ESP32-P4 main MCU, with WiFi handled by an on-board ESP32-C6 over ESP-Hosted. The Music Controller will migrate to this board after the Phase 3 Home Assistant integration is finished on the CYD's ESP-IDF build.

**Status:** not started — board has not yet arrived. This folder is a placeholder so the repo layout matches the planned architecture.

---

## Why move to the P4

The CYD is a great prototype board but limited:

- Only 4 MB flash, no PSRAM on most revisions (album thumbnails are tight)
- 240×320 panel, SPI-bus-limited refresh rate
- Resistive touch — needs calibration, no multi-touch
- ESP32 (Xtensa LX6), no hardware JPEG, modest CPU

The ESP32-P4 fixes all of those:

| Spec | CYD (ESP32-WROOM) | Waveshare ESP32-P4 |
|---|---|---|
| MCU | ESP32 dual-core Xtensa LX6 @ 240 MHz | ESP32-P4 dual-core RISC-V @ 360 MHz |
| RAM | 520 KB SRAM (no PSRAM most revs) | 768 KB SRAM + 32 MB PSRAM |
| Flash | 4 MB | 16 MB |
| Display | 2.8" 320×240 ILI9341 over SPI | 4.3" 480×800 over MIPI-DSI |
| Touch | XPT2046 resistive (1 point) | GT911 capacitive (5 points) |
| WiFi | Built-in ESP32 WiFi | ESP32-C6 co-processor via ESP-Hosted |
| Hardware acceleration | None | PPA (2D), JPEG codec, MIPI-CSI camera in |

For us the big wins are PSRAM (cache the whole album library in RAM, no SD seeks during browsing), the larger display (more readable, multi-touch gestures), and the hardware JPEG codec (decode album art near-instantly instead of multi-hundred-millisecond software decodes on the CYD).

---

## Planned approach

The architecture mirrors [`../cyd/esp-idf/`](../cyd/esp-idf/) — same ESP-IDF, same LVGL — with three hardware drivers swapped out:

- **Display:** `esp_lcd_mipi_dsi` (DPI + DSI lanes) instead of `esp_lcd_panel_io_spi`. Resolution becomes 480×800 (portrait) or 800×480 (landscape) — we'll pick once the board arrives and we see the panel orientation defaults.
- **Touch:** `espressif/esp_lcd_touch_gt911` managed component. GT911 is I2C, multi-touch, well-supported by Espressif and the LVGL port layer. Should be a near-drop-in replacement for the XPT2046 from a `lvgl_port_add_touch` standpoint.
- **WiFi:** `esp_hosted` — the P4 doesn't have its own radio, it goes through the on-board ESP32-C6 over SDIO. Different `Kconfig` selection vs `esp_wifi` but the connection / DHCP API surface is the same.

What carries over **unchanged**:

- LVGL 9 UI code (widgets, screens, event handlers)
- `ha_client/` component from Phase 3 (Home Assistant WebSocket + state subscription) — the whole point of the IDF port is so this component can be reused here
- `sdkconfig.defaults` for LVGL color depth and project task config
- The album-art conversion pipeline in [`../scripts/`](../scripts/), though we'll likely move to PSRAM-backed in-RAM cache and drop the SD card entirely

What changes from a hardware-design standpoint:

- No MCP23017 needed — the P4 has plenty of GPIO and capacitive multi-touch replaces most physical controls. We'll do a fresh enclosure / control-panel design once the board is in hand.
- Possible stretch: the P4 has an audio codec on board. Could run librespot locally to be a Spotify Connect *target* instead of just a remote control. Deferred well past Phase 3.

---

## When this starts

After [`../cyd/esp-idf/`](../cyd/esp-idf/) reaches feature parity (Phase 2 Step 11) **and** Phase 3 (Home Assistant integration on the CYD) is shipped. At that point the `ha_client/` component is proven, the LVGL UI is proven, and the only unknown is hardware-specific bring-up. The Phase 2 step-checkpoint pattern (Step 0 = blink, Step 1 = colour cycle, etc.) will be re-run on the P4 board to flush out hardware surprises before tackling features.

See [`../docs/ROADMAP.md`](../docs/ROADMAP.md) under "Future — ESP32-P4 migration" for the full plan.

---

## Project memory

Architecture details and the project-wide conventions are in [`../CLAUDE.md`](../CLAUDE.md). For hardware-port gotchas as they're discovered on the CYD IDF build (which will mostly carry over here), see [`../docs/PORT-NOTES.md`](../docs/PORT-NOTES.md).
