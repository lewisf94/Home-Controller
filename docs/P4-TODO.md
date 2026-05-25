# Waveshare ESP32-P4 — next-steps backlog

Running list of what's planned for `waveshare/esp-idf/` after cp5 (now-playing
art). Ordered by priority. Update as items land.

## 0. Animated-transition reliability (ACTIVE)
- Freeze RESOLVED for normal use: the UI hung whenever an *animated*
  `lv_screen_load_anim` ran (a swipe / tap-to-play transition). The render task
  stalled inside the DSI flush holding the LVGL lock, so the Spotify poll task
  then blocked on `ui_set_track_info`'s lock too — both stopped, no LVGL assert,
  internal heap flat (heap theory disproved by the per-poll log, since removed).
- Fix shipped: every browser<->now-playing switch routes through one helper
  (`load_screen` in `ui.c`) honouring a transition style; **default is NONE
  (instant)**, which skips the animated composite. Confirmed freeze-free over
  multiple multi-minute runs.
- OPEN: make the animated styles (Over/Move/Fade) reliable, not just instant.
  - New evidence muddies the root cause: in one run the user switched through
    all four styles and played albums with Over/Fade active (animated) for
    ~4.5 min with no freeze. The one *confirmed* hard freeze was on a **swipe**
    (`on_gesture`), not a tap (`on_card_clicked`). Suspect overlapping/`reentrant`
    `lv_screen_load_anim` calls from rapid touch during the 250 ms animation.
  - Lever A (low risk): guard against starting a new animated transition while
    one is still running (time-window or screen-loaded flag in `load_screen`).
  - Lever B (root cause, higher risk): the adapter's `tear_avoid_mode` is
    `ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL` (`main.c`). A lighter mode
    may stop the flush stalling under animation, but risks reintroducing
    carousel-scroll tearing. Try only if Lever A + a focused swipe stress-test
    still reproduces.

## 1. Settings menu — DONE (transition style)
- Gear button (top-right of the browser) opens a Settings screen with a Back
  control; enter/exit is always instant. First setting: **menu transition
  style** — Over / Move / Fade / None — persisted in NVS (namespace `settings`,
  key `transition`), restored at boot (default NONE). All switches go through
  the single `load_screen` helper.
- Next settings to add: dark/light theme (below), then the Cover Flow toggle.

## 2. Dark / light theme toggle
- Add to the settings screen; persist in NVS.
- Route the hardcoded black-bg / white-text colours through a small theme
  (one place to flip), then it's free for new screens.

## 3. Fonts — accented names (lv_tiny_ttf)
- Built-in montserrat is ASCII-only, so "Ö", Irish á/í, etc. render as boxes.
- Plan: enable `LV_USE_TINY_TTF`, embed Montserrat (primary, keeps the look) +
  DejaVu Sans (fallback for Cyrillic/Greek/…); build the 24/28px fonts at
  runtime and point the title/artist labels at them. Hints keep the built-in
  font (they hold the LVGL arrow symbols). Future-proof: no per-character
  maintenance. Hard ceiling: CJK/emoji fonts are multi-MB and won't fit the 8MB
  app partition.

## 4. Cover Flow carousel (toggleable browser style)
- iPod/iTunes Cover Flow look: centre cover face-on + large, side covers
  receding. LVGL has no true 3D perspective, so approximate with per-card
  horizontal squash (`scale_x`) + shrink + dim, recomputed during scroll; PPA
  can accelerate the scaling.
- Expose as a setting: classic grid ↔ cover flow. Build last (most
  experimental); pull proper reference clips when implementing.

## Later (existing roadmap)
- cp6: physical-control seam (encoder/buttons) via `input.c`.
- cp7: WiFi-strength indicator, volume HUD wiring, progress/parity polish.
- Spotify play returns 404 when there is no active device — handle gracefully
  (show a "no active device" hint) instead of a silent FAILED log.
