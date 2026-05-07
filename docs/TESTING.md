# Hardware Verification Checklist

This is the punch-list to run after flashing the latest `main` to the CYD.
Tick items off as you go. If any fail, capture the symptom and we'll fix
before moving to the next phase.

Latest commit to verify against: `2256078` — "Add WiFi signal indicator;
stop reloading static art every frame".

---

## Phase 1 — UX feedback for new inputs

### 1A — Debounced volume

- [ ] Spin RE2 rapidly, ~10 clicks in <1 second
- [ ] **Expected:** UI updates the volume bar instantly each click; no UI
      stutter or freeze
- [ ] **Serial monitor:** `spotify_set_volume()` is called only **once**,
      ~300 ms after the last click — not 10 times
- [ ] Spin RE2 slowly (one click per second). Each click eventually fires its
      own HTTP call (300 ms after each)

### 1B — Volume HUD overlay

- [ ] Turn RE2 once. A horizontal bar + "VOL XX%" appears in the top strip
      (y=0–26)
- [ ] HUD auto-disappears after ~2 seconds
- [ ] When HUD disappears, the WiFi indicator (top-left) is restored
- [ ] When HUD disappears in now-playing, album text + chevron are restored
      (full redraw triggered)

### 1C — SW4 seek preview

- [ ] Be in now-playing view with a track playing
- [ ] Hold SW4 down (>500 ms)
- [ ] While holding, turn RE1 a few clicks each direction
- [ ] **Expected:** `SEEK +M:SS` (or `-M:SS`) appears just above the progress
      bar, in light-blue text
- [ ] Album browser does **not** scroll while SW4 is held (RE1 deltas are
      consumed by the seek state machine)
- [ ] Release SW4 → Spotify seeks by the displayed offset

### 1D — Mute indicator

- [ ] Press RE2-SW (the volume-encoder push)
- [ ] HUD shows "MUTED" in red for 2 s
- [ ] After HUD expires, a small persistent "MUTED" badge is in the top-right
      corner of now-playing
- [ ] Press RE2-SW again → unmutes; badge disappears; HUD shows volume bar
      restored to pre-mute level

### 1E — Play / pause flash

- [ ] Press SW2 (play/pause)
- [ ] **Expected:** A large ▶ triangle (or ⏸ pair of bars) appears centred on
      the album art for ~1.5 s, then fades (next vinyl/art draw covers it)
- [ ] Try toggling play state from the Spotify phone app — flash also fires
      (ui_update detects `is_playing` change regardless of source)

### Encoder-switch view toggle (existing, sanity check)

- [ ] Press RE1-SW → toggles between album browser and now-playing

### Track navigation buttons

- [ ] SW1 → previous track
- [ ] SW2 → play/pause toggle (also triggers 1E)
- [ ] SW3 → next track
- [ ] SW4 single click → +10 s seek (no hold)
- [ ] SW4 double click (within 300 ms) → -10 s seek

---

## Phase 1.5 — WiFi indicator + JPEG fix

### WiFi bars

- [ ] After WiFi connects, 4-bar icon appears in top-left (x=3, y=2 to y=14)
- [ ] Bars filled in white = active, dim grey = inactive
- [ ] At your normal desk position, expect 3–4 bars
- [ ] Disconnect router or move far away → bars drop within 5 s
- [ ] Volume HUD covers the WiFi bars; after HUD expires, bars are repainted
      correctly (no stale row of pixels)

### JPEG fallback gate (mostly invisible)

- [ ] Play a track whose album is **not** in your local SD cache (so it falls
      back to `nowplaying.jpg`)
- [ ] In serial monitor, observe SD activity. Before this fix you'd see ~30
      reads/sec. After, only one read at track-change
- [ ] No visual regression — the static album image still appears centred

---

## Edge cases worth poking

- [ ] Press RE2-SW while RE2 is mid-rotation → does the HUD show the right
      state?
- [ ] Spam SW2 rapidly — does the play/pause flash spam visibly, and does
      Spotify keep up?
- [ ] Hold SW4, turn RE1 to a seek offset > track duration → seek clamps to
      end, doesn't overshoot
- [ ] Hold SW4, turn RE1 backwards past 0 → seek clamps to 0
- [ ] Disconnect WiFi and try a button → fails gracefully, doesn't hang
      forever (Spotify call has a timeout)

---

## If anything fails

1. Note the failure mode (what you saw, what you expected, serial monitor
   output)
2. Open Claude Code in VS Code, paste the failure into chat, point at the
   relevant feature in this checklist
3. The session will read `CLAUDE.md` and `docs/ROADMAP.md` automatically and
   know the codebase

When everything above is ✅, you're clear to start Phase 2 (`docs/ROADMAP.md`
§ Phase 2).
