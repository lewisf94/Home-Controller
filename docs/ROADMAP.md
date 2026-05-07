# Roadmap

Phased improvement plan for the Music-Controller project. Phase 1 + 1.5 are
shipped (see `git log` and `CLAUDE.md`). Phase 2 + 3 are not yet started.

---

## Phase 2 — Rendering performance + UI polish (NEXT)

Goal: eliminate the album-browser scroll lag and fix small rendering rough
edges. No visual redesign yet — that's Phase 3.

### 2A — Album-browser scroll performance (the big one)

**Root cause:** `CACHE_SLOTS = 1` in `ui.cpp` means only one 80×80 RGB565
thumbnail (12.5 KB) lives in RAM at a time. Three albums are visible in the
browser simultaneously, so `drawAlbumArt()` is forced to `loadAlbumImage()`
and SD-read **two extra thumbnails per frame** during scrolling. Fast scroll
= constant 12.5 KB SD blocking reads. SD reads are ~5–15 ms on a fast card,
worse on a slow one — this is the dominant cost.

Code references:
- Cache definition: `ui.cpp:92-95`
- Cache loader: `ui.cpp:375-419` (`loadAlbumImage`)
- Browser draw loop: `ui.cpp:485-494` (calls `drawAlbumArt` per visible album)

**Recommended approach: A + B + C combined.**

#### Option A — Increase cache slots from 1 → 3

- `#define CACHE_SLOTS 3`
- Each slot is 12.5 KB → +25 KB heap at boot
- Steady-state covers all visible albums → zero misses when scroll is parked
- The existing `if (ESP.getFreeHeap() < 20000)` heap-guard at `ui.cpp:102`
  needs raising slightly (or kept dynamic — bail per-slot rather than blanket)
- **Risk:** TLS handshake to `api.spotify.com` needs ~25 KB transient. Watch
  free-heap printout in serial after this change. Easy to roll back to 2 slots
  if needed
- **Effort:** ~5 LoC

#### Option B — Defer SD loads while scroll velocity is high

- In `ui_update()`, track scroll velocity (Δ`scroll_pos` per frame)
- While velocity > threshold, draw a placeholder colored rect (or a dimmed
  thumbnail of the last cached album) for any non-cached visible album
- Only call `loadAlbumImage()` for non-cached albums when velocity drops below
  threshold (i.e., user has stopped flicking)
- **Pro:** scrolling feels instant regardless of catalog size; UI never blocks
- **Con:** during fast scroll the placeholders look ugly. Mitigate with a
  subtle gradient or 1-px outline so it reads as "loading"
- **Effort:** ~30 LoC, all in `ui.cpp`

#### Option C — Preload neighbors on scroll-stop

- After scroll settles (velocity = 0 for ~150 ms), opportunistically call
  `loadAlbumImage(centerIndex - 1)` and `loadAlbumImage(centerIndex + 1)`
- Cheap; only useful with `CACHE_SLOTS >= 3`
- **Effort:** ~10 LoC

#### Option D — Stretch: enable PSRAM, cache all albums

- Most CYD revisions ship with 4–8 MB of PSRAM but it is **not enabled** in
  the current `platformio.ini` (just `board = esp32dev`)
- 100 albums × 12.5 KB = 1.25 MB → fits trivially
- Pre-load every thumbnail at boot into PSRAM; SD becomes a one-shot indexer
- **Pro:** zero SD reads after boot. Album browser is butter-smooth at any speed
- **Con:** requires PSRAM-enabled board variant (`board_build.arduino.memory_type
  = qio_opi` or similar). Risk of breaking the build until partition/board
  config is right. Need to identify exact CYD variant first
- **Effort:** ~1 hour of build-config + ~40 LoC of preload logic
- **Decision pending:** verify PSRAM presence on Lewis's specific CYD before
  committing to this path

### 2B — Album-browser redraw gating

Currently `draw_album_browser()` clears the album band on every scroll-pos
change. Combined with the SD reads above, this is wasteful. Once 2A lands,
verify the redraw is still gated correctly by `last_drawn_scroll` (see
`ui.cpp:476-478`).

### 2C — Vinyl angle quantisation

`current_rotation_angle` increments by `dt * 0.000628` rad/ms in `ui_update()`.
The vinyl draw is gated on `abs(angle - last_rendered_angle) > 0.02`
(`~1.1°`). Quantising to 5° steps cuts the `drawPixel` redraw-loop work
roughly 5× while remaining visually smooth at 30 fps cap. See `ui.cpp` near
`last_rendered_angle`.

### 2D — Long-text ellipsis

Album / track / artist strings longer than ~24 chars overflow the bottom of
now-playing and clip into the progress bar. Add a helper that truncates with
"…" once string width exceeds available pixel budget. TFT_eSPI has
`tft.textWidth()` — use it.

### 2E — Paused / nothing-playing state

When Spotify returns no active device or `is_playing=false` for an extended
period, draw_now_playing renders the last known track ghost-state. Add an
explicit "Nothing playing" centred-text state that takes precedence.

### 2F — Progress-bar edge cases

- Clamp to 100% on track end (currently underflows the wrap)
- Reset cleanly across track changes (currently relies on `progress_ms <
  last_prog` heuristic — fine, but worth a comment)

### 2G — Magic-number cleanup

Several layout numbers in `draw_now_playing()` are still inline (e.g., 213,
218 for the seek-preview region). Extract to `#define`s in the layout block
near the top of `ui.cpp`.

### Phase 2 verification

1. Scroll through full catalog at maximum encoder speed → UI never blocks > 50 ms
2. Free heap after Spotify auth completes is still > 30 KB
3. Long album / artist / track names display with "…" not clipped
4. Pause Spotify on phone → "Nothing playing" appears within 4 s
5. Track end → progress bar shows 100% briefly, not 99% or wraparound

---

## Phase 3 — Minimalist / Apple-style visual refresh (LATER)

Aesthetic goal: high contrast, generous whitespace, smooth easing, clean type
hierarchy. Don't start until Phase 2 is shipped and the perf baseline is good.

### 3A — Colour palette

| Token | Hex | RGB565 | Usage |
|---|---|---|---|
| `BG`            | `#000000` | 0x0000 | Backgrounds |
| `TEXT_PRIMARY`  | `#FFFFFF` | 0xFFFF | Titles, primary readouts |
| `TEXT_SECONDARY`| `#8E8E93` | 0x8C71 | Album, artist, hints |
| `ACCENT`        | `#0A84FF` | 0x041F | Apple-blue progress fill, active states |
| `ACCENT_ALT`    | `#1DB954` | 0x1DC9 | Spotify-green alternative if user prefers |
| `WARN`          | `#FF453A` | 0xFA28 | Mute badge |

Gather these as `#define`s at the top of `ui.cpp`.

### 3B — Rounded album art

- Software corner mask (8 px radius) drawn after `drawLocalAlbumArt`
- Same effect on now-playing square art
- Implementation: precompute the corner-pixel offsets at startup, paint them
  black post-draw

### 3C — Pill-shaped progress bar

- Replace `tft.drawRect` outline + `tft.fillRect` fill with a wider bar (10 px
  tall instead of 6) and rounded caps drawn via `fillCircle` at each end
- Fill colour = `ACCENT`, track = `TEXT_SECONDARY` dimmed

### 3D — Larger now-playing art

- Centre-fill layout: art up to 160×160 (currently 120×120)
- Move title down 10 px to compensate

### 3E — Improved metadata hierarchy

Currently:
- Top: album (small, grey)
- Centre: art
- Bottom-low: title (medium, white)
- Bottom-lower: artist (small, grey)

Apple-music-like ordering instead:
- **Title** (Font4, white, large, top of metadata stack)
- **Artist** (Font2, secondary)
- **Album** (Font2, secondary, smallest)

### 3F — HUD redesign

Current HUD is a 200×6 bar + text on a hard black strip. Refresh to:
- Pill-shaped 220×8 bar with rounded caps
- Centred percentage on top, bar below
- Subtle bottom drop-shadow line in `TEXT_SECONDARY` to separate from background

### 3G — SF-style typography (within constraints)

TFT_eSPI's bitmap fonts are limited (Font2 = 16 px body, Font4 = 26 px title,
Font6 = 48 px digits). No custom glyphs without flash budget. Use the largest
sizes for titles, smallest for hints.

### Phase 3 verification

Visual review only — Lewis decides when it looks right.

---

## Open questions / parking lot

- **PSRAM on Lewis's CYD revision?** Need to identify board exactly before
  Phase 2 Option D
- **FreeRTOS Core 0 task for blocking Spotify HTTPS?** Major architectural
  change, deferred indefinitely. Current debounce + optimistic local updates
  cover the worst UX pain
- **External pull-up resistors on SDA/SCL?** Recommended (4.7 kΩ to 3.3V) but
  not yet confirmed installed. Worth a probe if I2C ever flakes
- **Track-change detection** currently relies on `track_info_updated` flag
  toggled by `spotify_fetch_currently_playing()`. Consider hashing
  title+album to detect changes more robustly if the same track restarts
