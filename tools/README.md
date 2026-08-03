# Interface Design Tools

This folder contains design tools for the Waveshare interface.

These tools are not part of the firmware build.

## Theme Bench

`theme-bench.html` shows an approximate 800 x 480 interface preview.

It has controls that match the on-device DEVELOPER pages.

Open the file directly in a browser.

```powershell
Start-Process .\tools\theme-bench.html
```

The bench uses project layout values, palette values, and subset fonts.

Use it for a fast first design pass.

Use the hardware display for the final decision.

## Value Transfer

Use this sequence:

1. Adjust values in the browser bench.
2. Export the values.
3. Apply the values to the firmware tuning header.
4. Build and flash the firmware.
5. Adjust the values in Settings > DEVELOPER.
6. Select EXPORT TO SERIAL.
7. Record the serial values in the tuning header.
8. Reset the mode override.
9. Commit the final values.

NVS overrides apply only to one controller.

The values become project defaults only after a source change.

## Approximations

The bench does not reproduce these functions:

- Real album art.
- PIXEL dithering.
- PAPER duotone art.
- GLYPH dot-matrix art.
- Cover Flow.
- Device icon fonts.
- Exact bitmap-font metrics.
- Complete GLYPH, PIXEL, and PAPER interface details.
- The BOLD and HIFI modes. The bench has no preview for either mode. Use the
  hardware display to judge these two modes.

## Fonts

The `fonts` folder contains the offline WOFF2 subsets for the bench.

The subsets contain ASCII characters only.

Regenerate a subset from the applicable full TTF when the firmware font
changes.

Example:

```powershell
pyftsubset waveshare/components/p4_shared/Jost-Bold.ttf `
  --output-file=tools/fonts/Jost-Bold-subset.woff2 `
  --unicodes="U+0020-007E" `
  --flavor=woff2 `
  --layout-features="*"
```
