# cyd/esp-idf — ESP-IDF build (Phase 2, active)

Native ESP-IDF 5.x port of the Music Controller targeting the same CYD hardware as `../platformio/`. Uses LVGL for the display layer; this same stack carries over to the Waveshare ESP32-P4 build later.

**Status:** Phase 2 Step 0 — scaffold in place. Next: `idf.py build` + hardware verify (backlight blink), then Step 1 (colour cycle).

## Build

```bash
cd cyd/esp-idf
idf.py set-target esp32          # first time only
idf.py build                     # build
idf.py -p COM<X> flash monitor   # flash + serial monitor
idf.py reconfigure               # after editing idf_component.yml
```

## Project memory

See [`../../CLAUDE.md`](../../CLAUDE.md) at the repo root for full hardware details, pin mapping, architecture notes, and coding conventions.

See [`../../docs/ROADMAP.md`](../../docs/ROADMAP.md) Phase 2 for the migration step list and Phase 3 for the Home Assistant integration that builds on top of this.

Hardware gotchas discovered during the port go in [`../../docs/PORT-NOTES.md`](../../docs/PORT-NOTES.md).
