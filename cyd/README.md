# cyd — CYD board (ESP32-WROOM, 2.8" ILI9341)

Music Controller running on the "CYD" (Cheap Yellow Display) — an ESP32-WROOM dev board with a built-in 2.8" ILI9341 touchscreen.

Two builds live here:

| Folder | Framework | Status |
|---|---|---|
| [`platformio/`](platformio/) | PlatformIO + Arduino framework | Phase 1 complete, maintenance only |
| [`esp-idf/`](esp-idf/) | ESP-IDF 5.x + LVGL | Phase 2 in progress |

Both target the same hardware and aim for feature parity. The Arduino build is the working product today; the ESP-IDF build is the foundation for Phase 3 (Home Assistant integration).

For hardware pin mapping, architecture details, and coding conventions, see [`../CLAUDE.md`](../CLAUDE.md).

For the full phased plan, see [`../docs/ROADMAP.md`](../docs/ROADMAP.md).
