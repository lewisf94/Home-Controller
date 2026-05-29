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

## 6. Backlight brightness — DONE (needs hardware verify)
- Settings "Brightness" section: a horizontal slider (10–100%) that live-dims the
  panel via the BSP LEDC PWM (`bsp_display_brightness_set`) while dragging,
  persisted to NVS (`settings` / `brightness`) on release and re-applied at boot
  in `ui_init`. Floored at 10% so it can never be dimmed to a black screen.
- Benign boot warning `ledc: GPIO 26 is not usable` is the BSP backlight PWM
  channel double-init; dimming still works (`backlight_on()` == `brightness_set(100)`).

## Later (existing roadmap)
- cp6: physical-control seam (encoder/buttons) via `input.c`.
- WiFi-strength indicator + volume HUD are in `ui.c`; confirm wiring on hardware.
- Spotify play returns 404 when there is no active device — handle gracefully
  (show a "no active device" hint) instead of a silent FAILED log.
- Performance (after stability): PPA hardware acceleration, RAM art decode
  (PSRAM), adaptive poll backoff. TLS poll keep-alive is already done (#2.6).

## Look into next (noted 2026-05-26)

- **Playback control 403 = "Restricted device" — RESOLVED 2026-05-26 (environmental).**
  Writes (`play`/`next`/`previous`/`seek`) returned `403 {"message":"Restricted
  device"}` while the poll worked. NOT Premium, NOT scope, NOT a stale token
  (confirmed via the 403 body now logged in `spotify.c`). Cause: the active
  playback device was restricted (`is_restricted: true`) — for Lewis the **Sonos
  speaker and laptop** (Spotify Connect) are restricted; playing on the **iPhone**
  the controller drives playback fine. So control works on a non-restricted
  device; nothing to fix in code. Don't assume which device type is restricted —
  check the `is_restricted` flag (Sonos is a classic restricted device).
  Remaining OPTIONAL polish: (a) the device switcher below (transfer to a
  controllable device from the controller); (b) on a "Restricted device" 403,
  show an on-screen hint instead of the silent FAILED log.
- **Sonos direct control — DONE (transport/volume + album start), needs hardware verify.**
  `sonos.c` drives play/pause/next/prev/seek/volume over UPnP/SOAP (port 1400)
  when the active device is restricted; `main.c` routes by matching the active
  device name to `SONOS_DEVICES` (name->IP map in secrets.h; single `SONOS_HOST`
  also works). Transport/volume verified on two speakers across two Sonos systems.
  **Album start ON the Sonos is now implemented** (`sonos_play_spotify_album`):
  `BecomeCoordinatorOfStandaloneGroup` → enqueue the album cpcontainer
  (`x-rincon-cpcontainer:1004206cspotify%3aalbum%3a<id>` + DIDL whose cdudn carries
  the household Spotify service id `SONOS_SP_STYPE` / serial `SONOS_SP_SN`) → point
  the transport at the local queue (`x-rincon-queue:<uuid>#0`) → Play.
  `SCMD_PLAY_ALBUM` routes here whenever a Sonos is the active/selected target.
  **Fixed (needs hardware confirm):** the queue transport URI used `:0` instead of
  `#0`, so `SetAVTransportURI` returned UPnP 714 *after* the album was already
  enqueued (queue full, transport empty → Play 500). The cpcontainer-direct first
  attempt still 714s on this firmware — benign, it falls through to the queue path.
- **Active-device switching — DONE, needs hardware verify.** `spotify_get_devices()`
  + `spotify_transfer_playback(id)` added to `spotify.c` (with a JSON-array
  iterator); a DEVICES screen off the browser lists Spotify Connect devices
  (tap = transfer via `PUT /me/player`) and configured Sonos speakers
  (tap = drive over UPnP). Bonus: transferring to a desktop/Connect device also
  sidesteps the phone-volume limitation.
- **Cover Flow — show more covers either side.** Today only 1 shows each side
  because card slots are 248 px apart (2nd neighbour is off-screen). Needs a
  cover-flow-specific tighter slot spacing AND centre-on-top z-ordering; both
  touch the centre-snap math + the fragile image-transform path, so do it as an
  isolated, separately-verified change. Watch: LVGL negative `pad_column` support
  (if unhonored, `step` math desyncs from layout and breaks centring).
- **Aesthetic pass (retro-industrial / "TE" look).** Highest-impact first:
  functional colour-coded transport keys (e.g. prev=blue `#1270b8`,
  play=green `#1aa167`, next=yellow `#ffc003`, red `#ce2021` for active), keeping
  one accent for the progress bar; monospace tabular numerals for timestamps /
  volume % (caveat: a 2nd embedded TTF eats the ~4% app-flash headroom and
  re-opens the tiny_ttf kerning-crash surface); circular knob-style transport
  buttons; hairline section dividers. Start with the colour-coded keys (no new
  fonts, board-safe — colours only).
- **Album capacity is flash-bound.** UI cap `MAX_CARDS = 64` (currently 56). Real
  limit: thumbnails are embedded in the 8 MB app partition at 220x220 (~95 KB
  each) and it's ~96% full → only ~3-4 more before it won't build. Flash is
  32 MB with ~20 MB unused (8 MB app + 4 MB storage). Options: grow the app
  partition in `partitions.csv` (cleanest, ~85+ more albums), shrink thumbs
  (160x160 ~51 KB / 120x120 ~29 KB), or move thumbs to a data partition loaded at
  runtime. Raise `MAX_CARDS` alongside.
- **Confirm prev-button intent.** Prev (key + swipe-right) currently restarts the
  track if >3 s in, else goes to previous (Spotify-style). Confirm that's wanted
  vs. always-previous — the "slider jumps to start" the user saw was this seek-to-0
  (which also 403'd, so it didn't actually restart on the server).
