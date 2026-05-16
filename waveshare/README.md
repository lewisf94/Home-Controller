# waveshare — ESP32-P4 build (planned)

Future build targeting the Waveshare ESP32-P4-WIFI6 board (4.3" 480×800 MIPI-DSI display, GT911 capacitive touch, ESP-Hosted WiFi via onboard ESP32-C6).

**Status:** not started — board has not yet arrived.

## Planned approach

- ESP-IDF 5.x, LVGL UI (same library as `../cyd/esp-idf/`)
- MIPI-DSI display via `esp_lcd_mipi_dsi`
- GT911 capacitive touch via `esp_lcd_touch_gt911`
- WiFi through `esp_hosted` (ESP32-C6 co-processor)
- Home Assistant WebSocket client carries over unchanged from `../cyd/esp-idf/`

See `../docs/ROADMAP.md` ("Future — ESP32-P4 migration") for full plan.
