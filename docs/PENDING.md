# Pending — verification debt + deferred work

What's been written but not yet flashed/verified, plus the architecture work
that's been deliberately deferred. Keep this current as items get verified or
landed.

---

## Hardware-verify pending

### Waveshare ESP32-P4 (board in hand) — VERIFIED on hardware (2026-06-13)

**Cleared.** The whole stack below — everything on `main` past commit `42405ab`
(Sonos + device selector) through the 2026-06-13 theme restructure (four
dark/light MODEs, the colour accent picker, `ui_tune.h`) and the later THEME-label
/ GLYPH colour dot-matrix art / single-row COLOUR tweaks — is now flashed and
verified on device. The table is kept as the manifest of what landed; the
pre-flash reminder + sanity-check checklists that used to follow it have been
retired now that they're executed. One standing caveat survives:
`CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT` MUST stay `=1` — the `=2` dual-draw-unit
experiment (`dee8651`) boot-loops against the BSP's PPA acceleration and was
reverted (see `sdkconfig.defaults`).

| Area | What landed | Commits |
|---|---|---|
| Sonos | Album playback + combined device selector (Spotify Connect transfer + Sonos UPnP); `x-rincon-queue:#0` separator fix so `SetAVTransportURI` returns 200 | `42405ab` |
| UX — brightness | Settings slider (10-100 %), LEDC PWM via the BSP, persisted to NVS, re-applied at boot | `540df95` |
| Perf | TLS keep-alive on the `/me/player` poll + adaptive 5 s/15 s backoff; filtered JSON parse + persistent client on Arduino | `db5501b` |
| Code quality | `MAX_CARDS` log, mcp_input timing `#define`s, dead code removal, `ui_fancy_backup` deletion | `f212164` |
| Reliability — F1 | Removed per-tick `event_pending` clear in shared `mcp_input.c` (button presses during LVGL-lock timeouts were silently dropped) | `0157ffd` |
| Reliability — F2 | Background `esp_timer` WiFi reconnect after fast retries exhaust | `0157ffd` |
| Reliability — F3 | `s_art_url_failed` tracker so a malformed cover isn't re-downloaded every 5 s | `0157ffd` |
| Reliability — F4 | `xQueueCreate` failure halts cleanly instead of crashing on `xQueueReceive(NULL)` | `0157ffd` |
| Reliability — F5 | `esp_littlefs_info` return checked + distinct warning on failure | `0157ffd` |
| Reliability — 404 wake | `s_last_device_id` cached; `spotify_toggle_play_pause` retries via `PUT /me/player` on 404 | `2e69ee6` |
| Reliability — dispatcher | Named `ESP_LOGW` on each failed spotify_task command (no more silent presses) | `2e69ee6` |
| Reliability — MCP re-probe | Split `_configure_mcp` so MCP missing at boot retries every 5 s instead of disabling controls forever | `2e69ee6` |
| Arch | `cyd_shared` component extraction (waveshare unaffected) + `spotify_track.h` → `player.h` rename | `2f7accd` + `1731a6a`, `db576fd` |
| UX — bucket A | Empty album list message; volume HUD guard before first poll; JPEG SOI marker check | `49732e1` |
| UX — bucket B | On-screen `MAX_CARDS` warning; OFFLINE indicator on WiFi drop; generic `ui_show_toast` + play-failure toast; auto-snap browser to playing album + accent border | `aa7405b` |
| UX — bucket C | Auto-dim/sleep (waveshare only — CYD deferred); ramps `bsp_display_brightness_set` down at 60 s / 300 s idle, snaps back on touch | `434d3ea` |
| Reliability — code review (2026-05-31) | WiFi init-fail no longer early-returns (UI + reconnect timer come up regardless of initial connect failure); 401 token-clear in `_do_cmd`, `spotify_play_album`, `spotify_get_devices` (consistency with the poll path) | `25d2ab8` |
| PIXEL retro theme | Sixth MODE option (cp6): 1bpp Press Start 2P font, Bayer-dithered pixelated art + thumbnails, dark-CRT palette. `lv_font_pixel_16/24.c` added; font accessors route through `is_pixel_theme()`; PSRAM thumb pool (~0.5 MB) freed on switch-away. | `570c7a3` |
| Code quality (2026-06-02) | `_do_cmd` forward-declared so `spotify_play_album` can route through it (keep-alive reuse); `MAX_DEVICES` replaces 5 magic `16`s across `main.c`/`ui.c`/`ui.h`; `scmd_meta_t` table + `_Static_assert` replaces fragile exclusion chain; `strlcpy` replaces `copy_str`; finite `bsp_display_lock(1000)` timeout; progress-timer drift fix. | `479c986` + `85324e9` |
| GLYPH dot-matrix theme | Replaced the Yudho/Fuhrer VFX-backdrop themes (whole `lv_canvas` particle system deleted) with one MODE drawn entirely in round dots: dot text font from unscii-8 (`lv_font_dot_20/24/28.c`), dotted sparse-cmap FontAwesome fallback (`lv_font_dot_sym_*`), gas-tank progress bar (Brownian dots + playhead), 4-dot WiFi meter. Font fixed in GLYPH (FONT setting hidden). Fixed the browser-style-change crash (dangling WiFi-dot pointer across screen rebuild). | this session |
| UI sound + tabbed Settings | Synthesised SFX via ES8311 (`audio.c`): TICK/SELECT/BACK/CONNECT on a task+queue, named sound sets + AUTO, user volume (square-law taper), NVS-persisted. Settings split into DISPLAY + SOUND tabs. | this session |
| Title marquee | Long browser/NP titles scroll horizontally (`LV_LABEL_LONG_SCROLL_CIRCULAR`, fixed ~150 s/loop) instead of ellipsising; short titles stay centred. | this session |
| Cover-Flow centre-tap | Tap within `CENTRE_TAP_TOL` of screen-centre plays the centred album (touch X via `lv_indev_get_point`); off-centre tap scrolls that cover in. | this session |
| Settings cog icon | Settings button uses `LV_SYMBOL_SETTINGS` (cog), replacing the easy-to-miss faders glyph; renders in GLYPH via the dotted symbol font. *(first on-device check done; in GLYPH the dotted cog reads a touch muddy — dots merge — noted, fix deferred.)* | this session |
| Album-art decode crash | `JPEGIMAGE` working struct allocated in internal SRAM (`heap_caps_calloc(MALLOC_CAP_INTERNAL)`) not PSRAM — fixes intermittent `JPEGDecodeMCU` store fault. *(first on-device check done — needs a multi-track soak.)* | this session |
| FONT setting | Settings → FONT → SANS (Montserrat) or SLAB (Arvo Bold, OFL). NVS-persisted. PIXEL overrides to Press Start 2P and GLYPH overrides to the dot font regardless. | `c64f543` |
| Cover Flow 3D perspective | Replaced image-scale CF with a PSRAM column rasteriser (800×244 RGB565, 390 KB). Per-column perspective math produces true trapezoid foreshortening; three-pass draw guarantees centre-card z-order. Dissolve-animation cast bug fixed (`anim_set_bg_opa` wrapper). | `70812a0`, `c79aea4`, `5dd44a4` |
| CI — GitHub Actions | ESP-IDF build workflow (`.github/workflows/esp-idf-build.yml`) runs `idf.py build` on GitHub's servers (which can reach the Espressif registry) on every push to `waveshare/esp-idf/`. Placeholder `secrets.h` and zero-filled `album_thumbs.bin` generated before build. Currently green. | `9d92764`–`c79aea4` |
| PPA hardware acceleration | `.enable_ppa_accel = true` in vendored BSP; offloads 90° software rotation to the P4 hardware 2D accelerator. | verified |
| Cover Flow 2-side | CF card slot 220→180 px, gap 28→16 px (step 248→196). Card ±2 centres at 792/8 px — fully on-screen. Squash rate 150→100, floor 70→85, dim_rise 150→80 so second side card still reads. | verified |
| FPS display | Settings → FPS DISPLAY toggle (ON/OFF); live `N FPS` label in browser top bar updated every 1 s via `LV_EVENT_FLUSH_READY` counter. NVS-persisted. | verified |
| FPS-maxing batch (2026-06-10) | FPS counter reworked to **achieved frame rate** (RENDER_READY burst accounting — includes handler/rasterise/flush time); PSRAM thumb pools (raw ~5.4 MB feeds CF/PIXEL/pool builds, 286 px card-native ~9.2 MB makes Carousel/Focus centre blits 1:1); GLYPH gas-tank ticker frozen while now-playing is off-screen. (The EXPERIMENT `CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT=2`, one SW draw unit per core, was REVERTED — it boot-loops against PPA accel; setting stays `=1`.) | `89c3816`, `7af90de`, `f9a0337`, ~~`dee8651`~~ |
| PAPER theme | Sixth MODE slot: teletype data-brutalism — cream paper + ink, unscii-8 mono fonts (`lv_font_mono_16/24.c`), 1-bit Bayer-dithered art/thumbs, printed-form frames + rules, ruler-tick progress with ink block cursor, OUTPUT/LEVEL data fields, album index counter, inverted title chips, ink-framed keys/cards, TELEX typewriter sound set. | this session |
| GLYPH Nothing-light rework | GLYPH flipped to the Nothing-OS equaliser reference: light warm-grey ground + black ink; dot-matrix font for HEADINGS only (body/icons clean Montserrat — retires the muddy dotted cog); hairline outline pills, selected = solid ink + light text (`opt_sel_bg/fg`); gas tank = hairline capsule with ink dots + accent playhead; WiFi/volume dots in ink; dissolve dots in ink (were white); accent ring on colour swatches now theme-text (was invisible white on light themes). Unused `lv_font_dot_20/28` + `dot_sym_20/28` deleted. | this session |
| Theme restructure (2026-06-13) | Flat 6/7-theme enum collapsed to four MODEs (BASIC/GLYPH/PIXEL/PAPER) each with a DARK/LIGHT face + APPEARANCE toggle (`k_mode_palettes[MODE][2]`, NVS `ui_mode`/`ui_dark`); THEME ALBUM ART on/off toggle (`ui_themeart`); COLOUR grid grown to an 8-hue × 3-variant 24-swatch wheel (default deep orange, contrast-aware check); PAPER/Focus album frames baked into the cover pixels so they scale with the art; tap-to-toggle remaining/total timecode; visible GLYPH volume fader; `main/ui_tune.h` tweak-knob header; `gen_albums.py` ASCII-folds non-ASCII titles. | `d16094f` |

### Waveshare — committed/researched 2026-06-14 (Claude Code session)

Code committed (needs hardware verify):

| Area | What landed | Commits |
|---|---|---|
| Docs | Waveshare README caught up with PAPER theme, GLYPH rework, six MODEs, CF FPS-note fix; PPA/poll-backoff marked done | `990aff9` |
| Perf — CF profiler | `cf_render` logs a clear-vs-rasterise per-frame breakdown over serial when FPS display is ON (`cf_profile_tick`, gated on `s_fps_enabled`, zero cost otherwise). Flash, turn FPS on, flick Cover Flow, read the `cf_prof:` lines. | `99605ed` |
| Perf/Arch — `ART_DECODE_RAM` A/B | Compile flag in `main.c` (default 0 = LittleFS file path). Set =1 to decode album art straight from a PSRAM buffer (`spotify_download_bytes` + `album_art_decode`), LittleFS still mounted — validates whether `JPEG_openRAM` handles Spotify's mozjpeg covers before the LittleFS dependency is cut. | `9bdd80a` |

Investigations (read-only; findings folded into CLAUDE.md / P4-TODO.md):

- **Performance review.** Only `littlefs`+`vfs` are worth dropping (the RAM-decode path), GATED on the openRAM A/B above. JPEGDEC / esp_http_client / esp_codec_dev are load-bearing. CPU is already at 360 MHz (P4 IDF ceiling + unset default), build config optimal (PSRAM 200 MHz, -O2, 256 KB L2), panel ~60 Hz, PSRAM ~70 % free. Cover Flow scroll is **memory-bandwidth bound** (column-major `s_cf_buf` writes, 1600-byte stride, 128-byte L2 line, ~185 MB/s PSRAM) — the per-pixel divide is secondary; the unconditional 473 KB per-frame clear is pure overhead. Profile before restructuring.
- **Concurrency / lock-discipline audit — CLEAN.** All `ui_*` seam functions self-lock; the command queue is value-copied + non-blocking; the album-art double buffer is race-free (`s_art_buf` is read/written only on `spotify_task`, never by the render task, which only reads the lock-published `s_art_dsc->data`). An agent flagged an art-swap-order "race" — verified a FALSE POSITIVE. Standing constraint: keep `spotify_task` the sole writer of `s_track` / `s_art_buf` / `s_sonos_*`; a future physical-input task must post to `s_cmd_queue`, not touch that state.

Deep-dive queue (one at a time, Lewis's request): **A concurrency — DONE (clean).** **B failure-mode/resilience — DONE:** mostly solid (WiFi reconnect, token refresh, 429 holdoff, 404 wake, OOM-degrades-with-fallbacks all good). Real gap (now FIXED, `56e1a9b`): an unreachable explicitly-pinned Sonos (`s_sonos_explicit`) stalled the poll on a SOAP timeout every cycle — one timeout (~4 s), not the three the audit first claimed (`sonos_fetch_now_playing` bails after the first query); an auto/restricted Sonos already self-heals (clears `s_sonos_active` + 10 s backoff, main.c:460). Fix: 2 s SOAP timeout + a 10 s `s_sonos_fetch_hold` so a dead pinned speaker is retried every 10 s instead of stalling every poll. Minor: no extra backoff on Spotify 5xx (keeps normal poll cadence — not a hot-spin); non-JPEG art body retried not blacklisted; display-init + WiFi-wait return unchecked (hardware-fault only). Two agent "criticals" were FALSE POSITIVES: queue-create `return`s before `xTaskCreate` (no NULL-queue task), and `_post_cmd` NULL-guards the queue. **C security — DONE (clean):** no secret logged, all 6 HTTPS clients verify certs (`crt_bundle_attach`), secrets gitignored, parsing bounded; plaintext-NVS access token is informational (no flash encryption; the long-lived refresh token is not persisted). **D heap — DONE (clean):** no leaks / double-frees; theme pools freed-before-realloc; only nit is the per-poll malloc/free of the Spotify response buffer (could be a persistent reused buffer, but the IDF allocator coalesces — safe for multi-day). **E multi-build drift — DONE:** `cyd_shared` holds UI/input/MCP/art/FS; `spotify.c` + `main.c` are per-build (~290 L of WiFi/queue/art-loop dup → the `app_core` refactor already in "Deferred architecture work"). Real drift to port when the CYD is back: 429 Retry-After holdoff + `RESP_INITIAL_CAP=16K` are waveshare-only (absent from `cyd/esp-idf/spotify.c`). The internal-SRAM JPEGIMAGE fix is absent from `cyd_shared/album_art.cpp` but MOOT on the current CYD (no `CONFIG_SPIRAM` — `calloc` already lands in internal SRAM); only port it alongside the CYD-PSRAM Phase-2 item. **F Sonos SOAP — DONE:** no crash bugs — divide-by-zero is guarded at every progress site (`ui.c:2300/3817/4064`, verified false positive), the in-place unescape is overflow-safe, buffers are bounded. Minor: an 8 KB DIDL truncation only re-validates the title (duration/artist could silently blank), no range-check on the H:MM:SS/volume parse (garbage-but-no-crash), and the outgoing DIDL isn't XML-escaped (Spotify URIs never contain `&`/`<`). Across all six audits, every agent-flagged "critical" was verified and several were cleared as false positives (art-swap race, NULL-queue task, JPEGIMAGE-on-CYD, Sonos divide-by-zero).

### Waveshare — committed 2026-07-02 (Claude Code session)

| Area | What landed | Commits |
|---|---|---|
| Perf — Cover Flow rasteriser rewrite (VERIFIED on hardware, 3 profiled iterations) | `cf_render` rebuilt: `cf_prep_card` per-column trapezoid tables (internal-SRAM, ~23 KB) + `cf_compose` row-major composite — nearest-first with a covered-span clip so occluded pixels are never computed; separate 467 KB clear eliminated; per-pixel divide -> fixed-point multiply; PAPER ink frame + duotone folded in-row (duotone via a lazy 64 KB RGB565->luma LUT, frame test split out of the non-PAPER hot loop). Geometry math verbatim. Measured comp: BASIC 29->17.5 ms (19->23 FPS), PAPER 52-55->31-33 ms (12->16-18 FPS). `cf_prof` log relabelled `prep`/`comp`. Remaining CF perf levers parked in P4-TODO (downstream blit/rotate/flush ~24-27 ms, LV_DEF_REFR_PERIOD quantisation, GLYPH post-pass). | `af86e57` |

### Waveshare — committed 2026-07-05 (Claude Code session)

| Area | What landed | Commits |
|---|---|---|
| Arch — `app_core` shared component | See "Deferred architecture work" #1 below. | `31eaf04` |
| Refactor — `ui.c` screen-builder split (HARDWARE-VERIFIED) | `build_browser_screen`/`build_np_screen`/`build_settings_screen` split into 12/14/11 helpers, exact original call order + statics, byte-identical binary size. Lewis confirmed Browser/Now-Playing/Settings look correct in BASIC and PAPER, no draw-order regressions. | `489dd58` |
| Fix — RP2040 knob angle-wrap + live-anchor | `_compute_torque()` now diffs against a persistent unwrapped-angle accumulator instead of the raw [0,2pi) sensor reading (was guaranteed to hit a torque discontinuity on Volume's 0-100 sweep); `knob_input.c`'s `_activate_menu()` anchors Albums/Now-Playing to the live centred-album-index/playback-position instead of 0. P4 side build-verified; RP2040 side (`motor_task.cpp`) could not be compiled here (no PlatformIO) -- read carefully on first `pio run`. | `1bd4fa7` |
| Feature — HA build LIGHTS screen (BUILD-VERIFIED, not yet hardware-tested) | HA browser top-bar icon opens a scrollable `light.*` list (power toggle + release-gated brightness slider). Lives in shared `p4_shared/ui.c` like DEVICES, but the direct-Spotify build now compiles the top-bar Lights entry out (`P4_HAS_HA_LIGHTS` only set by the HA build) because it has no HA lights backend. `ha_client.c` gained `call_service_entity()` (targets an explicit entity, not just the active media_player), `build_light_list()`, and a second `s_lights_req_id` alongside the devices fetch. Both waveshare targets build clean (HA 14% / non-HA 13% free). Full detail in P4-TODO.md item 6. | pending commit |

### Waveshare — changed 2026-07-06 (Codex session)

| Area | What landed | Commits |
|---|---|---|
| Fix — HA lights diagnostics + feedback refresh (BUILD-VERIFIED) | Part 0 of `docs/IMPLEMENTATION-PLAN-2026-07-05.md`: light power taps now log the entity id, HA `call_service` sends log domain/service/entity plus data presence, failed HA result frames log their error message, and light toggle/brightness commands re-fetch the light list after a 400 ms Matter round-trip delay so the screen reflects the actual accepted state. Both waveshare targets build clean (HA 14% / non-HA 13% free). Needs HA-build hardware test against a real light. | pending commit |
| Feature — on-device Add Albums first slice (BUILD-VERIFIED, not yet hardware-tested) | Shared P4 UI now has a browser top-bar `+` screen that asks the direct-Spotify backend for saved albums (`/v1/me/albums`) and the HA backend for the active media player's browsable albums (`media_player/browse_media`), shows duplicate-aware ADD/ADDED rows, and appends selected album metadata to a small NVS runtime catalogue layered after the compiled seed albums. Browser and knob config read the combined catalogue. Direct Spotify requires `user-library-read`; HA depends on Music Assistant/media-player browse exposing playable Spotify album IDs. Runtime thumbnails, free-text search, and remove/reorder remain open. | `962a44f` |

### Waveshare — committed 2026-07-07 (Claude Code session)

| Area | What landed | Commits |
|---|---|---|
| Fix/UX — Add Albums failures explained ON SCREEN (BUILD-VERIFIED) | `ui_set_album_candidates`'s third param is now an err string (NULL = ok) across all three seam sites. Spotify 403 names the missing `user-library-read` scope + the fix; 401 says token rejected; network/HTTP errors say so; HA browse failures quote HA's `error.message`; the HA no-albums dead end points at Music Assistant and the serial browse-tree log. "Saved albums unavailable" as a blanket dead-end is gone. | pending commit |
| Tool — `get_spotify_token.py` scope update | The existing root token generator now requests the full firmware scope set incl. `user-library-read` and explains that refresh-token scopes are frozen at authorisation time — the old playback-only token can never gain the library scope, so ADD ALBUMS needs a freshly minted + installed token (Settings > SETUP or private secrets.h). | pending commit |
| Feature — Settings > SETUP runtime credentials (BUILD-VERIFIED, not yet hardware-tested) | Third Settings tab: WIFI SSID/PASSWORD (both builds) + SPOTIFY CLIENT ID/SECRET/REFRESH TOKEN (direct-Spotify build only; HA hides them via new `P4_BACKEND_HA` define). Full-screen `lv_keyboard` editor per field on `lv_layer_top()`; values in NVS namespace `creds` (`p4_shared/creds.c`); both `main.c`s read them once at boot with secrets.h as fallback (`creds_get`); empty save reverts to flashed; RESTART NOW key on the page. Secrets shown only as "set (n chars)", never logged, never pre-filled from secrets.h. Hardware test: type creds on-screen in each theme (keyboard renders? tap targets ok?), reboot picks them up, bad WiFi cred recovers via background reconnect. | pending commit |

### RP2040 haptic knob — committed/researched 2026-06-18 (Claude Code session)

Firmware committed; **gated behind `KNOB_ENABLED` (default 0)** so it does not
affect any P4 flash without the knob. Cannot be hardware-verified until the PCB
exists. Full reference in [`KNOB-NOTES.md`](KNOB-NOTES.md).

| Area | What landed | Commits |
|---|---|---|
| RP2040 firmware | `rp2040/` PlatformIO project: `motor_task` (SimpleFOC FOC + per-menu detent physics, `__not_in_flash_func` hot path, dual-core init-order guard), `interface_task` (UART/HX711/MX buttons/SK6812/VEML7700/MAX17048), vendored nanopb | `0c4170c`, `74f061a`, `b8a76cc` |
| P4-side driver | `waveshare/esp-idf/main/knob.c` (UART link: nanopb + CRC32 + COBS + ACK/retry) + `knob_input.c` (four-menu context mapper via the backend-neutral `ui_*` seam); vendored nanopb component | `0c4170c`, `74f061a` |
| Protocol schema | `proto/home_controller.proto` + generated `.pb.c/.h` (KnobConfig / KnobState / ToKnob / FromKnob) | `0c4170c` |
| Licensing | `NOTICE` (Apache 2.0 req 4d: SmartKnob / SimpleFOC / nanopb); per-file change notices on the SmartKnob-derived files | `0c4170c` |
| Bug — UART/PWM clash | RP2040 UART moved off GPIO0/1 (booked by the motor U-phase PWM) to UART1 GPIO8/9 | `414df9c` |
| Bug — P4 RX pin | P4 UART RX moved from GPIO33 (not on the J3 header) to GPIO46 | `fa62d4a` |
| Deep-research verify (2026-06-18) | All pin assignments + the UART protocol fact-checked across 5 web-research angles; findings folded into `CLAUDE.md` + `KNOB-NOTES.md`. Verified: `BLDCDriver6PWM` (no `TMC6300Driver6PWM`), `MagneticSensorMT6701SSI` (SPI_MODE2, 1 MHz), CRC32 agreement (`0xCBF43926`), 921600-baud divider error 0.1 %, ESP32-P4 has no input-only GPIOs. | `f90f12b` |

Remaining before this can clear: PCB in hand, flash with `KNOB_ENABLED=1`,
calibrate HX711 threshold + motor pole pairs, verify the four-menu feel on device.

### CYD (board not available 2026-05-30)

A large stack of changes hasn't been hardware-tested because Lewis didn't have
the CYD board around. Same general categories as the waveshare list above
(perf, reliability, UX, arch); the per-build details live in the
`project_cyd-verify-pending` project memory. Highest-impact items to check
when the CYD is back:

- **Volume base** — start the speaker at e.g. 80 %, boot the controller,
  confirm the HUD/encoder base is 80 % not 50 %.
- **WiFi reconnect** — same as waveshare check above.
- **MCP re-probe** — unplug the I2C wires during boot, plug back in after
  10 s; buttons/encoder should come online within ~5 s without rebooting.
- **Empty list message** — temporarily ship a build with an empty
  `spotify-albums-list.txt`; browser shows the "no albums configured" message
  instead of a black screen.
- **Volume HUD before first poll** — boot, immediately turn RE1 before the
  first poll lands; HUD should not appear (no nudging the speaker from a
  guessed 50 %); after the poll lands, encoder works normally.
- **2026-05-31 code-review batch** (commit `25d2ab8`) — 401 token-clear now
  also covers `spotify_play_album` (was only in `_do_cmd`); dead
  `s_warned_no_vol` removed from shared `input.c`. The 401 path is hard to
  trigger naturally; the same forced-corruption test as the waveshare item #8
  applies.

### CYD-IDF builds may not compile until verified

The `cyd_shared` extraction touched two `CMakeLists.txt`s and added `EXTRA_COMPONENT_DIRS`
to both top-level `CMakeLists.txt`s. The catch-up commit `1731a6a` filled in
the per-build `main/CMakeLists.txt` and the `spotify.h` thin wrappers, so the
layout *should* configure cleanly — but neither CYD build has been
compile-tested. Run `idf.py reconfigure && idf.py build` in `cyd/esp-idf/`
and `cyd/esp-idf-ha/` first; if anything fails, the most likely cause is a
missing `PRIV_REQUIRES` entry in `cyd/components/cyd_shared/CMakeLists.txt`
that one of the moved files needs (`esp_log` is implicit; double-check
`freertos` and `driver` if a header is "not found").

---

## Deferred architecture work

These are intentionally not on the active path until the verification debt
above is clear. Plans live in private memory; summaries here so they're
visible in the public repo.

### 1. Shared `app_core` component (from 2026-05-29 review)

The `cyd_shared` refactor extracted the byte-identical UI/input/MCP/art/FS
files, but each `main.c` still re-implements the WiFi state machine, the
`scmd_type_t` enum + `_post_cmd` + six `ui_request_*` posters, and (for
non-HA builds) `decode_and_publish_art`. Three near-duplicate copies, ~290
lines each.

**Landed 2026-07-02, WAVESHARE ONLY** (`waveshare/components/app_core/`,
`wifi.c` + `art_buffer.c`), needs a hardware flash+verify. Deliberately
narrower than the original plan sketch below:
- **WiFi connect + reconnect** (`app_core_wifi_connect()`): both waveshare
  `main.c`s now share one event handler + 20 s background-reconnect timer.
  This also fixes a real gap found while mapping the duplication -- the HA
  build's own WiFi code had NO recovery after its fast retries exhausted (the
  non-HA build's timer was hardware-verified; HA's absence was a silent
  drift). An optional `on_first_connect` callback preserves the non-HA
  build's connect chime; HA passes NULL (unchanged behaviour -- no chime
  today, not added as a side effect of this refactor).
- **Art buffer lifecycle** (`art_buffer_alloc/idle/publish`): the
  double-buffered-PSRAM swap logic was byte-identical between the two
  builds; extracted. The actual download/decode step (RAM vs LittleFS-file
  source, Spotify vs HA transport) stays in each `main.c` -- genuinely
  different, not worth forcing together.
- **NOT extracted: the command queue / `scmd_type_t` union idea below.**
  Read both files fully before scoping this: the two structs aren't just a
  differently-named enum, they're a different SHAPE (`scmd_t` is a flat
  type+uint32+char[64]; `hcmd_t` is a proper tagged union), the vocabularies
  only partly overlap (Sonos commands are non-HA-only), and unifying them
  would mean editing the hot dispatch `switch` in both `spotify_task` and
  `ha_task` -- the actual "talk to the backend" logic, which only Lewis can
  verify on hardware. Low value (each queue is already a compact, self
  contained ~30-90 lines) for real regression risk. Left as-is.
- **NOT extracted: CYD.** Both CYD-IDF builds are unverified since the
  `cyd_shared` extraction (see "CYD-IDF builds may not compile until
  verified" above) -- consolidating scaffolding that includes code no one
  can currently compile-test was judged too risky. Revisit once CYD
  verification clears; the waveshare `app_core` component is a reasonable
  template if CYD's WiFi/art code turns out similar enough.
- Bonus fix while in the area: HA's `xQueueCreate` for the command queue had
  no NULL check (the non-HA build already did) -- added, matching the
  existing PENDING.md reliability item F4 pattern.

**Verify on hardware:** flash both waveshare targets; confirm WiFi still
connects normally, and (if you can simulate it -- e.g. turn the AP off after
boot) that the HA build now recovers via the 20 s background timer instead
of staying disconnected forever. Confirm album art still displays/updates on
both builds (buffer lifecycle unchanged in behaviour, only where the code
lives).

### 2. CYD auto-dim/sleep (from 2026-05-30 review bucket C)

Waveshare auto-dim landed in `434d3ea`. The CYD path needs LEDC PWM init
added to the BL pin first (currently driven HIGH at boot, no PWM). That's
~50 lines of fresh init code on the hardware-verified lead build.

**Plan when revived:**
- One-time LEDC setup in each `main.c` during display bring-up (timer +
  channel on BL pin, 10-bit, 5 kHz).
- Thin `cyd_backlight_set(uint8_t pct)` helper in `cyd_shared/`
  (`backlight.c/.h`) mapping 0-100 → 0-1023 duty.
- Port the waveshare `idle_timer_cb` (60 s / 300 s thresholds, restores on
  touch via `lv_display_get_inactive_time`) to `cyd_shared/ui.c`.

**Trigger:** after CYD verification clears.

### 3. Optional architecture cleanups (low priority)

- Rename `spotify_track_t` → `track_info_t` (the type still bears the
  Spotify name even though the header is `player.h`). Touches every backend
  call site — bundle with the broader backend-contract rename if it ever
  happens.
- Long-press shortcuts on the CYD MCP keys (`btn_is_held` is exposed but
  unused). Would pair naturally with shuffle, jump-to-first-album, etc.

---

## How to clear an item

Flash the relevant build, run the sanity check, and either:
- **Pass:** move the row out of "Hardware-verify pending" into a "Verified"
  section (or just delete it from this doc), and update the relevant
  per-build README to mark the feature verified.
- **Fail:** open a bug entry with the symptom + serial log, fix, re-flash,
  re-verify.
