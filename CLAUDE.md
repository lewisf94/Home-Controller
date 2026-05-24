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

Lewis builds with PlatformIO (Arduino) and native ESP-IDF. There are now four
build folders: the CYD ESP-IDF direct-Spotify build (`cyd/esp-idf/`, the lead
build — feature-complete and hardware-verified), its Home Assistant variant
(`cyd/esp-idf-ha/`, not yet tested), the original Arduino build now ported to
LVGL (`cyd/platformio/`, needs a hardware pass), and the new Waveshare ESP32-P4
direct-Spotify build (`waveshare/esp-idf/`, checkpoint 3 / Spotify verified). He works
locally in VS Code with Claude Code and intermittently uses Claude Code on the
web. This file is the source of truth across sessions.

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

Button functions are context-dependent on the active view (handled in the
`input` dispatcher, identical mapping in both builds):

| Input   | MCP pin      | Code constant      | Browser view            | Now-playing view |
|---|---|---|---|---|
| SW1     | GPA0 (pin 0) | `PIN_SW1`     = 0 | Scroll one album left   | Previous track (restart if >5 s in) |
| SW2     | GPA1 (pin 1) | `PIN_SW2`     = 1 | Select centred album    | Play / Pause |
| SW3     | GPA2 (pin 2) | `PIN_SW3`     = 2 | Scroll one album right  | Next track |
| SW4     | GPA3 (pin 3) | `PIN_SW4`     = 3 | Toggle view ↔           | Toggle view ↔ |
| RE1 CLK | GPA4 (pin 4) | `PIN_RE1_CLK` = 4 | Browser scroll encoder A | (volume A) |
| RE1 DT  | GPA5 (pin 5) | `PIN_RE1_DT`  = 5 | Browser scroll encoder B | (volume B) |
| RE1 SW  | GPA6 (pin 6) | `PIN_RE1_SW`  = 6 | Select centred album     | Mute toggle |

RE1 turn scrolls the album carousel in the browser and adjusts volume
(clockwise = louder) in now-playing.

RE2 (volume encoder + mute switch) is not fitted. `re2_get_delta()` and
`re2_sw_get_event()` are stub no-ops in `mcp_input.cpp`.

**Encoder decoding:** gray-code state machine indexed by `(last_ab << 2) | new_ab`,
inherently rejects single-pin glitches. RE1 uses bits {4,5} of Port A (GPA4/GPA5).
See `mcp_input.cpp:_update_encoder()`.

**Button debounce:** 30 ms stable-state per button. `event_pending` is a latch
set on the confirmed press edge and cleared only when the consumer reads it via
`btn_get_event()` — it is NOT cleared each tick. This matters because the
producer (`mcp_input_update`) and consumer run in different contexts: a press
made while the consumer is stalled in a blocking Spotify call must survive until
it's read. `btn_is_held()` returns the stable pressed state.

**INTA usage:** all inputs consolidated onto Port A (GPA0–GPA6). INTA → GPIO35,
push-pull, active-LOW. All seven pins have interrupt-on-CHANGE enabled so any
button press or encoder edge fires INTA immediately. Poll path retained alongside
interrupt path to keep debounce timers ticking. Port B unused.

---

## Architecture

The project spans four self-contained build folders: `cyd/esp-idf/` (lead),
`cyd/esp-idf-ha/`, `cyd/platformio/`, and `waveshare/esp-idf/`. Each is a
self-contained section below so they stay decoupled as work moves between them.

### CYD — ESP-IDF build (lead, feature-complete) — `cyd/esp-idf/`

ESP-IDF 5.x/6.0 + LVGL 9.5. Cooperative multitasking via three FreeRTOS tasks;
input is fully decoupled from rendering and network I/O, which is why the
encoder and buttons stay smooth even during blocking Spotify HTTPS calls.

```
app_main (main.c) ── init NVS/WiFi/LittleFS, bring up LVGL port + display + touch,
   │                 mount albums, create the task set and the command queue
   │
   ├── lvgl task (esp_lvgl_port) ── owns all rendering; LVGL objects only
   │                                touched under lvgl_port_lock()
   │
   ├── input_task (main.c) ── 2 ms loop: mcp_input_update() then, under the LVGL
   │     │                    lock, input_update(). Never blocks on the network.
   │     ├── mcp_input.c ── low-level MCP23017 driver (new IDF i2c_master API):
   │     │                  gray-code encoder state machine, button debounce
   │     └── input.c ── dispatcher: encoder/button events → ui_request_*() which
   │                    post typed scmd_t commands onto s_cmd_queue
   │
   └── spotify_task (main.c) ── drains s_cmd_queue (scmd_t) and runs the blocking
         │                      HTTPS calls off the LVGL/input path; also polls
         │                      /me/player and pushes state into ui.c
         ├── spotify.c ── Web API client (token refresh persisted to NVS, GET
         │                /me/player, POST /next /previous, PUT /play /pause
         │                /seek, volume); JSON via a small purpose-built scanner
         ├── ui.c ── LVGL album browser + now-playing + volume HUD
         ├── album_art.cpp ── JPEGDEC decode of now-playing art → LittleFS
         ├── album_thumbs.c ── embedded RGB565 browser thumbnails (EMBED_FILES)
         ├── albums.c ── album list / metadata
         └── littlefs.c ── internal-flash storage mount (album art)
```

Threading rule: LVGL is single-threaded — only the lvgl task and code holding
`lvgl_port_lock()` may touch LVGL objects. Cross-task work flows one way:
`input_task` → `s_cmd_queue` → `spotify_task`. No blocking call runs under the
LVGL lock.

### CYD — Arduino/PlatformIO build — `cyd/platformio/`

> **STATUS: LVGL port (Stage 1) committed but NOT yet hardware-verified.** The
> build compiles and fits (RAM 15.5%, Flash 55.9%) but has never been flashed.
> See "Where to look first → Current status" for what to test and what's
> deferred. The pre-LVGL TFT_eSPI direct-draw renderer was replaced wholesale.

Ported to **LVGL 9.5** so it shares the IDF build's look and behaviour (same
fonts, 120×120 embedded thumbnails, centre-snap carousel, slide transitions).
Three contexts mirror the IDF task model:

```
main.cpp ── LVGL glue (TFT_eSPI flush cb + XPT2046 touch cb + lv_tick),
   │        recursive lvgl mutex (lvgl_lock/unlock), scmd_t command queue,
   │        ui_request_*() posters
   │   ├── loop() (core 1) ── lvgl_lock(); lv_timer_handler(); unlock; input_update()
   │   ├── spotify_task (core 1) ── blocking HTTPS off the render path; drains the
   │   │                            command queue; publishes track info under the lock
   │   └── mcp_input_task (core 0) ── mcp_input_update() every ~2 ms
   │
   ├── ui.cpp ── LVGL UI ported from the IDF ui.c: album carousel, now-playing,
   │             volume HUD, WiFi-strength bars. All LVGL access under lvgl_lock().
   ├── input.cpp ── dispatcher: button/encoder events → ui_request_*() (enqueue,
   │                never blocks) + ui_scroll_browser / ui_play_centered_album
   ├── mcp_input.cpp ── low-level MCP driver (producer; portMUX-guarded state)
   ├── albums.cpp / album_thumbs.cpp ── static album list + embedded RGB565
   │                thumbnails (album_thumbs_data.c, generated from the IDF blob)
   ├── spotify.cpp ── Web API client (WiFiClientSecure, TLS verified against the
   │                  embedded CA bundle certs/x509_crt_bundle); same controls
   └── lv_conf.h (include/) ── LVGL config; found via -DLV_CONF_INCLUDE_SIMPLE + -I include
```

Threading rule (same as IDF): LVGL is single-threaded; the loop and the Spotify
task serialise through `lvgl_lock()`. `mcp_input_task` only touches the MCP
driver's own statics, never `current_track_info`. SD is no longer used by the UI
(thumbnails are embedded); dynamic now-playing art is deferred (Stage 2).

**`get_encoder_delta()` in main.cpp** is a thin wrapper around `re1_get_delta()`
kept so `ui.cpp` can call it without knowing about the MCP driver. This is a
deliberate compatibility shim from the MCP migration.

`ui_fancy_backup.cpp/h` are unused legacy files — safe to ignore.

### CYD — ESP-IDF HA build (Phase 3) — `cyd/esp-idf-ha/` — NOT hardware-tested

**First HA build exists but is unverified.** A self-contained copy of
`cyd/esp-idf/` with the Spotify Web API backend replaced by a Home Assistant
WebSocket client (`ha_client.c`) talking to a Music Assistant `media_player`
entity. HA OS on a Pi 5 owns the Spotify integration, eliminating on-device
OAuth refresh, fixing phone volume, and enabling real-time push state. The UI /
input / album code is a copy of `cyd/esp-idf/` — a fix to one must be applied to
the other. Secrets are `HA_HOST` / `HA_PORT` / `HA_TOKEN` / `HA_ENTITY` in
`include/secrets.h`. See `docs/ROADMAP.md` Phase 3 for the handshake and HA
setup. The backend is kept behind the `ui_request_*()` seam — swap the backend,
don't entangle it with the UI.

### Waveshare ESP32-P4 — ESP-IDF build (direct Spotify) — `waveshare/esp-idf/` — checkpoint 3 / Spotify verified

**Board in hand; brought up incrementally.** Waveshare
ESP32-P4-WIFI6-Touch-LCD-4.3 (ESP32-P4 RISC-V, 4.3" IPS, ST7701 MIPI-DSI, GT911
capacitive touch, onboard ESP32-C6 WiFi over SDIO, PSRAM, 32 MB flash). Talks
**directly to the Spotify Web API**; a future `waveshare/esp-idf-ha/` will swap
to the HA backend. **Checkpoints 1 (display), 2 (WiFi) and 3 (Spotify) are
hardware-verified** — a label renders at 800×480 landscape, the board associates
to WiFi via the onboard C6 (`esp_wifi_remote` + `esp_hosted` over SDIO) and pulls
a DHCP lease, and the Spotify task refreshes the OAuth token (cached in NVS),
validates the TLS cert bundle, and polls `/me/player` every 5 s (track logged to
serial). Memory budget verified on-chip at cp2 (~390 KB internal heap free +
31 MB PSRAM; see `docs/PORT-NOTES.md`). cp4 (UI port) is next.
- **Known cp3 inefficiency (not yet fixed):** `spotify.c` does
  `esp_http_client_init`/`perform`/`cleanup` per call, so every 5 s poll runs a
  full TLS handshake + cert-bundle validation (`Certificate validated` each
  poll). Reuse the client handle / enable keep-alive to persist the TLS session;
  watch TLS heap (CLAUDE flags it tight). Same pattern in the CYD build.

Key differences from the CYD IDF build:
- **Toolchain ESP-IDF 5.5.x** (NOT 5.4, NOT 6.0): the vendored BSP needs the
  `usb` component (gone in 6.0) and its `esp_lvgl_adapter` needs IDF ≥5.5.
- Display/touch via the **vendored BSP** `esp32_p4_wifi6_touch_lcd_4_3`
  (`components/`, not on the registry): `bsp_display_start_with_config()`,
  native 480×800 rotated to 800×480 landscape.
- LVGL via **`esp_lvgl_adapter`** (NOT `esp_lvgl_port`): lock is
  `bsp_display_lock()/unlock()` — `ui.c`'s `lvgl_port_lock(0)` maps to it.
- WiFi via `esp_wifi_remote` + `esp_hosted` (slave esp32c6, SDIO).
- App logic (`spotify.c`, `albums.c`, `album_art.cpp`, `littlefs.c`,
  `album_thumbs.c`) copied unchanged from `cyd/esp-idf/`; UI re-laid-out for
  800×480; input is touch-first (no MCP23017), with a seam for optional physical
  controls.
- **SRAM budget:** the full stack overflows internal SRAM by ~451 B at once, so
  sources/deps are staged per checkpoint (`main/CMakeLists.txt` comments).
  See `waveshare/esp-idf/README.md` for the checkpoint roadmap.

### Future: ESP32-P4 HA variant — not started — `waveshare/esp-idf-ha/`

A copy of `waveshare/esp-idf/` with the backend swapped to the Phase 3
`ha_client.c`. The HA client carries over untouched from the CYD HA build; do
not start until the P4 direct-Spotify build is verified on hardware.

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

### Known bugs open (as of latest commit)

Hardware-verified but not yet fixed. See `docs/ROADMAP.md` Phase 1 for
full analysis and fix options.

1. **Album art blank on second now-playing visit** — JPEGDEC library
   state-corruption bug on consecutive `open()`/`decode()` calls on the
   same instance. Fix: make `jpeg_np` a local variable inside the decode
   block (2-line change).
2. **Encoder less responsive than the IDF build (Arduino only)** — RESOLVED in
   code (pending hardware verification). `mcp_input_update()` now runs in its own
   `mcp_input_task` pinned to core 0, decoupled from the blocking
   `loop()` (Spotify HTTPS / redraws on core 1), so fast spins no longer drop
   quadrature steps. Required two supporting changes: button `event_pending`
   became a consume-on-read latch (was cleared every tick), and the shared state
   is guarded by a `portMUX` spinlock. See the Arduino architecture section.
3. **Volume PUT doesn't change phone volume** — Spotify API limitation on
   Android/iOS. Works on desktop / Spotify Connect speakers. Fix comes in
   Phase 3 (HA integration).

---

## Project roadmap (summary)

Full detail in `docs/ROADMAP.md`. Three phases:

1. **Phase 1 — Bug fixes (Arduino/CYD):** JPEG blank, serial flood, poll
   interval bump. Done concurrently with the IDF port.
2. **Phase 2 — ESP-IDF port (CYD hardware):** DONE — same hardware, same
   features, ESP-IDF 6.0 + LVGL 9.5. `cyd/esp-idf/` is feature-complete and
   verified on hardware; it's the lead build.
3. **Phase 3 — Home Assistant integration (on IDF build):** Pi 5 runs HA OS
   with Spotify integration. ESP32 speaks HA WebSocket instead of Spotify API
   directly. Eliminates OAuth refresh, fixes volume, enables real-time push
   state. A first build exists in `cyd/esp-idf-ha/` (not yet hardware-tested).
   See `docs/ROADMAP.md` Phase 3 for WebSocket handshake and HA setup.

ESP32-P4 migration: ACTIVE — the Waveshare board is in hand and
`waveshare/esp-idf/` (direct Spotify, ESP-IDF 5.5) has checkpoints 1 (display)
and 2 (WiFi) verified on hardware; cp3 (Spotify) is in progress. A future
`waveshare/esp-idf-ha/` carries the Phase 3 HA client over untouched.

---

## Coding conventions for this codebase

- **No emojis in code or commit messages** (Lewis preference)
- **No `// removed comments` / dead-code stubs** — delete cleanly
- **Magic-number layout values** are gathered at the top of `ui.cpp`; prefer
  reusing existing `#define`s over inlining new ones. New layout values should
  follow the `NP_*` / `BROWSER_*` naming
- **Spotify API calls are blocking** — every `spotify_*()` is HTTPS (IDF
  `esp_http_client`, Arduino `WiFiClientSecure`) and stalls its caller for
  0.5–2 s. Both builds run them on a dedicated Spotify task fed by a command
  queue; UI/input post `ui_request_*()` and never block. Keep new playback calls
  on that task, not on the render path.
- **Both builds now render with LVGL** (IDF 9.5, Arduino 9.5). The old TFT_eSPI
  direct-draw renderer (and its Sprite) is gone. Do not reintroduce direct-draw.
  Touch only LVGL objects under the LVGL lock (`lvgl_port_lock` on IDF,
  `lvgl_lock()` on Arduino). TLS heap headroom is still tight — keep an eye on it.
- **MCP polling runs in its own FreeRTOS task on both builds** (IDF: `input_task`;
  Arduino: `mcp_input_task` on core 0). The split is safe *only* because the task
  touches just the MCP driver's own statics (spinlock-guarded) and never
  `current_track_info` — all Spotify/UI mutation stays on the consumer
  (loop / Spotify task). Keep it that way: don't move `input_update()`,
  `spotify_*()`, or UI rendering into the polling task.
- **Album list is generated — never hand-edit `albums.c`/`albums.cpp`.** The
  single source of truth is `spotify-albums-list.txt` (repo root, gitignored —
  personal choices). `python scripts/gen_albums.py` regenerates all four album
  source files (3× `albums.c`, 1× `albums.cpp`), sorted by artist then title
  (leading "The"/"A"/"An" ignored). Each file carries a "GENERATED — do not edit"
  header. To change the list, edit the txt and rerun the script.
- **Browser thumbnails stay aligned via the same source.** `album_thumbs.bin` is
  indexed *positionally* by album order, so it must match `albums.c`.
  `scripts/embed_albums_idf.py` imports the sorted list from `gen_albums.py` and
  bakes the 120×120 RGB565 thumbs in that exact order, matching each album to a
  cover in `scripts/input_albums/` by Spotify id (`<id>.jpg`/`.png`; legacy
  descriptive filenames still accepted). Albums with no cover get a neutral
  placeholder tile so the blob never desyncs (the UI shows a coloured letter
  card). The `.bin` is gitignored (copyright art) — regenerate it locally after
  changing the list or adding a cover.

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

### Arduino build (cyd/platformio/)
```bash
cd cyd/platformio
pio run                        # build
pio run -t upload              # build + flash
pio device monitor -b 115200   # serial monitor
```

### IDF build (cyd/esp-idf/)
```bash
cd cyd/esp-idf
idf.py set-target esp32        # first time only
idf.py build                   # build
idf.py -p COM<X> flash monitor # flash + serial monitor
idf.py reconfigure             # after editing idf_component.yml
```

### General
```bash
git log --oneline -10          # recent history
```

---

## Where to look first

- **Plans for next phases:** `docs/ROADMAP.md`
- **What still needs to be tested on hardware:** `docs/TESTING.md`
- **IDF port gotchas discovered on hardware:** `docs/PORT-NOTES.md`
- **Lead build (direct Spotify, verified):** `cyd/esp-idf/`
- **HA backend variant (untested):** `cyd/esp-idf-ha/`
- **Arduino build (LVGL port, needs re-verify):** `cyd/platformio/`
- **Waveshare ESP32-P4 (direct Spotify, checkpoint 3 / Spotify verified):** `waveshare/esp-idf/`
- **Current status: MILESTONE — CYD fully working on ESP-IDF.** The ESP-IDF
  build is now feature-complete for the CYD hardware and verified smooth on
  device: display, LVGL 9.5, XPT2046 touch, WiFi STA, Spotify HTTPS (token
  persisted to NVS), album art, the LVGL album browser + now-playing UI, and
  **MCP23017 hardware controls — four buttons + RE1 rotary encoder — all
  responsive**. Input runs in its own 2 ms FreeRTOS task and posts typed
  commands to the Spotify task via a queue, so controls stay smooth during
  blocking HTTPS calls. The encoder snaps the carousel to the centred album per
  detent; RE1 turn in now-playing adjusts volume (clockwise = louder) with a
  300 ms-debounced PUT and a volume HUD; long titles ellipsise instead of
  overlapping the artist. Album art is stored in a 256 KB LittleFS partition on
  internal flash (`storage` label, 0x210000) — avoids SD/SPI/DMA conflicts.
  JPEGDEC `openFile` path (via VFS) scales 640×640 Spotify JPEGs to 160×160
  RGB565 and renders via `lv_image`. WiFi + Spotify credentials live in
  `cyd/esp-idf/include/secrets.h` (gitignored; template at
  `include/secrets.h.example`).
  - **Known hardware limit:** browser scroll tearing — the CYD's ILI9341 TE pin
    isn't wired, so there's no vsync to sync redraws to. Buffer-size and
    SPI-clock tweaks were tried and reverted (both degraded TLS heap / made it
    worse). Accepted as unfixable without hardware TE wiring.
  - **Next:** Phase 3 — Home Assistant integration (see Architecture →
    "CYD — ESP-IDF HA build" and `docs/ROADMAP.md` Phase 3).

- **Waveshare ESP32-P4 — checkpoints 1 (display) + 2 (WiFi) + 3 (Spotify) HARDWARE-VERIFIED.**
  The board arrived and `waveshare/esp-idf/` is being brought up incrementally.
  cp1 renders a centred label at 800×480 landscape; cp2 associates to WiFi via
  the onboard ESP32-C6 (`esp_wifi_remote` + `esp_hosted` over SDIO) and pulls a
  DHCP lease (`wifi_init_sta` ported from `cyd/esp-idf/main.c`); cp3 runs the
  Spotify task — OAuth token cached in NVS, TLS cert bundle validated, `/me/player`
  polled every 5 s with the track logged to serial (`scmd_t` command queue in
  place for cp4+ controls, nothing posts to it yet). Toolchain is
  **ESP-IDF 5.5.x** (NOT 5.4/6.0 — see the Architecture section and
  `waveshare/esp-idf/README.md` for why), display/touch via the vendored
  `esp32_p4_wifi6_touch_lcd_4_3` BSP, LVGL via `esp_lvgl_adapter`. Sources/deps
  staged per checkpoint in `main/CMakeLists.txt`.
  - **cp2 memory budget (measured on-chip):** 768 KB internal SRAM total; after
    WiFi there's ~390 KB internal heap free + 31 MB PSRAM. The WiFi link
    overflowed the fixed IRAM segment by ~2 KB — fixed with
    `CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM=n`. Display framebuffers (2.25 MB) live
    in PSRAM. The 256 KB Spotify response buffer + album art must be allocated
    from PSRAM at cp3/cp5. Full analysis + PSRAM-first policy in `docs/PORT-NOTES.md`.
  - **To build:** dot-source the IDF 5.5.4 PowerShell profile, then
    `idf.py set-target esp32p4` and `idf.py build flash monitor`. WiFi + Spotify
    creds go in `waveshare/esp-idf/include/secrets.h` (gitignored; template at
    `include/secrets.h.example`). The board enumerates as a CH343 USB-serial
    device (COM3/COM4 on Lewis's machine, depending on USB port).
  - **cp3 verified (Spotify):** boot log shows the OAuth token loaded from NVS,
    `esp-x509-crt-bundle: Certificate validated`, and `now playing: <artist> --
    <title> [.../... ms, playing]` every 5 s. SSID/IP are no longer logged (so
    serial output is shareable without redaction). Known inefficiency: a full TLS
    handshake per poll — see the Architecture section's keep-alive note.
  - **Next:** checkpoint 4 — UI. Port `cyd/esp-idf/main/ui.c`; map
    `lvgl_port_lock(0)` → `bsp_display_lock(0)`; re-lay-out constants for 800×480;
    wire the browser carousel + now-playing to the existing `ui_request_*()` seam.

- **PlatformIO LVGL port — committed but NOT YET HARDWARE-TESTED (needs Lewis to
  check on device).** The Arduino build was rewritten from TFT_eSPI direct-draw
  to LVGL 9.5 so it matches the IDF build (fonts, 120×120 embedded thumbnails,
  centre-snap carousel, slide transitions, volume HUD, WiFi bars). It compiles
  and fits (`pio run` green: RAM 15.5%, Flash 55.9%) but has **never been
  flashed** — display colours/byte-order, touch, runtime heap, the TLS
  handshake, and rendering are all unverified.
  - **To verify on hardware:** flash `cyd/platformio`, confirm it boots, the
    carousel renders + scrolls (touch *and* encoder), buttons work, Spotify
    connects over verified TLS, track info shows.
  - **Deferred (Stage 2):** dynamic now-playing album art (currently shows the
    played album's embedded thumbnail; real art needs a decode-to-RGB565-in-RAM
    path, no SD). **Orientation:** ships at `DISPLAY_ROTATION 1` (180° from the
    IDF build); flip to `3` in `main.cpp` to match IDF, then re-check touch.
  - **Generated build inputs (committed):** `src/album_thumbs_data.c` (from the
    IDF `album_thumbs.bin`) and `certs/x509_crt_bundle` (from the IDF
    `cacrt_all.pem` via `gen_crt_bundle.py`).
- **Security (this session):** PlatformIO TLS now verifies against the embedded
  CA bundle (was `setInsecure()`); token-endpoint bodies no longer logged on
  either build; `SPOTIFY_SETUP.md` points at gitignored `secrets.h`; `.cache`
  gitignored. Verified no credentials were ever committed/pushed. The IDF-side
  changes (WiFi bars in `ui.c`, log redaction in `spotify.c`, browser button
  remap in `input.c`) are also **not yet rebuilt/flashed**.
