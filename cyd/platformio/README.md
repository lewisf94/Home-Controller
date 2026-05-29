# cyd/platformio — Arduino/PlatformIO build (Phase 1, frozen)

The original PlatformIO + Arduino-framework build of the Music Controller. Reached feature parity at the end of Phase 1.5 and is now in maintenance mode. Active development has moved to [`../esp-idf/`](../esp-idf/) for the IDF rewrite, but this folder remains the working firmware until the IDF build reaches feature parity at Step 11 of the Phase 2 plan.

**Status:** frozen — bug fixes only. No new features. Pull from the open-issues list in [`../../docs/ROADMAP.md`](../../docs/ROADMAP.md) Phase 1 if you want to chip at something here.

---

## What's running on it (Phase 1 + 1.5)

Everything below is verified on hardware and lives on the `main` branch.

**Album browser**
- SD-card-driven grid of 80×80 RGB565 thumbnails (pre-converted by [`../../scripts/`](../../scripts/))
- Scroll with RE1 (one detent ≈ one row)
- Tap a tile to start that album playing
- Layout constants gathered at the top of `src/ui.cpp` under `BROWSER_*` defines

**Now-playing screen**
- JPEG album art (`nowplaying.jpg` on SD), decoded via JPEGDEC
- Title / artist / album / progress bar
- Vinyl-style toggle between square art and a round "label" view
- Persistent mute badge top-right when muted
- Play/pause flash overlay for 1.5 s when state changes

**Spotify Web API client (`spotify.cpp`)**
- `WiFiClientSecure` HTTPS with the Spotify root CA bundled
- Refresh-token OAuth flow (one-time setup via [`../../SPOTIFY_SETUP.md`](../../SPOTIFY_SETUP.md))
- Player-state poll every 2 s
- play / pause / next / prev / seek / shuffle / volume endpoints

**MCP23017 input (`mcp_input.cpp`)**
- I2C on the default Wire bus (SDA 27, SCL 22 @ 400 kHz; address `0x20`)
- Gray-code state machine for RE1 (CLK on GPA4, DT on GPA5) — inherently rejects single-pin glitches
- 30 ms stable-state debounce for SW1..SW4
- INTA → ESP32 GPIO 35 (active-LOW, interrupt-on-CHANGE on all Port A pins) for low-latency event delivery; poll path runs alongside to keep debounce timers ticking

**HUD overlays (`ui.cpp`)**
- Volume bar with percentage at the top of the screen, auto-hides after 2 s of encoder inactivity
- WiFi signal indicator (4-bar icon, polled every 5 s via `WiFi.RSSI()`, only repaints when bar count changes)
- SW4 hold + RE1 → manual scrub preview (`SEEK +M:SS`) above the progress bar

**Debounced volume (`input.cpp`)**
- RE2 turns update local state instantly; the HTTP `PUT /me/player/volume` fires once after 300 ms of encoder inactivity. Keeps the UI responsive even though the HTTPS call blocks the loop for 0.5..2 s.

---

## Hardware target

CYD (ESP32-2432S028R or compatible clone): ESP32-WROOM, 2.8" ILI9341 SPI TFT (landscape 320×240), XPT2046 resistive touch on a second SPI bus, on-board SD slot. Plus an external CJMCU-2317 (MCP23017) breakout for the physical control panel.

Pin tables, I2C addresses, and per-input mapping live in [`../../CLAUDE.md`](../../CLAUDE.md) — that's the source of truth. Don't duplicate them here.

---

## Build / flash / monitor

This is a standard PlatformIO project. Open this directory in VS Code with the PlatformIO extension installed, or use the CLI:

```bash
cd cyd/platformio
pio run                          # build
pio run -t upload                # build + flash
pio device monitor -b 115200     # serial monitor
pio run -t clean                 # nuke build artefacts if things look stale
```

The PlatformIO toolbar buttons (build / upload / serial monitor) map to those commands.

### First-time setup

1. Install PlatformIO (or open this folder in VS Code with the PlatformIO extension — it'll prompt to install everything).
2. Create [`include/secrets.h`](include/) with your WiFi credentials and Spotify OAuth tokens. The exact format is documented in [`../../SPOTIFY_SETUP.md`](../../SPOTIFY_SETUP.md). This file is gitignored.
3. Populate the SD card. Output of [`../../scripts/`](../../scripts/) goes at the SD root: `metadata.csv` plus one 12,800-byte `.bin` per album (80×80 RGB565), plus `nowplaying.jpg` as the now-playing fallback art.
4. Plug the CYD in over USB. Identify its COM port (`pio device list`). PlatformIO auto-detects on most systems; if not, set `upload_port` in `platformio.ini`.
5. `pio run -t upload` — builds and flashes. First build takes a few minutes (downloads framework + libraries); subsequent ones are seconds.

### Project structure

```
src/                         Main application source
  main.cpp                   setup() / loop(), SPI bus init, SD mount, touch read
  app.cpp                    WiFi setup, top-level wiring
  ui.cpp                     All rendering: browser, now-playing, HUDs, overlays
  input.cpp                  High-level dispatcher: encoder/button -> Spotify calls
  mcp_input.cpp              Low-level MCP23017 driver: encoders, buttons, debounce
  spotify.cpp                Spotify Web API client over HTTPS
include/
  secrets.h                  WiFi + Spotify credentials (gitignored)
  pins.h                     Pin defines pulled from CLAUDE.md
  ...                        Other shared headers
lib/                         Local libraries (currently empty)
test/                        PlatformIO tests (none yet)
platformio.ini               Board, framework, build flags, library deps
```

### Build flags worth knowing about

`platformio.ini` sets `board = esp32dev` and configures TFT_eSPI via `build_flags`. The TFT pins (MISO/MOSI/SCLK/CS/DC/RST/BL = 12/13/14/15/2/-1/21) and 40 MHz pixel clock are baked in there. PSRAM is **not** currently enabled — enabling it is a stretch item under Phase 2 album-browser perf (see `docs/ROADMAP.md` Option D).

---

## Known issues (Phase 1 — still open)

Confirmed root causes with clear fixes; pick one if you want a short PR. Full analysis in [`../../docs/ROADMAP.md`](../../docs/ROADMAP.md) Phase 1.

1. **Album art blank on second visit to now-playing.** JPEGDEC library state-corruption bug on consecutive `open()` / `decode()` calls on the same `JPEGDEC` instance. The fix in `src/ui.cpp:draw_now_playing()` is to make `jpeg_np` a local heap allocation per call (a stack local is too large for the loop task's default stack and crashes SW4 hold).
2. **Encoder feels sluggish on fast spin.** `mcp_input_update()` hot-path `Serial.printf` calls flood the TX FIFO and block the loop ~60 ms per event. Gate all `[INTA]` / `[POLL]` / `[HB]` / `[EVT]` / `[ENC]` prints behind `#define MCP_DEBUG`. Already done.
3. **Volume PUT doesn't change phone volume.** Confirmed Spotify Web API limitation on Android / iOS — `volume_percent` GET always returns 100 and SET is silently ignored. Works on desktop / Spotify Connect speakers. Fixed in Phase 3 by routing volume through Home Assistant instead.

---

## Coding conventions (this folder)

- No emojis in code or commit messages
- No `// removed comments` / dead-code stubs — delete cleanly
- UI magic numbers gathered at the top of `ui.cpp`; reuse existing `NP_*` / `BROWSER_*` defines rather than inlining new constants
- Spotify HTTPS calls are blocking — every `spotify_*()` stalls the loop for 0.5..2 s. New API calls must be rate-limited or debounced if the user might rapid-fire them
- Keep `ui.cpp` as direct-draw (the off-screen Sprite was removed for TLS heap headroom). Don't reintroduce it without a hardware-verified plan
- Don't fork the MCP driver into a FreeRTOS task without explicit ask — there are shared-state hazards around `current_track_info`

---

## Project memory

Hardware details, full architecture, and the rationale behind decisions live in [`../../CLAUDE.md`](../../CLAUDE.md). The phased plan is in [`../../docs/ROADMAP.md`](../../docs/ROADMAP.md).
