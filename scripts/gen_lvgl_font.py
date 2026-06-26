#!/usr/bin/env python3
"""Bake a TTF into a compiled LVGL font (C array) -- no Node/lv_font_conv needed.

The Waveshare P4 can't use runtime tiny_ttf (its stb_truetype rasterizer
crashes), but compiled lv_font_t C arrays work fine (the PIXEL theme already
uses them). This emits the same lv_font_fmt_txt format lv_font_conv produces,
at bpp=4 (16-level anti-aliasing), using Pillow's FreeType backend to rasterise.

    python scripts/gen_lvgl_font.py FONT.ttf SIZE OUT.c VAR_NAME [--fallback NAME]

e.g.  python scripts/gen_lvgl_font.py waveshare/components/p4_shared/Arvo-Bold.ttf 28 \
          waveshare/components/p4_shared/lv_font_arvo_28.c lv_font_arvo_28 \
          --fallback lv_font_montserrat_28
      (fonts + TTFs live in the shared p4_shared component, not waveshare/esp-idf/main/)

Covers ASCII 0x20-0x7E; anything outside that range falls back to --fallback
(a compiled LVGL font, e.g. Montserrat) so accented album titles still render.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

RANGE_START = 0x20
RANGE_END = 0x7E            # inclusive
BPP = 4


def _dotify(img, pitch: int, radius: float, thresh: int, supersample: int = 3):
    """Convert a rendered glyph mask into a dot-matrix version: sample the mask
    on a fixed grid (pitch px) and, where any cell pixel is inked above thresh,
    stamp a filled round dot. Drawn supersampled then downscaled for smooth,
    anti-aliased dots. The grid is anchored to the canvas origin so dots line up
    consistently across every glyph."""
    from PIL import Image, ImageDraw

    w, h = img.size
    px = img.load()
    big = Image.new("L", (w * supersample, h * supersample), 0)
    draw = ImageDraw.Draw(big)
    r = radius * supersample
    for gy in range(0, h, pitch):
        for gx in range(0, w, pitch):
            peak = 0
            for yy in range(gy, min(gy + pitch, h)):
                row_peak = 0
                for xx in range(gx, min(gx + pitch, w)):
                    v = px[xx, yy]
                    if v > row_peak:
                        row_peak = v
                if row_peak > peak:
                    peak = row_peak
                if peak == 255:
                    break
            if peak >= thresh:
                cx = (gx + pitch / 2.0) * supersample
                cy = (gy + pitch / 2.0) * supersample
                draw.ellipse([cx - r, cy - r, cx + r, cy + r], fill=255)
    return big.resize((w, h), Image.LANCZOS)


def _quantize_nibbles(pixels: list[int]) -> bytes:
    """Pack 8-bit grey pixels (row-major) to bpp=4, 2 px/byte, first px = high
    nibble, byte-aligned at glyph start (LVGL fmt_txt PLAIN bitmap_format=0)."""
    out = bytearray()
    hi = None
    for v in pixels:
        q = (v * 15 + 127) // 255            # 0..15
        if hi is None:
            hi = q
        else:
            out.append((hi << 4) | q)
            hi = None
    if hi is not None:                        # odd pixel count -> pad low nibble
        out.append(hi << 4)
    return bytes(out)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("ttf")
    ap.add_argument("size", type=int)
    ap.add_argument("out")
    ap.add_argument("var")
    ap.add_argument("--fallback", default=None,
                    help="compiled LVGL font symbol for out-of-range glyphs")
    ap.add_argument("--dots", action="store_true",
                    help="render each glyph as a matrix of round dots")
    ap.add_argument("--dot-pitch", type=int, default=0,
                    help="dot grid spacing in px (default: ~size/6)")
    ap.add_argument("--dot-radius", type=float, default=0.0,
                    help="dot radius in px (default: ~0.42*pitch)")
    ap.add_argument("--dot-thresh", type=int, default=120,
                    help="ink threshold 0..255 for lighting a dot")
    ap.add_argument("--codepoints", default=None,
                    help="comma-separated hex codepoints to render instead of the "
                         "ASCII range (e.g. 0xF04B,0xF04C); emits a sparse cmap. "
                         "Use for icon fonts (FontAwesome symbols).")
    args = ap.parse_args()

    if args.codepoints:
        cps = sorted({int(c, 0) for c in args.codepoints.split(",") if c.strip()})
    else:
        cps = list(range(RANGE_START, RANGE_END + 1))

    # Small dots with a clear gap (radius capped under half the pitch) so adjacent
    # strokes read as separate dots instead of merging into blobs. A simple,
    # uniform-stroke base font (e.g. a bold monospace) dot-ifies far more legibly
    # than one with thin/curvy strokes.
    dot_pitch  = args.dot_pitch  or max(3, round(args.size / 8))
    dot_radius = args.dot_radius
    if not dot_radius:
        dot_radius = min(dot_pitch * 0.45, (dot_pitch - 1) / 2.0)

    try:
        from PIL import Image, ImageDraw, ImageFont
    except ImportError:
        sys.exit("Install Pillow:  pip install Pillow")

    font = ImageFont.truetype(args.ttf, args.size)
    ascent, descent = font.getmetrics()       # descent positive (px below baseline)

    # Render each glyph onto a padded canvas with a known baseline, then crop to
    # the true ink bbox (Image.getbbox) -- reliable + tight, unlike getmask2 whose
    # masks aren't always tight at the bottom for top-anchored glyphs (" ' ^ ~).
    pad = args.size
    canvas_h = ascent + descent + 2 * pad
    baseline_y = pad + ascent

    bitmap = bytearray()
    glyphs = [(0, 0, 0, 0, 0, 0)]             # id 0 reserved: idx,adv,bw,bh,ox,oy
    for cp in cps:
        ch = chr(cp)
        adv = round(font.getlength(ch) * 16)
        canvas_w = int(font.getlength(ch)) + 3 * pad
        img = Image.new("L", (canvas_w, canvas_h), 0)
        ImageDraw.Draw(img).text((pad, pad), ch, fill=255, font=font, anchor="la")
        if args.dots:
            img = _dotify(img, dot_pitch, dot_radius, args.dot_thresh)
        bbox = img.getbbox()
        idx = len(bitmap)
        if bbox is None:                      # blank glyph (e.g. space)
            glyphs.append((idx, adv, 0, 0, 0, 0))
            continue
        l, t, r, b = bbox
        bitmap += _quantize_nibbles(img.crop(bbox).tobytes())  # mode 'L': 1 byte/px, row-major
        glyphs.append((idx, adv, r - l, b - t, l - pad, baseline_y - b))

    line_height = ascent + descent
    base_line = descent
    guard = Path(args.out).stem.upper()
    fallback = f"&{args.fallback}" if args.fallback else "NULL"

    lines = []
    w = lines.append
    rng = (f"codepoints 0x{cps[0]:04X}..0x{cps[-1]:04X} ({len(cps)} sparse)"
           if args.codepoints else f"range 0x{cps[0]:02X}-0x{cps[-1]:02X}")
    w(f"/*\n * GENERATED by scripts/gen_lvgl_font.py -- do not edit by hand.\n"
      f" * Source: {Path(args.ttf).name}  size: {args.size}px  bpp: {BPP}  {rng}\n"
      f" * Out-of-range glyphs fall back to {args.fallback or '(none)'}.\n */\n")
    w('#include "lvgl.h"\n')
    # The fallback may be another generated font in this build; declare it so
    # `&<fallback>` resolves (LVGL's own fonts are already declared via lvgl.h,
    # but re-declaring them is harmless).
    if args.fallback:
        w(f"extern const lv_font_t {args.fallback};\n")
    w(f"#ifndef {guard}\n#define {guard} 1\n#endif\n#if {guard}\n")

    # --- bitmaps ---
    w("\nstatic LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {")
    for i in range(0, len(bitmap), 16):
        w("    " + "".join(f"0x{b:02x}, " for b in bitmap[i:i + 16]).rstrip())
    w("};\n")

    # --- glyph descriptors ---
    w("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {")
    for n, (idx, adv, bw, bh, ox, oy) in enumerate(glyphs):
        tag = (" /* id = 0 reserved */" if n == 0
               else f" /* U+{cps[n - 1]:04X} */")
        w(f"    {{.bitmap_index = {idx}, .adv_w = {adv}, .box_w = {bw}, "
          f".box_h = {bh}, .ofs_x = {ox}, .ofs_y = {oy}}},{tag}")
    w("};\n")

    # --- cmap ---
    if args.codepoints:
        # Sparse list: codepoints aren't contiguous, so map each via a sorted
        # unicode_list of offsets from range_start; glyph ids run 1..N in order.
        base = cps[0]
        w("static const uint16_t glyph_unicode_list[] = {")
        w("    " + ", ".join(str(cp - base) for cp in cps))
        w("};\n")
        w("static const lv_font_fmt_txt_cmap_t cmaps[] = {")
        w(f"    {{ .range_start = {base}, .range_length = {cps[-1] - base + 1},"
          f" .glyph_id_start = 1,")
        w(f"      .unicode_list = glyph_unicode_list, .glyph_id_ofs_list = NULL,"
          f" .list_length = {len(cps)},"
          " .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY }")
        w("};\n")
    else:
        w("static const lv_font_fmt_txt_cmap_t cmaps[] = {")
        w(f"    {{ .range_start = {cps[0]}, .range_length = "
          f"{cps[-1] - cps[0] + 1}, .glyph_id_start = 1,")
        w("      .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0,"
          " .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY }")
        w("};\n")

    # --- descriptor + public font (LVGL version guards mirror lv_font_conv) ---
    w("#if LVGL_VERSION_MAJOR == 8\nstatic lv_font_fmt_txt_glyph_cache_t cache;\n#endif")
    w("#if LVGL_VERSION_MAJOR >= 8\nstatic const lv_font_fmt_txt_dsc_t font_dsc = {"
      "\n#else\nstatic lv_font_fmt_txt_dsc_t font_dsc = {\n#endif")
    w("    .glyph_bitmap = glyph_bitmap,\n    .glyph_dsc = glyph_dsc,\n"
      "    .cmaps = cmaps,\n    .kern_dsc = NULL,\n    .kern_scale = 0,\n"
      f"    .cmap_num = 1,\n    .bpp = {BPP},\n    .kern_classes = 0,\n"
      "    .bitmap_format = 0,\n#if LVGL_VERSION_MAJOR == 8\n    .cache = &cache\n#endif\n};\n")

    w("#if LVGL_VERSION_MAJOR >= 8\nconst lv_font_t " + args.var +
      " = {\n#else\nlv_font_t " + args.var + " = {\n#endif")
    w("    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,\n"
      "    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,\n"
      f"    .line_height = {line_height},\n    .base_line = {base_line},\n"
      "#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)\n"
      "    .subpx = LV_FONT_SUBPX_NONE,\n#endif\n"
      "#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8\n"
      "    .underline_position = -2,\n    .underline_thickness = 1,\n#endif\n"
      "    .dsc = &font_dsc,\n"
      "#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9\n"
      f"    .fallback = {fallback},\n#endif\n    .user_data = NULL,\n}};\n")
    w(f"#endif /* {guard} */")

    Path(args.out).write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"wrote {args.out}  ({args.size}px, {len(bitmap)} bitmap bytes, "
          f"{len(glyphs) - 1} glyphs, fallback={args.fallback or 'none'})")


if __name__ == "__main__":
    main()
