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
- **PIXEL retro theme** — sixth MODE (cp10); Press Start 2P 1bpp font, Bayer-
  dithered art + thumbnails, dark-CRT palette. PSRAM thumb pool freed on
  switch-away. Needs hardware verify.
- **Code quality** — `_do_cmd` forward-decl + `spotify_play_album` keep-alive
  reuse; `MAX_DEVICES` constant; `scmd_meta_t` table + `_Static_assert`; `copy_str`
  consistency in `main.c`.

---

## Open — perf (after hardware verify)

The three items deliberately deferred until the board confirms the UI is
stable. Each is isolated; do them one at a time and re-flash between.

1. **PPA hardware acceleration** — `enable_ppa_accel = true` in
   `bsp_display_cfg_t`. The P4 PPA does the 90° rotation/blit in hardware
   (currently software every frame). One config line; should be a meaningful
   frame-rate win. Verify nothing regresses (cover-flow blackout was a
   layer-snapshot artefact of the rotation path, so this is the touchiest
   "should be safe" knob in the build).
2. **RAM art decode** — waveshare has PSRAM. Switch album art from the
   LittleFS file round-trip to the existing `spotify_download_bytes` +
   `album_art_decode` RAM path. Removes flash write/read for every track
   change, takes one I/O system out of the hot path.
3. **TLS keep-alive on commands** — currently the poll has keep-alive but
   playback commands each re-handshake. Mirroring the keep-alive pattern to
   commands cuts ~0.5-2 s + ~30 KB heap per press. (Lower payoff than (1) /
   (2); commands are infrequent.)

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
- **GetTransportInfo connect timeout (4 s) on Sonos probe** — the
  "is anything playing?" probe at boot occasionally times out and blocks
  the Spotify task for the full 4 s. Drop the timeout to 1.5-2 s
  specifically for the probe so a slow/off speaker can't stall the task
  that long. Pair with the adaptive-poll backoff work above.
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
