# Waveshare ESP32-P4 — next-steps backlog

Running list of what's planned for `waveshare/esp-idf/` after cp5 (now-playing
art). Ordered by priority. Update as items land.

> **NOTE:** the items below marked DONE are committed but the latest UI code
> (Cover Flow, colour accents, kerning crash fix) **still needs a hardware
> verification pass.** See `CLAUDE.md` / `docs/ROADMAP.md` for the current
> waveshare status and the post-stability work order (PPA → RAM art → poll
> backoff).

## 0. Animated-transition reliability — RESOLVED
- The UI hung whenever an *animated* `lv_screen_load_anim` ran — worst on a
  swipe out of now-playing, where the 320x320 art slides too. Root cause: the
  partial-refresh DSI flush (`TRIPLE_PARTIAL`) couldn't keep up with a full-panel
  redraw every frame and stalled the render task; the Spotify poll task then
  blocked on the LVGL lock too (both stopped, no assert, internal heap flat).
- Fixed by switching the flush to full-frame triple buffering
  (`ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_FULL` in `main.c`): full-frame suits a
  full-screen transition, `ROTATE_90` needs 3 buffers either way, still
  tear-avoided, same memory. Plus a `load_screen` guard so a second transition
  animation can't start while one is running. All four styles verified reliable
  on hardware under rapid swipes + album switches with art loaded.
- Every browser<->now-playing switch routes through one helper (`load_screen`
  in `ui.c`); transition style is user-selectable in Settings (persisted NVS),
  default NONE.

## 1. Settings menu — DONE (transition style)
- Gear button (top-right of the browser) opens a Settings screen with a Back
  control; enter/exit is always instant. First setting: **menu transition
  style** — Over / Move / Fade / None — persisted in NVS (namespace `settings`,
  key `transition`), restored at boot (default NONE). All switches go through
  the single `load_screen` helper.
- Next settings to add: dark/light theme (below), then the Cover Flow toggle.

## 2. Dark / light theme toggle — DONE
- Dark / Light buttons in Settings; persisted to NVS (`settings` / `theme`),
  restored at boot. All three screens rebuilt on switch via `lv_async_call`;
  carousel position and track labels restored after rebuild.
- `apply_theme_cb` preserves `s_centered_card` and snaps the new scroller to
  the same card via `lv_obj_scroll_to_x` (layout forced first).

## 2.5. Now-playing playback controls — DONE
- Prev / play-pause / next buttons in the right column beside the art
  (x=600..760, y=72/160/248, 160x64 each) on `s_screen_np`.
- Play/pause icon updates on every Spotify poll and on theme rebuild.
- Calls `ui_request_prev/toggle_play/next` -- same path as the encoder.

## 2.6. Spotify TLS keep-alive fix — DONE
- `poll_client_close()` was only called on `status==0`; now called on
  `err!=ESP_OK` so the broken keep-alive handle is dropped on conn-reset /
  timeout and the next poll opens a fresh TLS session.

## 3. Fonts — accented names (lv_tiny_ttf) — DONE
- `LV_USE_TINY_TTF` on; Montserrat (primary, keeps the look) + DejaVu Sans
  (fallback for accented/non-Latin glyphs) embedded via `EMBED_FILES`; 24/28px
  faces built at runtime and pointed at the title/artist labels.
- **Crash fix baked in:** all faces are created with
  `lv_tiny_ttf_create_data_ex(..., LV_FONT_KERNING_NONE, 128)`. The LVGL 9.4
  kerning cache (upstream #6304) corrupts the heap under sustained scrolling;
  KERNING_NONE bypasses the cache. **Never use plain `lv_tiny_ttf_create_data`.**
- Hard ceiling unchanged: CJK/emoji fonts are multi-MB and won't fit the app
  partition.

## 4. Cover Flow carousel (toggleable browser style) — DONE
- Three browser styles selectable in Settings (Carousel / Focus / Cover Flow),
  NVS-persisted. Cover Flow approximates the iPod look with per-card horizontal
  squash (`lv_image_set_scale_x/y`) + dim, recomputed during scroll.
- **Critical:** the transform is applied to the child `lv_image`, NOT the card
  `lv_obj`. Object-level `transform_scale`/`opa` forces a layer snapshot that the
  DIRECT-mode rotated DSI flush mis-composites → progressive card blackout.
  Image-direct transforms create no layer. `LV_USE_MATRIX` crashes (negative X in
  the SW blender). See `CLAUDE.md` CRITICAL constraints.
- `find_centered_card()` / `ui_scroll_browser()` use `scroll_x / step` math, not
  `lv_obj_get_coords()`, so the transformed visual bounds don't desync art/title.

## 5. Colour accent system — DONE
- Settings "Colour" section: Orange / Red / Green / Purple, NVS-persisted
  (`settings` / `accent`). Separate from the Dark/Light neutral palette so every
  accent works in both modes. Drives selection highlights and the progress bar.

## Later (existing roadmap)
- cp6: physical-control seam (encoder/buttons) via `input.c`.
- WiFi-strength indicator + volume HUD are in `ui.c`; confirm wiring on hardware.
- Spotify play returns 404 when there is no active device — handle gracefully
  (show a "no active device" hint) instead of a silent FAILED log.
- Performance (after stability): PPA hardware acceleration, RAM art decode
  (PSRAM), adaptive poll backoff. TLS poll keep-alive is already done (#2.6).
