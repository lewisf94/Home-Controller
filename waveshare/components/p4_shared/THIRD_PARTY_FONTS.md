# Neo Hi-Fi heading fonts

The compiled `lv_font_hifi_*.c` files are generated font software. They cover
printable ASCII and fall back to the project's compiled `lv_font_hc_*` faces
for other supported characters. They are distributed under the SIL Open Font
License 1.1 in [OFL-1.1.txt](OFL-1.1.txt).

- Terminal Grotesque — Raphaël Bastide, with contributions by Jérémy Landes;
  distributed by Velvetyne Type Foundry. Source:
  <https://gitlab.com/raphaelbastide/Terminal-Grotesque>
- GTL001 — Copyright 2022 The GTL001 Project Authors. Source:
  <https://github.com/eliheuer/GTL001>
- Space Mono — Copyright 2016 The Space Mono Project Authors. Source:
  <https://github.com/googlefonts/spacemono>
- Bebas Neue — Copyright © 2010 Dharma Type. Source:
  <https://github.com/dharmatype/Bebas-Neue>

Regenerate from the upstream TTF files with `scripts/gen_lvgl_font.py`, using
the source size recorded in each generated file header and the corresponding
`lv_font_hc_20` or `lv_font_hc_28` fallback. The `_20`/`_28` filenames identify
the intended UI slot; source sizes are optically normalised per family so their
line boxes remain comparable.
