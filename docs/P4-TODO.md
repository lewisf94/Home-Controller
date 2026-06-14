# Waveshare ESP32-P4 — next-steps backlog

What's still planned for `waveshare/esp-idf/` after the cp1-7 + reliability/UX
batches. Ordered roughly by priority. Many of the items previously listed here
have shipped; for the rolling list of "shipped but not yet hardware-verified"
items, see [`PENDING.md`](PENDING.md).

---

## Shipped to date (one-liners — see commit history + per-build README for detail)

- **cp1-3 (display / WiFi / Spotify)** — hardware-verified.
- **cp4-7 (UI)** — full LVGL browser + now-playing, Settings (Menu Transition,
  Mode, Colour, Browser Style, Brightness, Selection Line), three browser
  styles (Carousel / Focus / Cover Flow), runtime tiny_ttf fonts (Montserrat +
  DejaVu) with kerning-cache crash fix, colour accent system, transport keys,
  scrub thumb. Committed; needs hardware verify.
- **Spotify TLS keep-alive on poll** — persistent `s_poll_client`, drops on
  transport error.
- **Adaptive poll backoff** — 5 s playing, 15 s paused/idle.
- **Sonos direct control** — transport, volume, and album-start over UPnP
  (port 1400); device selector merges Spotify Connect transfer targets with
  configured Sonos speakers; now-playing fallback reads UPnP `GetPositionInfo`
  when Spotify can't see the speaker. `x-rincon-queue:#0` separator fix
  applied.
- **Volume sync from device** — `device.volume_percent` parsed, fed through
  to the encoder/HUD/slider base.
- **404 wake-on-play** — `s_last_device_id` cached + transfer-and-play retry.
- **Settings brightness** — slider, persisted to NVS, applied at boot.
- **Auto-dim / sleep** — ramps backlight to 30 % at 60 s idle, 10 % at 5 min,
  restores on touch.
- **OFFLINE title** — title flips to "OFFLINE" when WiFi drops, restores on
  reconnect.
- **Generic toast** (`ui_show_toast`) — used by play-failure path so taps
  aren't silent no-ops.
- **Auto-snap browser to playing album** — accent-bordered match.
- **MAX_CARDS truncation** — on-screen warning when album list exceeds cap.
- **JPEG SOI marker check** — rejects non-JPEG bodies before decode.
- **WiFi background reconnect** — `esp_timer` retries every 20 s after fast
  retries exhaust (no more permanent dead-end after a router blip).
- **Dispatcher logging** — every failed Spotify command gets a named
  `ESP_LOGW` so silent presses are debuggable.
- **PIXEL retro theme** — a MODE (cp10); Press Start 2P 1bpp font, Bayer-
  dithered art + thumbnails, dark-CRT palette. PSRAM thumb pool freed on
  switch-away. Needs hardware verify.
- **GLYPH dot theme** (cp12) — replaced the Yudho/Fuhrer VFX backdrops (canvas
  particle system deleted) with one all-dots MODE: dot text + dotted-icon fonts,
  gas-tank progress bar (Brownian dots + playhead), dot WiFi meter. Font fixed in
  GLYPH. Cog reads a touch muddy at dot size — deferred.
- **UI sound + tabbed Settings** (cp13) — synthesised SFX via ES8311 (`audio.c`),
  sound sets + volume; Settings split into DISPLAY + SOUND tabs; scrolling long
  titles; Cover-Flow centre-tap fix; album-art `JPEGIMAGE` in internal SRAM.
- **Code quality** — `_do_cmd` forward-decl + `spotify_play_album` keep-alive
  reuse; `MAX_DEVICES` constant; `scmd_meta_t` table + `_Static_assert`; `copy_str`
  consistency in `main.c`.

---

## Open — perf (after hardware verify)

Deferred until the board confirms the UI is stable. Isolated; re-flash between.

1. **RAM art decode — GATED on an openRAM hardware check.** Switching album art
   from the LittleFS file round-trip to the `spotify_download_bytes` +
   `album_art_decode` RAM path would drop the `littlefs`+`vfs` deps and the 4 MB
   `storage` partition. BUT the file path is a deliberate workaround: JPEGDEC
   1.6.2's `JPEG_openRAM` mis-handles some of Spotify's mozjpeg covers (see
   `album_art.h`). The `ART_DECODE_RAM` compile flag in `main.c` (default 0)
   A/Bs it on hardware with LittleFS untouched — set =1, flash, confirm every
   cover decodes cleanly, THEN remove LittleFS. Do not remove it blind. (commit
   `9bdd80a`)

2. **Cover Flow scroll — memory-bandwidth bound (profile first).** `cf_render`'s
   unconditional per-frame full-canvas clear (473 KB) plus the column-major
   `cf_draw_col` writes (1600-byte stride vs a 128-byte L2 line, ~185 MB/s PSRAM)
   dominate — the per-pixel divide is secondary. `cf_render` now logs a
   clear-vs-rasterise breakdown when FPS display is on (`cf_profile_tick`, commit
   `99605ed`). Read those numbers before touching anything; the real levers are a
   cache-friendly rasterise (scratch-tile / row-major) and trimming the clear.
   CPU (360 MHz = P4 IDF ceiling), build flags (-O2, PSRAM 200 MHz, 256 KB L2)
   and PSRAM headroom (~70 %) are already maxed. Panel caps at 60 Hz, so the goal
   is holding 60 during a flick, not a higher number.

Done, not TODOs (verify on hardware, see PENDING.md): **PPA rotation** is
already enabled — the vendored BSP hardcodes `.enable_ppa_accel = true`
(`bsp_display_lcd_init`). **TLS keep-alive on commands** is in — `_do_cmd`
reuses the persistent `s_cmd_client` (keep_alive_enable), same as the poll.
The FPS-maxing batch also landed: achieved-frame-rate FPS counter, PSRAM
thumb pools (1:1 card blits, no flash XIP reads on scroll), GLYPH gas-tank
ticker frozen off-screen, and the EXPERIMENT `CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT=2`
(two-core SW render — revert that single sdkconfig line if hardware shows
artifacts).

---

## Code-quality deep-dive audits (2026-06-14, in progress — one at a time)

Read-only investigations of the current code, Lewis's request. Findings logged
to PENDING.md; fixes only on request.

- **A. Concurrency / lock discipline — DONE, CLEAN.** Lock discipline sound;
  command queue value-copied + non-blocking; album-art double buffer race-free
  (`s_art_buf` is single-task). Constraint: `spotify_task` must stay the sole
  writer of `s_track` / `s_art_buf` / `s_sonos_*`; future physical input posts to
  `s_cmd_queue`.
- **B. Failure-mode / resilience sweep — DONE.** Mostly graceful (WiFi reconnect,
  token refresh, 429 holdoff, 404 wake, OOM degrades with fallbacks). Real gap:
  an unreachable Sonos blocks `spotify_task` on 4 s SOAP timeouts — auto/restricted
  self-heals, an explicitly-picked one does NOT (~12 s/poll until device switched);
  see the expanded Sonos item under "Open — UX polish". Minor: Spotify 5xx no extra
  backoff; non-JPEG art retried not blacklisted; display-init/WiFi-wait returns
  unchecked (hardware-fault only). Two agent "criticals" were false positives.
- **C. Credential & TLS security posture** — pending. Token-in-NVS handling, cert
  bundle verification, no-leak logging, secrets gitignore across all builds.
- **D. Long-uptime heap / fragmentation** — pending. Pool churn on theme switches
  + TLS buffers over a multi-day run.
- **E. Multi-build drift / `app_core` consolidation** — pending. What's diverged
  across the 4 builds; the `app_core` refactor (see "Deferred architecture work"
  in PENDING.md).
- **F. Sonos UPnP/SOAP robustness** — pending. SOAP/DIDL parsing edge cases +
  device-routing logic.

---

## Open — UX polish

These would each be a small/medium PR.

- **Restricted-device 403 hint** — on a `403 Restricted device` write, show
  the existing toast ("Active device is restricted -- transfer first") so
  the silent FAILED log isn't the only feedback. Pattern is already in
  place (`ui_show_toast`); just thread it through.
- **Cover Flow show more covers either side** — today only 1 shows each side
  because card slots are 248 px apart (2nd neighbour is off-screen). Needs
  cover-flow-specific tighter slot spacing AND centre-on-top z-ordering.
  Both touch the centre-snap math + the fragile image-transform path -- do
  as an isolated, separately-verified change. Watch: LVGL negative
  `pad_column` support (if unhonored, `step` math desyncs from layout and
  breaks centring).
- **Aesthetic pass (retro-industrial / "TE" look)** — highest-impact first:
  functional colour-coded transport keys (prev=blue `#1270b8`, play=green
  `#1aa167`, next=yellow `#ffc003`, red `#ce2021` for active), keeping one
  accent for the progress bar; monospace tabular numerals for timestamps /
  volume % (caveat: a 2nd embedded TTF eats ~4 % app-flash headroom and
  re-opens the tiny_ttf kerning-crash surface); circular knob-style
  transport buttons; hairline section dividers. Start with the colour-coded
  keys (no new fonts, board-safe).
- **Sonos unreachable stalls the Spotify task (4 s SOAP timeouts)** — confirmed
  in the B resilience audit. The idle probe (`GetTransportInfo`, gated 10 s)
  blocks 4 s when a configured speaker is powered off; worse, once a Sonos is the
  active device, `sonos_fetch_now_playing` runs THREE 4 s queries back-to-back, so
  an unreachable active Sonos stalls `spotify_task` ~12 s per poll. An auto-probed
  / restricted Sonos self-heals (a failed fetch clears `s_sonos_active` + 10 s
  backoff, main.c:460), but an EXPLICITLY user-picked one (`s_sonos_explicit`)
  does NOT — it stalls every poll until the user switches device. Fixes: drop the
  SOAP `timeout_ms` to ~1.5-2 s (esp. the probe); add a short TCP connect timeout
  so a dead host fails fast; and after N consecutive failures clear/back-off the
  explicit-pick case so a powered-off pinned speaker can't stall forever.
- **First "place me at the playing album" on browser open** — auto-snap is
  in (`s_target_card`), but if the user opens the browser before any poll
  has matched an album the carousel sits at index 0. After the next match
  it scrolls — could feel jumpy. Tiny tweak: animate-OFF the snap when the
  browser isn't visible (already does this implicitly because the scroll
  request just updates internal state when not on screen, but worth
  double-checking on hardware).

---

## Open — physical controls (cp6 successor)

The Settings UI is touch-driven today; there's no MCP23017 on the waveshare.
The `input.c` seam left in `cyd_shared/input.c` could be re-used for a future
optional CYD-style hardware panel on the waveshare. Not committed to.

---

## Capacity ceiling

`MAX_CARDS = 64` on waveshare (currently ~56 albums). Real limit: thumbnails
are embedded in the 8 MB app partition at 220×220 (~95 KB each) and it's
~96 % full → only ~3-4 more before the build fails to link. Flash is 32 MB
with ~20 MB unused (8 MB app + 4 MB storage). Options when you hit it:

- **Grow the app partition** in `partitions.csv` (cleanest, ~85+ more albums
  possible). Raise `MAX_CARDS` alongside.
- **Shrink the thumbs** — 160×160 ~51 KB each / 120×120 ~29 KB each. Visual
  quality drops at small sizes.
- **Move thumbs to a data partition** loaded at runtime. Cleanest long-term
  but adds a startup load step.

---

## Open — questions for Lewis

- **Prev-button intent** — Prev (key + swipe-right) currently restarts the
  track if >3 s in, else goes to previous (Spotify-style). Confirm that's
  desired vs. always-previous. The "slider jumps to start" the user saw was
  this seek-to-0 (which also 403'd before the restricted-device finding —
  so it didn't actually restart on the server either).
- **Auto-dim thresholds** — currently 60 s → 30 %, 300 s → 10 %. Adjust if
  it feels too aggressive / too slow once on hardware.

---

## Out of scope for this build

- **Local audio via P4 onboard codec** — board has an audio codec. Could
  run librespot (Spotify Connect client) locally instead of being a remote
  control. Significant project, far past the current scope.
