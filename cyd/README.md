# cyd — CYD board (ESP32-WROOM, 2.8" ILI9341)

Music Controller firmware for the **CYD** ("Cheap Yellow Display") — a budget ESP32-WROOM dev board sold under various Sunton / generic SKUs (commonly `ESP32-2432S028R`). It bundles a 2.8" 320×240 ILI9341 SPI TFT and an XPT2046 resistive touch controller into one board. Around it we add an MCP23017 I2C IO expander breakout (CJMCU-2317) carrying four push buttons and a rotary encoder, plus an SD card slot for album thumbnails.

Two builds live here:

| Folder | Framework | LVGL | Status |
|---|---|---|---|
| [`platformio/`](platformio/) | PlatformIO + Arduino framework | n/a (TFT_eSPI direct draw) | **Phase 1 + 1.5 shipped, frozen** |
| [`esp-idf/`](esp-idf/) | ESP-IDF 6.0 native | LVGL 9.x via esp_lvgl_port | **Phase 2 in progress** — Steps 0..3 verified |

Both target identical hardware and aim for identical features. The Arduino build is the working product today. The ESP-IDF build is being brought up step-by-step so it can be the foundation for Phase 3 (Home Assistant integration) and the eventual ESP32-P4 port in [`../waveshare/`](../waveshare/).

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

**Use [`platformio/`](platformio/)** if you want the working firmware today: tap-to-play album browser, now-playing screen with cover art, working playback controls, mute / volume / play-pause overlays. Configure WiFi + Spotify credentials, build, flash, done. Code is C++ on top of TFT_eSPI, JPEGDEC, and a Spotify HTTPS client built on `WiFiClientSecure`.

**Use [`esp-idf/`](esp-idf/)** if you want to track or contribute to the IDF port. As of the latest commit Steps 0..3 of the Phase 2 plan are verified on hardware (backlight blink, full-screen colour cycle, LVGL "Hello CYD" label, XPT2046 touch driving a draggable square). Steps 4..11 (WiFi, HTTPS, SD, MCP23017, feature parity) are still ahead.

Both builds can coexist — flashing one overwrites the application partition, but switching back is just another flash. There is no shared build state between them, no shared `secrets.h`, and no shared SD card layout assumptions (the SD layout *is* identical, but each firmware mounts it independently).

---

## Project memory

For hardware pin maps, I2C addresses, architecture details, coding conventions, and the per-phase status, see [`../CLAUDE.md`](../CLAUDE.md). For the phased plan (Phase 1 fixes → Phase 2 IDF port → Phase 3 Home Assistant) see [`../docs/ROADMAP.md`](../docs/ROADMAP.md). For IDF-port gotchas discovered on real hardware, see [`../docs/PORT-NOTES.md`](../docs/PORT-NOTES.md).
