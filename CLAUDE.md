# CLAUDE.md — Project Memory for Claude Code

This file gives a fresh Claude Code session full project context. Read it before
making changes. If you change architecture or hardware, update this file.

---

## Project overview

**Music-Controller** — A handheld Spotify controller built around the
"CYD" (Cheap Yellow Display, ESP32-WROOM with 2.8" ILI9341 + XPT2046 touch).
It shows an album browser loaded from an SD card, drives playback via the
Spotify Web API, and has a custom hardware control panel with two rotary
encoders and four push buttons routed through an MCP23017 I2C IO expander.

The user (Lewis) builds with PlatformIO. He works locally in VS Code with
Claude Code and intermittently uses Claude Code on the web. This file is the
source of truth across sessions.

---

## Hardware

### Board: CYD (ESP32-WROOM, ILI9341 320×240 landscape, XPT2046 resistive touch)

`platformio.ini` declares `board = esp32dev`. PSRAM is **not** currently
enabled. Enabling PSRAM is a Phase-2-stretch item (see `docs/ROADMAP.md`,
Option D under album-browser perf).

### TFT pins (TFT_eSPI build_flags)

| Function | GPIO |
|---|---|
| MISO | 12 |
| MOSI | 13 |
| SCLK | 14 |
| CS   | 15 |
| DC   | 2  |
| RST  | -1 (tied) |
| BL   | 21 |

`tft.setRotation(1)` — landscape, 320 W × 240 H.

### Touch (XPT2046, separate HSPI bus)

| Function | GPIO |
|---|---|
| IRQ  | 36 |
| MOSI | 32 |
| MISO | 39 |
| CLK  | 25 |
| CS   | 33 |

Touch coordinates are smoothed with a 3:1 IIR in `main.cpp:get_touch_coords()`.

### SD card

CS = GPIO 5 (`SD.begin(5)` in `main.cpp`). Default VSPI bus. Holds:

- `metadata.csv` — album list (filename, title, artist, spotify URI)
- `*.bin` — pre-converted RGB565 album thumbnails (80×80, 12,800 bytes each)
- `nowplaying.jpg` — JPEG fallback art for now-playing view

### I2C bus summary

The Wire bus runs on GPIO 27 (SDA) and GPIO 22 (SCL) at 400 kHz.
External 4.7 kΩ pull-ups to 3.3V on both lines are recommended.

| Component | Breakout | I2C address | Address pins |
|---|---|---|---|
| MCP23017 IO expander | CJMCU-2317 | `0x20` | A0=A1=A2=GND |

Only one I2C device currently on the bus. If a second is added (e.g.
an SSD1306 OLED or BME280 sensor), verify its address doesn't clash
with `0x20`. MCP23017 address can be changed to `0x21`–`0x27` by
lifting A0/A1/A2 to 3.3V in different combinations.

### MCP23017 IO expander (CJMCU-2317)

I2C address `0x20` (A0=A1=A2 grounded). RESET pin tied to 3.3V (always
active — not software-controlled). ESP32 talks to it over the **default
Wire bus**, repurposing the GPIOs that were once used for the original
direct-wired encoder.

| Signal | ESP32 GPIO |
|---|---|
| SDA  | 27 |
| SCL  | 22 |
| INTA | 35 (input-only, no internal pull-up; MCP drives it push-pull active-LOW) |

External pull-ups on SDA/SCL recommended (4.7 kΩ to 3.3V) — ESP32 internals
are too weak for reliable I2C.

### Inputs (all on the MCP23017, active LOW with internal pull-ups)

| Input   | MCP pin      | Code constant in `src/mcp_input.cpp` | Function |
|---|---|---|---|
| SW1     | GPA0 (pin 0) | `PIN_SW1`     = 0 | Previous track |
| SW2     | GPA1 (pin 1) | `PIN_SW2`     = 1 | Play / Pause   |
| SW3     | GPA2 (pin 2) | `PIN_SW3`     = 2 | Next track     |
| SW4     | GPA3 (pin 3) | `PIN_SW4`     = 3 | Seek (single = +10s, double = -10s, hold + RE1 = manual scrub) |
| RE1 CLK | GPA4 (pin 4) | `PIN_RE1_CLK` = 4 | Browser scroll encoder A |
| RE1 DT  | GPA5 (pin 5) | `PIN_RE1_DT`  = 5 | Browser scroll encoder B |
| RE1 SW  | GPA6 (pin 6) | `PIN_RE1_SW`  = 6 | View toggle (browser ↔ now-playing) |

RE2 (volume encoder + mute switch) is not fitted. `re2_get_delta()` and
`re2_sw_get_event()` are stub no-ops in `mcp_input.cpp`.

**Encoder decoding:** gray-code state machine indexed by `(last_ab << 2) | new_ab`,
inherently rejects single-pin glitches. RE1 uses bits {4,5} of Port A (GPA4/GPA5).
See `mcp_input.cpp:_update_encoder()`.

**Button debounce:** 30 ms stable-state per button; `event_pending` fires for
exactly one `mcp_input_update()` tick on confirmed press edge. `btn_is_held()`
returns the stable state (used for SW4 hold detection).

**INTA usage:** all inputs consolidated onto Port A (GPA0–GPA6). INTA → GPIO35,
push-pull, active-LOW. All seven pins have interrupt-on-CHANGE enabled so any
button press or encoder edge fires INTA immediately. Poll path retained alongside
interrupt path to keep debounce timers ticking. Port B unused.

---

## Architecture

```
main.cpp ── setup wiring, mounts SD, polls touch, runs main loop
   │
   ├── mcp_input.cpp ── low-level MCP driver: encoders, buttons, debounce
   │
   ├── input.cpp ── high-level dispatcher: encoder/button events → Spotify calls
   │                + state for mute, SW4 seek, debounced volume
   │
   ├── ui.cpp ── all rendering (album browser, now-playing, HUD overlays)
   │
   ├── spotify.cpp ── Spotify Web API client (token refresh, GET /me/player,
   │                  POST /next, /previous; PUT /play, /pause, /volume,
   │                  /seek, /shuffle)
   │
   └── app.cpp ── WiFi setup, top-level wiring
```

**`get_encoder_delta()` in main.cpp** is a thin wrapper around `re1_get_delta()`
kept so `ui.cpp` can call it without knowing about the MCP driver. This is a
deliberate compatibility shim from the MCP migration.

`ui_fancy_backup.cpp/h` are unused legacy files — safe to ignore.

---

## What's been shipped (Phase 1 + 1.5)

All on `main`. Latest commit: `Add WiFi signal indicator; stop reloading static
art every frame`.

### Phase 1 (commit `86cde31`)

1. **Debounced volume (1A)** — RE2 turns no longer block the UI. The local
   `current_volume_pct` updates instantly; the HTTP PUT fires once after 300 ms
   of encoder inactivity. See `input.cpp` static vars `last_vol_change_ms` /
   `vol_pending`.
2. **Volume HUD (1B)** — `ui_show_volume_hud(pct, muted)` in `ui.cpp`. Top-strip
   overlay (y=0–26), 200×6 px bar + percentage, auto-hides after 2 s. Shows
   "MUTED" red text when muted. HUD expiry triggers `np_needs_full_redraw` (now
   playing) or a strip clear + WiFi-icon repaint (browser).
3. **SW4 seek preview (1C)** — While SW4 is held > 500 ms and RE1 turns,
   `SEEK +M:SS` is drawn above the progress bar. Getters `input_sw4_seek_active()`
   and `input_sw4_seek_offset_ms()` are called from `draw_now_playing()`.
4. **Mute badge (1D)** — Persistent small "MUTED" label in red top-right of
   now-playing. Tracked separately from the transient HUD via
   `input_is_muted()`. State change triggers a one-shot redraw of a 60×14 px
   region.
5. **Play/pause flash (1E)** — `ui_update()` watches `current_track_info.is_playing`;
   on change, sets `play_flash_ms = millis()`. `draw_now_playing()` overlays a
   ▶ triangle or ⏸ pair of bars (filled via `fillTriangle` / `fillRect`) over
   the art for 1.5 s.

### Phase 1.5 (commit `2256078`)

1. **WiFi signal indicator** — 4-bar icon top-left (x=3, y=2 to y=14). Polled
   every 5 s via `WiFi.RSSI()`; only redraws when bar count changes
   (thresholds: -55/-65/-75/-85 dBm). Disconnected = 0 bars.
2. **JPEG fallback gate** — In vinyl mode without local cache, the JPEG was
   re-decoded from SD ~30×/sec for zero visual benefit (it can't rotate). Now
   gated on `initial_draw || last_square_state != np_show_square_art` only.

### Awaiting hardware verification

These are all in `main` but not yet tested on physical hardware. See
`docs/TESTING.md` for the verification checklist.

---

## Coding conventions for this codebase

- **No emojis in code or commit messages** (Lewis preference)
- **No `// removed comments` / dead-code stubs** — delete cleanly
- **Magic-number layout values** are gathered at the top of `ui.cpp`; prefer
  reusing existing `#define`s over inlining new ones. New layout values should
  follow the `NP_*` / `BROWSER_*` naming
- **Spotify API calls are blocking** — every `spotify_*()` is HTTPS via
  `WiFiClientSecure` and stalls the loop for 0.5–2 s. New calls should be
  rate-limited or debounced where the user might rapid-fire them
- **Keep `ui.cpp` direct-draw** — the Sprite was removed for TLS heap headroom.
  Don't reintroduce it without a hardware-verified plan
- **Don't fork the MCP driver into a FreeRTOS task** without explicit ask —
  shared-state hazards with `current_track_info`

---

## Commit / push policy

- Commit author should be **Lewis** (already in `git config user.name`),
  **not "Claude"**
- **Do NOT include the `https://claude.ai/code/session_…` footer** in commit
  messages
- Push to `main` directly is fine for solo work; create a branch only if the
  user asks
- Never `--no-verify` and never force-push without explicit instruction

---

## Useful local commands

```bash
pio run                        # build
pio run -t upload              # build + flash
pio device monitor -b 115200   # serial monitor
git log --oneline -10          # recent history
```

---

## Where to look first

- **Plans for next phases:** `docs/ROADMAP.md`
- **What still needs to be tested on hardware:** `docs/TESTING.md`
- **Current pending decisions:** none — Phase 1 + 1.5 done, Phase 2 plan
  exists, awaiting hardware verification before starting Phase 2
