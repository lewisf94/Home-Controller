# cyd — CYD board (ESP32-WROOM, 2.8" ILI9341)

Music Controller firmware for the **CYD** ("Cheap Yellow Display") — a budget ESP32-WROOM dev board sold under various Sunton / generic SKUs (commonly `ESP32-2432S028R`). It bundles a 2.8" 320×240 ILI9341 SPI TFT and an XPT2046 resistive touch controller into one board. Around it we add an MCP23017 I2C IO expander breakout (CJMCU-2317) carrying four push buttons and a rotary encoder.

> The primary development board is now the **Waveshare ESP32-P4** — see [`../waveshare/`](../waveshare/). The CYD builds are actively maintained but no longer the lead.

Three builds live here, plus one shared ESP-IDF component:

| Folder | Framework | LVGL | Status |
|---|---|---|---|
| [`esp-idf/`](esp-idf/) | ESP-IDF 6.0 native, direct Spotify | LVGL 9.x via esp_lvgl_port | Feature-complete + originally hardware-verified; recent perf/reliability/UX batches need a CYD re-flash |
| [`esp-idf-ha/`](esp-idf-ha/) | ESP-IDF 6.0 native, Home Assistant backend | LVGL 9.x via esp_lvgl_port | Shares UI/input/etc with `esp-idf/` via `cyd_shared`; only the backend (`ha_client.c`) is HA-specific; **never hardware-tested** |
| [`platformio/`](platformio/) | PlatformIO + Arduino framework | LVGL 9.5 (ported from TFT_eSPI direct draw) | Phase 1 + 1.5 shipped on hardware; LVGL port + recent perf/reliability fixes committed, needs re-verify; maintenance mode |
| [`components/cyd_shared/`](components/cyd_shared/) | ESP-IDF component | — | Shared `ui.c` + `input.c` + `mcp_input.c` + `album_art.cpp` + `littlefs.c` (with their headers + `player.h`) used by both IDF builds |

The two IDF builds share UI/input/MCP/album-art/LittleFS code through the `cyd_shared` component, so a fix to either benefits both. The HA variant (`esp-idf-ha/`) swaps the Spotify backend for a Home Assistant WebSocket client (Phase 3) — see [`../docs/HA-SETUP.md`](../docs/HA-SETUP.md) for the Pi 5 setup guide. The Arduino build is in maintenance mode (perf and reliability fixes welcome, no new features). The `cyd_shared` UI stack is *not* used by the Waveshare build — that has its own `ui.c` laid out for 800×480.

---

## Hardware summary

The CYD specs (typical revision):

- **MCU:** ESP32-D0WD-V3 dual-core Xtensa @ 240 MHz, 4 MB flash (BOYA chip on this revision, no PSRAM)
- **Display:** ILI9341, 2.8", 320×240 (landscape), SPI bus
- **Touch:** XPT2046 resistive, separate SPI bus
- **Backlight:** GPIO 21, PWM-capable via LEDC
- **SD card slot:** on-board, default VSPI, CS on GPIO 5
- **USB-serial:** CP2102 or CH340 depending on clone, 115200 baud

Around it we add:

- **MCP23017 IO expander** (CJMCU-2317 breakout) on the default I2C bus (SDA = GPIO 27, SCL = GPIO 22 @ 400 kHz; address `0x20`, INT on GPIO 35 active-LOW). Hosts 4 buttons (SW1..SW4) and one rotary encoder (RE1: CLK / DT / SW). Recommend external 4.7 kΩ pull-ups on SDA/SCL — the ESP32 internals are too weak for reliable I2C.
- **SD card** on GPIO 5 — holds `metadata.csv`, 12,800-byte RGB565 thumbnails, and the JPEG fallback `nowplaying.jpg`.
- A reserved second rotary encoder + mute switch (RE2) is stubbed in software for future fitment.

Full pin tables — TFT, touch, I2C, MCP23017 input mapping — live in [`../CLAUDE.md`](../CLAUDE.md). Pick *that* file as the source of truth.

### What "input on Port A" means

To consolidate wiring, every input (4 push-buttons + RE1's CLK/DT/SW) is wired to MCP23017 GPA0..GPA6. INTA is configured as push-pull active-LOW with interrupt-on-CHANGE on all seven pins, so any button press or encoder edge fires the ESP32 INT line immediately. A poll path runs alongside to keep debounce timers ticking. Port B is unused. See `src/mcp_input.cpp` in the Arduino build for the gray-code state machine and debounce timing.

---

## Choosing a build

**Use [`esp-idf/`](esp-idf/)** for the lead firmware: it is a feature-complete, hardware-verified ESP-IDF build with the LVGL album browser, now-playing screen with cover art, MCP23017 buttons + RE1 encoder, and a Spotify HTTPS client (token persisted to NVS). Input runs on its own FreeRTOS task so controls stay smooth during blocking network calls. Configure WiFi + Spotify credentials in `include/secrets.h`, build, flash, done.

**Use [`esp-idf-ha/`](esp-idf-ha/)** for the Home Assistant variant — the same UI with the Spotify backend replaced by a WebSocket client to a Music Assistant `media_player` entity. Not yet hardware-tested.

**Use [`platformio/`](platformio/)** for the original Arduino build, now ported to LVGL 9.5 to match the IDF look (embedded thumbnails, centre-snap carousel, volume HUD, WiFi bars). It compiles and fits but the LVGL port has not been re-flashed yet — colours/byte-order, touch, heap, and TLS are unverified. Code is C++ on top of TFT_eSPI (as LVGL's flush driver), JPEGDEC, and a Spotify HTTPS client built on `WiFiClientSecure` with verified TLS.

All three builds can coexist — flashing one overwrites the application partition, but switching back is just another flash. There is no shared build state between them and no shared `secrets.h`.

---

## Project memory

For hardware pin maps, I2C addresses, architecture details, coding conventions, and the per-phase status, see [`../CLAUDE.md`](../CLAUDE.md). For the phased plan (Phase 1 fixes → Phase 2 IDF port → Phase 3 Home Assistant) see [`../docs/ROADMAP.md`](../docs/ROADMAP.md). For IDF-port gotchas discovered on real hardware, see [`../docs/PORT-NOTES.md`](../docs/PORT-NOTES.md).
