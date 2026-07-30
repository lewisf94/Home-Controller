# tools/

Design tooling for the Waveshare ESP32-P4 UI. Nothing here is compiled into the
firmware.

## theme-bench.html

A browser preview of the P4 UI (800x480, correct proportions) with a live
control panel. Open it directly — no server, no build step:

```
# just double-click it, or
xdg-open tools/theme-bench.html      # macOS: open, Windows: start
```

It renders from the firmware's own numbers: layout coordinates taken from
`ui_tune.h`, palettes from the `theme_t` values in `p4_shared/ui.c`, and the
real `Jost-Bold` face (subset copies live in `tools/fonts/`).

### What it is for

Judging shape and palette decisions without a 2-minute build-and-flash cycle.
Every slider/toggle corresponds to a knob that ALSO exists on the device under
**Settings > DEVELOPER**, and the export panel emits `ui_tune.h` `#define` lines
in byte-identical format to the device's **EXPORT TO SERIAL** output.

### Round trip

```
   bench (instant, approximate)
      |  paste the emitted #define block
      v
   ui_tune.h  --> idf.py build flash
      |
      v
   Settings > DEVELOPER  (real panel, real fonts, real album art)
      |  tap EXPORT TO SERIAL, copy from the monitor
      v
   ui_tune.h  --> commit
```

The bench is the fast, rough pass; the device is the truth. Values only become
permanent when they land back in `ui_tune.h` and get committed — DEVELOPER
overrides live in that one board's NVS and are wiped by `RESET MODE`.

### Known approximations

The bench is not a simulator. It deliberately does not model:

- **Album art** — abstract placeholder gradients, not real covers, and none of
  the per-theme art treatments (PIXEL dither, PAPER duotone, GLYPH dot-matrix).
- **Cover Flow** — the PSRAM column rasteriser has no browser equivalent; only
  the Carousel layout is shown.
- **Icons** — SVG stand-ins. The THIN/SOLID switch is a preview of what a
  custom icon font could look like; the firmware is still limited to LVGL's
  baked FontAwesome subset (see the note in `ui_tune.h` about which codepoints
  are safe in the PIXEL font).
- **Text metrics** — the browser uses the TTF with its own hinting; the device
  renders a 4bpp bitmap baked by `scripts/gen_lvgl_font.py`, so glyph edges and
  line heights differ slightly.
- **Themes other than BASIC/BOLD** — GLYPH, PIXEL and PAPER carry bespoke
  chrome that is not reproduced here.

## fonts/

WOFF2 subsets (ASCII only) of the faces the bench needs, so it works offline
with no CDN. Regenerate from the full TTFs in `waveshare/components/p4_shared/`:

```bash
pyftsubset waveshare/components/p4_shared/Jost-Bold.ttf \
  --output-file=tools/fonts/Jost-Bold-subset.woff2 \
  --unicodes="U+0020-007E" --flavor=woff2 --layout-features='*'
```
