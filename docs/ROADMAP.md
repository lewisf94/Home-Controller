# Roadmap

**Lead build: `waveshare/esp-idf/`** (Waveshare ESP32-P4) — hardware-verified
end to end (2026-06-13). The `cyd/esp-idf/` build is feature-complete and was
hardware-verified on the CYD, now awaiting a re-flash; the PlatformIO and
CYD-IDF-HA builds are committed but not yet hardware-tested. See the build matrix
below.

---

## Current state (lead build: CYD ESP-IDF)

`cyd/esp-idf/` is the live product. Shipped and hardware-verified:
- Album browser (embedded RGB565 thumbnails, encoder scroll, touch scroll, tap-to-play)
- Now-playing screen (JPEG album art via LittleFS, title/artist/album, progress bar)
- Spotify Web API: auth/token refresh persisted to NVS, GET `/me/player`, play/pause/next/prev/seek/volume
- MCP23017 IO expander: RE1 encoder (scroll in browser, volume in now-playing), RE1-SW (mute/select), SW1–SW4 (prev/play/next/view-toggle)
- INTA interrupt on GPIO35 for button responsiveness; 30 ms debounce; latch-on-read event model
- Volume HUD (transient, shows "MUTED" when muted), WiFi bars (4-bar, 2 s refresh)
- OFFLINE indicator, empty-album message, MAX_CARDS warning, auto-snap to playing album, play-failure toast

Accepted hardware limits:
- Browser scroll tearing: ILI9341 TE pin not wired; no vsync available.

Known issues still open in the Arduino PlatformIO build (see Phase 1):

---

## Build matrix — 6 variants (3 platforms × 2 backends)

Each of the three hardware/toolchain iterations should ship in **two
backend flavours**: a **non-HA** build that talks to the Spotify Web API
directly, and an **HA** build that talks to Home Assistant over WebSocket
(see Phase 3 for the HA backend design). The UI/input layers are shared
within a platform; only the backend component (`spotify/` vs `ha_client/`)
swaps out.

| Platform | Non-HA (direct Spotify Web API) | HA (WebSocket to Home Assistant) |
|---|---|---|
| **PlatformIO CYD** (Arduino) | Shipped (Phase 1 + 1.5); now ported to LVGL, needs re-verify on hardware. | Not started. |
| **ESP-IDF CYD** | **Done — feature-complete and verified on hardware** (lead build). | First build exists (`cyd/esp-idf-ha/`), not yet hardware-tested. |
| **ESP-IDF P4** (Waveshare) | **Active — UI hardware-verified end-to-end (2026-06-13)**: cp1–3 + full UI (Cover Flow, four dark/light MODEs incl. GLYPH colour dot-matrix art, single-row colour accents). Open: RAM-art-decode (gated on an openRAM A/B check), Cover Flow memory-bandwidth perf, and a rolling code-quality audit — see `docs/P4-TODO.md` / `docs/PENDING.md`. | Future — copy of the P4 non-HA build with the backend swapped. |

**Status legend:** "Needs polish" = functional on hardware but has open
bugs / rough edges to clean up before it's a finished variant.

**How the two backends differ (applies to every platform):**
- *Non-HA:* `esp_http_client`/`WiFiClientSecure` HTTPS to Spotify; OAuth
  refresh-token flow; polls `GET /v1/me/player`; album art from Spotify CDN.
- *HA:* `esp_websocket_client` to `ws://<ha-host>:8123/api/websocket`; one
  static long-lived token (no OAuth refresh); real-time `state_changed`
  push (no polling); album art via HA-proxied `entity_picture` (local
  network, no TLS). Fixes the Spotify mobile volume limitation (1C).

**Selection mechanism (decide during Phase 3 build-out):** a compile-time
switch — e.g. a `BACKEND=spotify|ha` CMake/PlatformIO option (or a
`menuconfig` choice on IDF) that selects which backend component links in.
Keeps one source tree per platform rather than forked branches.

**Shared across all 6:** album-art conversion scripts (`scripts/`), album
list/metadata, the `current_track_info`-style state struct, and command
function signatures (`*_next_track`, `*_play_album`, etc.) so the UI never
needs to know which backend is compiled in.

**Polish backlog (pre-existing variants):**
- PlatformIO CYD non-HA: LVGL port needs a full hardware pass (colours/byte-order,
  touch, heap, TLS handshake); then clear the residual Phase 1 bug list.
- ESP-IDF CYD non-HA: done — feature-complete and verified. Remaining is the
  accepted browser scroll-tearing limit (no ILI9341 TE pin wired).
- ESP-IDF CYD HA: bring up `cyd/esp-idf-ha/` on hardware (WebSocket auth,
  state push, service calls, HA-proxied art).

---

## Phase 1 — Fix known CYD/Arduino bugs (do before or during IDF port)

These are confirmed root causes with clear fixes. Short work items.

### 1A — Album art blank on second visit to now-playing  [SHIPPED — needs verification]

**Root cause:** `jpeg_np` was a file-scoped `JPEGDEC` instance. JPEGDEC has a
confirmed library bug (GitHub issue #6 — "decode reset issue"): consecutive
calls to `open()` / `decode()` on the same object leave stale VLC buffer
pointers from the previous decode.

**Fix applied:** heap-allocate a fresh `JPEGDEC` (`new`/`delete`) per call in
`draw_now_playing()`. Stack-local was tried first but the object is too
large and overflows the loop task's default stack (caused SW4 hold to crash
the board).

### 1B — Serial debug flood causes encoder sluggishness  [SHIPPED]

**Fix applied:** all hot-path `Serial.printf` in `mcp_input_update()`
(`[INTA]` / `[POLL]` / `[HB ]` / `[EVT]` / `[ENC]`) now gated behind
`#define MCP_DEBUG`. Off in production builds. Re-enable for hardware
bring-up if needed.

### 1E — Now-playing visual issues  [PARTIAL — needs hardware test]

Three related issues observed once 1A's heap-allocated JPEG path was live.
Fixes applied in same commit as 1A, all need hardware verification:

1. **Album colours inverted** — JPEG decode path didn't set
   `tft.setSwapBytes(true)`. JPEGDEC outputs little-endian RGB565; ILI9341
   wants big-endian. Without the swap, red and blue channels swap.
   *Fix applied: wrap `decode()` in setSwapBytes(true)/setSwapBytes(false).*

2. **Now-playing flickers every ~2 s** — `draw_now_playing()` keyed its
   "full redraw" flag off `track_info_updated`, which the Spotify poller
   sets every 2 s for progress updates. Result: full `fillScreen` + JPEG
   re-decode every poll cycle.
   *Fix applied: track changes detected by comparing title+album strings
   against last-drawn values; full redraw only on actual track change.*

3. **Art rendered in bottom-right quadrant** — if `jpeg_np->getWidth()` or
   `getHeight()` returns 0 (corrupt / progressive / placeholder JPEG),
   `np_img_x` becomes `SCREEN_W/2` and JPEGDraw blocks render from screen
   centre toward bottom-right.
   *Fix applied: guard `if (w > 0 && h > 0)` before computing position;
   clamp top-left to on-screen; bumped large-image threshold so 480 px+
   images use JPEG_SCALE_QUARTER instead of HALF.*

4. **Browser blank when toggling back from now-playing** — `draw_album_browser`
   has a static `(last_drawn_scroll, last_drawn_view)` guard that
   short-circuits when nothing scrolled. After `ui_show_album_browser`
   calls `fillScreen(TFT_BLACK)`, that guard kept the screen black.
   *Fix applied: new `browser_needs_redraw` module flag set by
   `ui_show_album_browser` to bypass the guard once.*

All four fixes need a hardware run-through:
- Switch into now-playing → colours correct?
- Stay on now-playing 30 s → no flicker, no full re-decode every 2 s?
- Art centred and full-sized?
- Toggle back to browser → albums visible immediately?

### 1C — Volume PUT doesn't change phone volume  [LOW — Spotify limitation]

**Root cause:** Confirmed Spotify Web API behaviour.  
- Android: `volume_percent` in GET always returns 100; SET is silently ignored.  
- iOS: returns 403 "Player command failed: Cannot control device volume" on
  some configurations.  
- Works reliably on: Spotify desktop, web player, Spotify Connect hardware.

The `[VOL ] set X% → ok/FAILED` Serial line (added in latest commit) will
confirm whether the HTTP call fires and what Spotify returns. If "ok" but
phone volume is unchanged, this is a Spotify mobile client restriction, not
our code.

**Fix options:**
- Accept it — volume knob works when casting to a Spotify Connect speaker.
- Route volume through Home Assistant instead (Phase 3 — HA handles device
  targeting correctly and the limitation goes away).

### 1D — Spotify poll blocks loop every 2 s  [MEDIUM]

Every `spotify_fetch_player_state()` HTTP call (up to 2 s at current timeout)
blocks `mcp_input_update()`, causing encoder transitions generated during the
block to coalesce into a single read. Even with 1B fixed, this cap remains.

**Fix options (pick one):**
- Increase poll interval from 2 s to 4–5 s. Playback controls still fire
  immediately on button press; only the GET poller is lazier. One line change.
- FreeRTOS dual-core: pin Spotify HTTP on Core 0, UI + input on Core 1.
  Requires a mutex on `current_track_info`. CLAUDE.md flags this as opt-in.

Recommended: bump poll to 4 s first. If encoder still lags, do FreeRTOS split.

---

## Phase 2 — ESP-IDF port (same CYD hardware, same feature set)  [DONE — plus subsequent reliability/UX work]

**Status: cp0-11 complete and verified on hardware (the original port).** The
build then absorbed a substantial perf / reliability / UX / arch backlog from
the 05-24 / 05-26 / 05-27 / 05-28 / 05-30 / 05-31 daily reviews — TLS keep-alive,
adaptive poll backoff, 404 wake-on-play, WiFi background reconnect, MCP
re-probe, `_do_cmd` 401 handling, volume sync from device, MAX_CARDS log,
OFFLINE indicator, toast on play failure, auto-snap-to-playing-album, empty
list message, JPEG SOI check, shuffle toggle (SW4 long-hold in now-playing,
all four builds), "Nothing playing" initial UI state, HA build volume/shuffle
state parsing, HA command bool return + offline toast, etc. None of that has been
re-flashed yet (CYD board has been out of reach). See [`PENDING.md`](PENDING.md)
for the verify-pending list and [`TESTING.md`](TESTING.md) for the sanity-check menu.

**Architecture update:** the two CYD IDF builds (`cyd/esp-idf/` direct
Spotify and `cyd/esp-idf-ha/` Home Assistant backend) now share their UI /
input / MCP / album-art / LittleFS code via the
[`cyd/components/cyd_shared/`](../cyd/components/cyd_shared/) ESP-IDF
component — extracted in commits `2f7accd` + `1731a6a`. Only the backend
file differs between them (`spotify.c` vs `ha_client.c`). A fix to shared
code lands in both builds at once instead of needing hand-syncing.

The one accepted hardware limit is browser scroll tearing (no ILI9341 TE pin
wired). This is the lead build and the carrier for Phase 3.

**Goal (achieved):** identical product running on ESP-IDF instead of Arduino, as
the foundation for Phase 3 (HA integration). Phase 3 is implemented directly in
the IDF build.

**Why IDF now:**
- ESP-IDF is required for the ESP32-P4 migration later (separate project).
- HA integration via WebSocket (Phase 3) fits the IDF event-loop model
  naturally; it's awkward to retrofit into Arduino's blocking loop.
- LVGL (the display layer used in IDF) will be needed for the P4's
  MIPI-DSI display too — learn it once.

### Repo structure

When work starts, current files move into a subfolder and a new IDF project
lives alongside it:

```
Music-Controller/
├── cyd/                       # CYD board (ESP32-WROOM)
│   ├── platformio/            # Arduino build via PlatformIO, frozen/maintained
│   │   ├── src/  include/  platformio.ini ...
│   │   └── README.md
│   └── esp-idf/               # native ESP-IDF build
│       ├── main/
│       ├── components/
│       ├── sdkconfig.defaults
│       ├── partitions.csv
│       ├── idf_component.yml
│       └── CMakeLists.txt
├── waveshare/                 # ESP32-P4 build (planned)
├── docs/
│   ├── ROADMAP.md             (this file)
│   ├── TESTING.md
│   └── PORT-NOTES.md          (IDF gotchas as discovered)
├── scripts/                   # album-art conversion (shared)
├── CLAUDE.md
└── README.md
```

### Architecture decisions

**Display:** `esp_lcd_panel_io_spi` + ILI9341 managed component, backlight via
LEDC PWM on GPIO21.

**UI layer:** LVGL via `esp_lvgl_port`. Rationale: esp_lcd gives only
`draw_bitmap()` — no text, fonts, or JPEG without LVGL or a large custom
graphics lib. LVGL is the practical choice and reusable on P4 later.
LVGL draw buffers: two 320×20 px buffers in internal RAM (no PSRAM on CYD).

**Touch:** `esp_lcd_touch_xpt2046` managed component if mature; otherwise
~100-line custom SPI driver on SPI3_HOST (same pin mapping as today).

**MCP23017:** custom I2C driver (~150 lines) mapping directly from current
`mcp_input.cpp`. The gray-code state machine and debounce logic copy over
unchanged.

**Networking:** `esp_wifi` STA mode + `esp_http_client` for Spotify.
In Phase 3 this becomes `esp_websocket_client` to HA instead.

**JSON:** `bblanchon/ArduinoJson` IDF-native managed component. Keeps current
parsing code largely unchanged. Faster to port than switching to cJSON.

### IDF component manifest (`idf_component.yml`)

```yaml
dependencies:
  idf: ">=5.3.0"
  espressif/esp_lcd_ili9341: "^2.0.0"
  lvgl/lvgl: "^9.2.0"
  espressif/esp_lvgl_port: "^2.4.0"
  bblanchon/ArduinoJson: "^7.0.0"
```

Touch component added once XPT2046 maturity is confirmed.

### `sdkconfig.defaults`

```
CONFIG_FREERTOS_HZ=1000
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y
CONFIG_LV_COLOR_DEPTH_16=y
CONFIG_LV_COLOR_16_SWAP=y
CONFIG_LV_USE_JPG=y
CONFIG_FATFS_LFN_HEAP=y
```

No PSRAM — `CONFIG_SPIRAM` stays disabled.

### Migration phases (each is a flashable checkpoint)

| # | Goal | Done when |
|---|---|---|
| 0 | `idf.py create-project`; blink backlight; serial log | "hello" in monitor |
| 1 | esp_lcd + ILI9341; fill screen with colour cycle | Red → green → blue |
| 2 | LVGL init; "Hello CYD" label centred on screen | Text on display |
| 3 | XPT2046 driver; LVGL input device; drag a widget | Square follows finger |
| 4 | WiFi STA connect | IP address logged |
| 5 | HTTPS to Spotify; token refresh; GET player state | Track title logged |
| 6 | Download nowplaying.jpg; display via `lv_image` | Album art on screen |
| 7 | SD mount; load metadata.csv + bin thumbnails as raw RGB565 | Static browser grid |
| 8 | LVGL scrollable container + snap; tap → spotify_play_album | Tap plays album |
| 9 | MCP23017 I2C driver; LVGL encoder input device | Encoder scrolls browser |
| 10 | Button dispatch; volume debounce; mute toggle | All controls working |
| 11 | Feature parity: WiFi bars, volume HUD (with mute), OFFLINE indicator, empty-album message, auto-snap, toast | Matches Arduino LVGL build |

---

## Phase 3 — Home Assistant integration (on the IDF build)

**Goal:** replace the direct Spotify Web API calls with HA's media player
service running on a Pi 5 (Home Assistant OS). The device stops caring about
Spotify OAuth and device selection — HA owns that.

**Why HA instead of direct Spotify:**
- Volume control works reliably (HA handles device targeting; Android/iOS
  volume restriction goes away).
- No OAuth refresh logic on the device: one static HA long-lived access token,
  never expires.
- Works with any future music source (local files, Tidal, Apple Music) without
  changing device firmware.
- Real-time state push via WebSocket instead of polling every 2–4 s.

### Architecture

```
ESP32 CYD ──WebSocket──► Pi 5 (HA OS) ──Spotify Integration──► Spotify
                          (media_player.spotify_*)
```

**ESP32 → HA:** `esp_websocket_client` connected to `ws://pi5.local:8123/api/websocket`.

**Auth:** one static HA long-lived access token stored in NVS (written once
via serial command or hardcoded in `secrets.h` equivalent).

**State updates (inbound):** subscribe to `state_changed` events for the
Spotify media player entity. HA pushes every track change, play/pause, volume
change in real time. No polling.

**Commands (outbound):** call HA services via WebSocket:
- `media_player.media_play_pause`
- `media_player.media_next_track`
- `media_player.media_previous_track`
- `media_player.volume_set` (with `volume_level: 0.0–1.0`)
- `media_player.shuffle_set`
- `media_player.play_media` (for album browser: pass Spotify URI as `media_content_id`)

**Album art:** `entity_picture` in HA's state attributes is a relative URL
to the HA server (proxied from Spotify). Download it via `esp_http_client`
to `GET http://pi5.local:8123{entity_picture}`. No TLS required for local
network — faster than Spotify CDN direct.

### WebSocket handshake sequence

```
ESP32 connects → receives {"type":"auth_required"}
→ sends {"type":"auth","access_token":"<token>"}
→ receives {"type":"auth_ok"}
→ sends {"id":1,"type":"get_states"}
→ receives {"type":"result","id":1,"result":[...all entities...]}
→ sends {"id":2,"type":"subscribe_trigger",
          "trigger":{"platform":"state","entity_id":"media_player.xxx"}}
→ receives {"type":"result","id":2,"success":true}
→ ongoing: {"type":"event","id":2,
             "event":{"variables":{"trigger":{"to_state":{...entity state...}}}}}
```

`subscribe_trigger` (not `subscribe_events`) filters to a single entity
server-side so the firmware only receives frames for its own media player.
`get_states` seeds the initial track without waiting for the first change.

### HA setup required (on Pi 5)

See `docs/HA-SETUP.md` for the full step-by-step guide (Pi OS install,
Music Assistant, long-lived token, entity ID, first-run checklist,
troubleshooting). Short version:

1. Install Music Assistant add-on (recommended) or the native Spotify
   integration. Note the `media_player.*` entity ID.
2. Create a long-lived access token: Profile → Security → Long-Lived Access
   Tokens → Create. Copy immediately (shown once).
3. Assign the Pi a static IP or DHCP reservation.
4. Fill `HA_HOST` / `HA_PORT` / `HA_TOKEN` / `HA_ENTITY` in
   `cyd/esp-idf-ha/include/secrets.h`. Use the IP, not `homeassistant.local`.

### Migration within Phase 3

Build on top of the Phase 2 IDF build:
1. Replace `spotify/` component with `ha_client/` component.
2. `ha_client` opens WebSocket, handles auth handshake, subscribes to events.
3. State updates from HA feed directly into `current_track_info` struct
   (same struct as today).
4. Command functions (`spotify_next_track` etc.) become `ha_next_track` etc.,
   calling HA services instead of Spotify endpoints.
5. Album art URL becomes the HA-proxied URL — no Spotify CDN, no TLS.

UI and input code is untouched — they still read `current_track_info` and
call the same command functions, just from a different backend.

---

## ESP32-P4 migration (active — board in hand)

The Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3 (4.3" IPS, ST7701 MIPI-DSI,
GT911 capacitive touch, onboard ESP32-C6 WiFi) has arrived. The build lives in
`waveshare/esp-idf/` (direct Spotify, touch-first). **Checkpoints 1 (display),
2 (WiFi) and 3 (Spotify) are hardware-verified.** Everything since — the full
UI (cp4-7), Sonos integration (direct UPnP control + album-start + combined
device selector), Settings (Brightness + Theme + Accent + Browser Style +
Transition + Selection Line), and the full reliability/UX batch (auto-dim,
OFFLINE indicator, toast on play failure, auto-snap to playing album,
MAX_CARDS warning, etc.) — is committed but **needs a hardware verification
pass**. See [`PENDING.md`](PENDING.md) for the rolling list and
[`P4-TODO.md`](P4-TODO.md) for shipped one-liners plus what's still open.

On-chip memory budget measured at cp2 (~390 KB internal heap free + 31 MB PSRAM;
see `docs/PORT-NOTES.md`). See `waveshare/esp-idf/README.md` for the full
checkpoint roadmap (1 display → 2 WiFi → 3 Spotify → 4 UI → 5 assets →
6 controls → 7 parity).

### What the UI commit contains (needs hardware verify)

- Full LVGL browser (carousel, touch scroll, centre-snap) + now-playing
  (album art, title/artist, progress bar, volume HUD, WiFi bars) at 800×480.
- Three browser styles (Carousel / Focus / Cover Flow), NVS-persisted.
  - Cover Flow uses `lv_image_set_scale_x/y` + image recolor on child `lv_image`
    objects — the only safe per-scroll transform path on this board. Object-level
    `transform_scale` / `opa` forces layer snapshots that the DIRECT-mode rotated
    DSI flush mis-composites → progressive card blackout. Never regress to that.
  - `LV_USE_MATRIX` / `LV_DRAW_TRANSFORM_USE_MATRIX` produces negative X
    coordinates in the SW blender → store/load fault. Never enable.
- Settings screen: Menu Transition / Mode (Dark/Light) / Colour (accent) /
  Browser Style — all NVS-persisted.
- Colour accent system: Orange / Red / Green / Purple, drives highlights + progress
  bar. Separate from the Dark/Light neutral palette so all accents work with both.
- Charcoal palette, flat buttons (radius 3, no shadow), uppercase letter-spaced
  section headers.
- **Crash fix (tiny_ttf kerning cache):** `lv_tiny_ttf_create_data_ex(...,
  LV_FONT_KERNING_NONE, 128)` on all font instances. LVGL 9.4 kerning cache
  (upstream issue #6304) corrupts the heap under sustained scrolling; KERNING_NONE
  bypasses the cache entirely. Never use plain `lv_tiny_ttf_create_data` here.
  *(SUPERSEDED 2026-07: runtime tiny_ttf is now fully disabled on the P4 — its
  stb rasteriser also proved unstable. All text renders through compiled
  `lv_font_hc_*` fonts; see CLAUDE.md "Text crash prevention".)*
- JPEGDEC third-party warnings silenced via `CMakeLists.txt`
  `target_compile_options(${jpegdec_lib} PRIVATE -w)`.

### Already done (do not re-list as TODO)

- **TLS keep-alive on the poll:** the `/me/player` poll reuses a persistent
  `s_poll_client` (`.keep_alive_enable = true`), so the TLS session + cert bundle
  are negotiated once instead of re-handshaking every 5 s; `poll_client_close()`
  drops the handle on transport error so the next poll reconnects. Token refresh
  and playback commands stay one-shot by design (infrequent). Same in `cyd/esp-idf/`.

### Deferred — after hardware is confirmed stable

1. **RAM art decode:** waveshare has PSRAM — switch album art from the LittleFS
   round-trip to the existing `spotify_download_bytes` + `album_art_decode` RAM
   path, bypassing LittleFS entirely.

Already in, not TODOs: **PPA rotation** (the vendored BSP hardcodes
`.enable_ppa_accel = true`), **adaptive poll backoff** (5 s playing / 15 s
paused + 429 Retry-After holdoff), **TLS keep-alive** on both the poll and
command clients.

### Physical input — RP2040 haptic knob (firmware done, hardware pending)

The planned physical control is a custom **RP2040 SmartKnob-style daughterboard**
(FOC gimbal motor, strain-gauge press, 4 MX buttons, LED ring, ambient + battery
sensors) on a dedicated UART. The firmware is committed and **gated behind
`KNOB_ENABLED` (default off)**, so it does not affect knob-less P4 flashes:

- RP2040 firmware in `rp2040/` (SimpleFOC FOC on core 1, UART/sensors/LEDs on
  core 0); P4-side driver in `waveshare/esp-idf/main/knob.c` + `knob_input.c`.
- UART protocol: nanopb + CRC32 + COBS, with ACK/retry. Schema in `proto/`.
- `knob_input.c` drives only the backend-neutral `ui_*` seam, so it carries over
  to `waveshare/esp-idf-ha/` untouched.
- Hardware design: [`DESIGN_NOTES.md`](DESIGN_NOTES.md). Pin map, protocol, and
  SimpleFOC/MT6701 facts (deep-research-verified 2026-06-18): [`KNOB-NOTES.md`](KNOB-NOTES.md).
- Remaining: PCB in hand → flash `KNOB_ENABLED=1` → calibrate → verify the
  four-menu feel on device.

Key facts and adaptations:
- **Toolchain: ESP-IDF 5.5.x** (NOT 5.4, NOT 6.0). The vendored BSP needs the
  `usb` component (removed in 6.0) and its `esp_lvgl_adapter` needs IDF ≥5.5 —
  5.5.x is the only line satisfying both. The CYD builds stay on 6.0.
- Display: MIPI-DSI ST7701 via the vendored `esp32_p4_wifi6_touch_lcd_4_3` BSP
  (`bsp_display_start_with_config`), native 480×800 rotated to 800×480 landscape.
- LVGL via `esp_lvgl_adapter` (NOT `esp_lvgl_port`) — lock is
  `bsp_display_lock()/unlock()`; `ui.c`'s `lvgl_port_lock(0)` maps to that.
- Touch: GT911 capacitive (I2C), driven through the BSP.
- WiFi: `esp_wifi_remote` + `esp_hosted` (slave esp32c6, SDIO) — the C6 routes
  the `esp_wifi_*` API, so `wifi_init_sta` ports nearly unchanged.
- App logic (`spotify.c`, `albums.c`, `album_art.cpp`, `littlefs.c`,
  `album_thumbs.c`) copied unchanged from `cyd/esp-idf/`; UI re-laid-out for
  800×480; input is touch-first with a seam left for optional physical controls.
- **SRAM budget:** the full stack overflows internal SRAM by ~451 B at once, so
  sources/deps are staged per checkpoint (display-only at cp1).
- Backend: a future `waveshare/esp-idf-ha/` will swap to the Phase 3 HA client.

The `ha_client/` component from Phase 3 is the one piece that ports to P4
with zero changes.

---

## Parking lot / open questions

- **PSRAM on Lewis's CYD revision?** Not confirmed. If present, Phase 1D
  performance fix (cache all thumbnails at boot) becomes easy.
- **External I2C pull-ups (4.7 kΩ to 3.3 V) on SDA/SCL?** Recommended but
  not confirmed installed. Probe if I2C ever flakes.
- **Local audio via P4 onboard codec?** Future stretch — board has an audio
  codec. Could run librespot (Spotify Connect client) locally instead of
  being a remote control. Deferred well past Phase 3.
