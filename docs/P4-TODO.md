# Waveshare ESP32-P4 — next-steps backlog

What's still planned for `waveshare/esp-idf/` after the cp1-7 + reliability/UX
batches. Ordered roughly by priority. Many of the items previously listed here
have shipped; for the rolling list of "shipped but not yet hardware-verified"
items, see [`PENDING.md`](PENDING.md).

---

## Shipped to date (one-liners — see commit history + per-build README for detail)

- **cp1-3 (display / WiFi / Spotify)** — hardware-verified.
- **cp4-7 + full UI (HARDWARE-VERIFIED 2026-06-13)** — LVGL browser + now-playing;
  tabbed Settings (DISPLAY: Appearance dark/light, Mode, Theme album art, Colour,
  Browser Style, Font, Selection Line, Brightness, FPS, Menu Transition; SOUND:
  on-off, Volume, Sound set); three browser styles (Carousel / Focus / Cover Flow,
  the latter a PSRAM column rasteriser showing ~3 covers/side); runtime tiny_ttf
  fonts with the kerning-cache crash fix; scrub thumb.
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
- **Theme system overhaul (HARDWARE-VERIFIED 2026-06-13)** — collapsed to four
  MODEs (BASIC / GLYPH / PIXEL / PAPER), each with a DARK/LIGHT face + an
  APPEARANCE toggle, a THEME-ALBUM-ART on/off, and an 8-hue x 3-variant
  (24-swatch) accent grid. Per-MODE layout/colour knobs live in `main/ui_tune.h`;
  FONT setting (SANS Montserrat / SLAB Arvo, overridden in PIXEL/GLYPH/PAPER). All
  NVS-persisted.
  - **PIXEL** — Press Start 2P 1bpp font, Bayer-dithered art/thumbs, dark-CRT.
  - **GLYPH** — Nothing-OS-light: dot-matrix HEADINGS only over clean Montserrat
    body/icons (retired the old all-dots "muddy cog"), hairline pills, solid-ink
    selection, ink gas-tank/WiFi/volume instruments. (Replaced the deleted
    Yudho/Fuhrer VFX backdrops.)
  - **PAPER** — teletype data-brutalism: cream + ink, unscii mono fonts, 1-bit
    Bayer-dithered art, printed-form frames/rules, ruler-tick progress, TELEX SFX.
- **UI sound + tabbed Settings (cp13)** — synthesised SFX via ES8311 (`audio.c`),
  sound sets + volume; scrolling long titles; Cover-Flow centre-tap fix; album-art
  `JPEGIMAGE` in internal SRAM.
- **Diagnostics / safe experiments (2026-06-14)** — Cover Flow clear-vs-rasterise
  profiler (`cf_profile_tick`, FPS-gated, `99605ed`) and the `ART_DECODE_RAM` A/B
  flag (`9bdd80a`); see "Open — perf". Plus the Sonos-stall fix (`56e1a9b`).
- **Code quality** — `_do_cmd` forward-decl + `spotify_play_album` keep-alive
  reuse; `MAX_DEVICES` constant; `scmd_meta_t` table + `_Static_assert`; `copy_str`
  consistency in `main.c`.

---

## Active queue (2026-07-02, Lewis-ranked — work top to bottom)

1. **Cover Flow rasteriser rewrite** — DONE 2026-07-02, HARDWARE-VERIFIED
   over three profiled iterations (HA build, cf_prof serial):
   - Row-major single pass (`cf_prep_card` tables + `cf_compose`; clear
     eliminated, divide -> fixed-point multiply): BASIC comp 29 ms.
   - Occlusion clip (nearest-first, covered-span per row; occluded pixels
     never computed): BASIC 19 ms, PAPER 42 ms.
   - RGB565->luma LUT for the PAPER duotone + frame/no-frame span variants:
     **BASIC 17.5 ms (23 FPS), PAPER 31-33 ms (16-18 FPS)** — from 29 ms /
     19 FPS and 52-55 ms / 12 FPS at the start. prep stays ~0.35 ms.
   Geometry math unchanged (canonical-geometry note above `cf_prep_card`).
   **Parked follow-ups (open the next time CF perf matters):**
   - The downstream ~24-27 ms/frame (LVGL canvas blit + PPA rotate + DSI
     flush) now dominates over comp in BASIC — that's esp_lvgl_adapter/BSP
     territory (mind the DIRECT-mode/rotation CRITICAL notes).
   - Frame times quantise to `CONFIG_LV_DEF_REFR_PERIOD=15` ms buckets
     (observed FPS = 1000/(n*15)); dropping to 10 is a one-line experiment.
   - GLYPH's dot pass is still a whole-canvas post-pass (+4-8 ms in GLYPH);
     candidate: per-row-band dotting inside cf_compose.
2. **`app_core` shared component** — DONE 2026-07-02 (needs hardware flash
   verify): `waveshare/components/app_core/` now holds the WiFi
   connect-and-reconnect state machine and the double-buffered art-buffer
   lifecycle, shared by both waveshare `main.c`s. Also fixed a real gap found
   while mapping the duplication: the HA build had no WiFi recovery after its
   fast retries exhausted (non-HA's 20 s background timer was
   hardware-verified; HA's absence was silent drift) -- HA now gets the same
   resilience. The command-queue vocabulary was deliberately left
   per-build (real struct-shape/vocabulary differences, Sonos-only commands,
   too much dispatch-switch risk for the value); CYD was left out entirely
   (its builds are unverified since `cyd_shared` -- see item 5). Full
   rationale in PENDING.md "Deferred architecture work" #1.
3. **Split the big screen-builders** — DONE 2026-07-05, HARDWARE-VERIFIED
   (Lewis confirmed the visual pass 2026-07-05: Browser/Now-Playing/Settings
   look correct in BASIC and PAPER, no draw-order regressions).
   `build_browser_screen`/`build_np_screen`/`build_settings_screen` split into
   12/14/11 small helpers respectively, each doing exactly what its block did
   inline; every helper is called from the parent in the ORIGINAL TEXTUAL
   ORDER, and all state is the same file-scope statics as before (no new
   params except `pg_disp`/`pg_snd` in settings, which were always plain
   locals). Both targets build with BYTE-IDENTICAL binary size to pre-split
   (0x6eeff0/13% free non-HA, 0x6dc510/14% free HA).
4. **RP2040 knob firmware pre-flight review** — DONE 2026-07-05, BOTH FINDINGS
   FIXED (details in `docs/KNOB-NOTES.md` "Pre-flight code review"). (1)
   `_compute_torque()` didn't unwrap the encoder's 0-2π angle (Volume was
   guaranteed to hit a torque discontinuity on a full sweep; Now-Playing scrub
   on any track >2 min; Albums partially self-healed via per-detent re-anchor)
   -- fixed with a persistent unwrapped-angle accumulator in
   `rp2040/src/motor_task.cpp`. (2) Albums/Now-Playing anchored to position 0
   on menu activation instead of the live centred-album-index/playback-position
   -- fixed via a new `ui_get_centered_album_index()` getter + wiring both
   menus to their live values in `knob_input.c`. P4-side changes (`ui.c`,
   `ui.h`, `knob_input.c`) build-verified clean on both waveshare targets,
   byte-identical binary size. The RP2040 fix (`motor_task.cpp`) could NOT be
   compile-checked -- no PlatformIO in this environment -- read it carefully
   and watch for build errors on the first `pio run`; KNOB-NOTES.md has a
   specific first-flash sanity check (sweep Volume 0->100, no torque kick).
5. **CYD resurrection** — ON HOLD 2026-07-05 (Lewis: "ignore cyd stuff atm").
   Blocked on tooling anyway: this machine only has ESP-IDF 5.5.4 installed
   (used for waveshare); the CYD builds need 6.0 (`cyd/esp-idf/README.md` —
   the display driver already uses 6.0's `rgb_ele_order`, not 5.x's
   `rgb_endian`, per `PORT-NOTES.md`). Still TODO whenever picked back up:
   compile-verify both CYD-IDF builds (untested since the `cyd_shared`
   extraction) and port back the waveshare-only fixes (429 Retry-After
   holdoff, `RESP_INITIAL_CAP=16K`).
6. **HA build: lights menu** — DONE 2026-07-05, BUILD-VERIFIED (both waveshare
   targets, HA 14% / non-HA 13% app-partition free), NOT YET HARDWARE-TESTED.
   A third top-bar icon (left of DEVICES) opens a scrollable `light.*` list:
   power-icon toggle + a brightness slider (shown only when the entity reports
   `brightness`), release-gated so dragging doesn't flood HA. Fetched the same
   one-shot `get_states` way as DEVICES (`ha_request_lights()` /
   `build_light_list()`, own `s_lights_req_id`) -- no live push subscription,
   so the list is a snapshot re-fetched on every screen open. Toggle/brightness
   go through a new `call_service_entity()` targeting the tapped light's own
   entity_id (`call_service()` is now a thin wrapper over it for the active
   media_player, unchanged at every existing call site). Lives in the SHARED
   `p4_shared/ui.c` like DEVICES, but the direct-Spotify build compiles out the
   top-bar Lights entry because it has no HA lights backend (`P4_HAS_HA_LIGHTS`
   is only defined by the HA build). The non-HA seam functions remain no-ops so
   the shared code still links. Follow-up diagnostics
   now log light taps, HA service sends, and failed result frames; toggle and
   brightness commands also re-fetch the light list after a short Matter
   round-trip delay so the UI reflects accepted state. One real bug caught
   by the build itself: `ui_light_t.entity_id` was first sized 64 B against a
   96 B `eid` source buffer (GCC `-Werror=format-truncation`) -- resized to 96
   throughout (`ui_light_t`, both `hcmd_t` fields in the HA build's `main.c`) to
   match the `eid[96]`/`s_dev_ids[][96]` convention already used everywhere
   else in `ha_client.c` for one HA entity_id.
   **Verify on hardware:** toggle + dim a real light from the HA build; confirm
   the non-HA build's LIGHTS screen opens cleanly to the empty state and never
   crashes/hangs (it never calls a backend).

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

2. **Cover Flow scroll — REWRITTEN 2026-07-02 (row-major single pass; needs
   on-device verify).** The old path was memory-bandwidth bound: a 467 KB
   full-canvas clear + column-major `cf_draw_col` writes (1600-byte stride vs
   a 128-byte L2 line, ~185 MB/s PSRAM) + PSRAM overdraw at card overlaps +
   a whole-canvas re-read for the PAPER duotone. Now `cf_prep_card` fills
   per-column tables (internal SRAM) and `cf_compose` builds each row in
   cache — bg fill, card sampling, ink frame and PAPER duotone in one pass —
   so each canvas pixel hits PSRAM once per frame, and the per-pixel src_y
   divide became a fixed-point multiply. `cf_profile_tick` now logs
   `prep`/`comp` (was `clear`/`rast`); flick Cover Flow with FPS ON and
   compare `frame=` against the pre-rewrite numbers. CPU (360 MHz = P4 IDF
   ceiling), build flags (-O2, PSRAM 200 MHz, 256 KB L2) and PSRAM headroom
   (~70 %) are already maxed. Panel caps at 60 Hz, so the goal is holding 60
   during a flick, not a higher number. Remaining follow-up if GLYPH still
   lags: fold `cf_glyph_dither` (still a post-pass) into a per-band scratch
   tile.

Done, not TODOs (verify on hardware, see PENDING.md): **PPA rotation** is
already enabled — the vendored BSP hardcodes `.enable_ppa_accel = true`
(`bsp_display_lcd_init`). **TLS keep-alive on commands** is in — `_do_cmd`
reuses the persistent `s_cmd_client` (keep_alive_enable), same as the poll.
The FPS-maxing batch also landed: achieved-frame-rate FPS counter, PSRAM
thumb pools (1:1 card blits, no flash XIP reads on scroll), GLYPH gas-tank
ticker frozen off-screen. The two-SW-draw-unit experiment
(`CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT=2`) was REVERTED — it boot-loops against the
BSP's PPA accel; the setting MUST stay `=1` (see `sdkconfig.defaults`).

---

## Code-quality deep-dive audits (2026-06-14 — A-F all DONE)

Read-only investigations of the current code, Lewis's request. Findings logged
to PENDING.md. The one actionable runtime gap (B, the Sonos stall) is now fixed
(`56e1a9b`); the rest are minor/clean.

- **A. Concurrency / lock discipline — DONE, CLEAN.** Lock discipline sound;
  command queue value-copied + non-blocking; album-art double buffer race-free
  (`s_art_buf` is single-task). Constraint: `spotify_task` must stay the sole
  writer of `s_track` / `s_art_buf` / `s_sonos_*`; future physical input posts to
  `s_cmd_queue`.
- **B. Failure-mode / resilience sweep — DONE.** Mostly graceful (WiFi reconnect,
  token refresh, 429 holdoff, 404 wake, OOM degrades with fallbacks). The one real
  gap — an unreachable explicitly-pinned Sonos stalling the poll on a SOAP timeout
  every cycle (~4 s; `sonos_fetch_now_playing` bails after the first query, so it's
  one timeout, not three) — is now **FIXED** (`56e1a9b`: 2 s timeout + a 10 s
  fetch-backoff that retries instead of re-stalling). Minor, still open: Spotify 5xx
  no extra backoff; a persistently non-JPEG art URL retried not blacklisted;
  display-init/WiFi-wait returns unchecked (hardware-fault only). Two agent
  "criticals" were false positives.
- **C. Credential & TLS security posture — DONE (clean).** No secret logged, all
  6 HTTPS clients verify certs, secrets gitignored, parsing bounded; plaintext-NVS
  access token is informational (no flash encryption; refresh token not persisted).
- **D. Long-uptime heap / fragmentation — DONE (clean).** No leaks/double-frees;
  pools freed-before-realloc; only nit is the per-poll Spotify response-buffer
  malloc/free (could be a persistent reused buffer; safe as-is).
- **E. Multi-build drift / `app_core` consolidation — DONE.** `cyd_shared` =
  UI/input/MCP/art/FS; `spotify.c`+`main.c` per-build (~290 L dup → `app_core`).
  Port to CYD when that board is back: 429 holdoff + `RESP_INITIAL_CAP=16K`
  (waveshare-only). The internal-SRAM JPEGIMAGE fix is missing from `cyd_shared`
  but MOOT on the current no-PSRAM CYD — port only with the CYD-PSRAM item.
- **F. Sonos UPnP/SOAP robustness — DONE.** No crash bugs (divide-by-zero guarded
  — verified; unescape overflow-safe; buffers bounded). Minor: 8 KB DIDL
  truncation only validates the title; no range-check on time/volume; outgoing
  DIDL not XML-escaped (Spotify URIs are safe). See `sonos.c` recommendations.

---

## Post-hardware refactor candidates (readability)

From the 2026-06-15 readability pass. The two clearly-safe wins shipped
(`2917fca`: NVS `save_*` dedup -> one `nvs_save_u8` helper; the RGB565-packing
expression at 6 sites -> the existing `rgb888_to_565`). A third — splitting
the big screen-builders — is also now DONE + hardware-verified (see active
queue item 3 above). Remaining, still DEFERRED because they touch verified /
perf-sensitive code and want an on-device re-test:

- **Dither-loop helpers** — factor the RGB565 *unpack* (`r=((px>>11)&0x1F)<<3`…),
  the Rec.601 luma, and the Bayer threshold into `static inline` helpers
  (zero-cost at -O2) used by `paperize_rgb565` / `cf_paper_dither` /
  `accent_text_color`. Safe but in the hottest pixel loops — batch with the
  Cover Flow perf work.
- **Minor naming** — a few one-off settings-page dimensions could move to
  `ui_tune.h`; `cf_draw_col` -> `cf_draw_column`. Marginal.

## Open — UX polish

These would each be a small/medium PR.

- **Aesthetic pass (functional colour-coded transport keys, mono numerals)** —
  NOTE the theme SYSTEM was overhauled since this was written (four dark/light
  MODEs, 24-swatch accents, PAPER's mono fonts), so re-scope first. Still un-done
  (verified absent in `ui.c`): functional colour-coded transport keys (prev=blue
  `#1270b8`, play=green `#1aa167`, next=yellow `#ffc003`, active=red `#ce2021`),
  tabular numerals for timestamps outside PAPER, circular knob-style transport
  buttons, hairline dividers. Caveat unchanged: a 2nd embedded TTF eats ~4 %
  app-flash and re-opens the tiny_ttf kerning-crash surface.
- **First "place me at the playing album" on browser open** — auto-snap is in
  (`s_target_card`); if the browser is opened before any poll matched an album it
  sits at index 0 until the next match, then scrolls (could feel jumpy). Minor;
  double-check on hardware.

Retired since this list was written:
- **Restricted-device 403 hint** — DONE: the direct-Spotify dispatcher now
  surfaces `spotify_last_cmd_status() == 403` with the existing toast
  ("Active device is restricted -- transfer first") instead of leaving only a
  serial `FAILED` log.
- **Cover Flow "show more covers each side"** — DONE/obsolete: the old "only 1 per
  side / 248 px slots" was the pre-rasteriser layout. The PSRAM column rasteriser
  (`70812a0`) now draws a converging fan capped at `CF_MAX_SIDE = 3` covers/side.
- **Sonos-unreachable poll stall** — FIXED (`56e1a9b`); see audit B above.

---

## Physical controls — RP2040 haptic knob (FIRMWARE DONE, hardware pending)

The Settings UI is touch-driven today; there's no MCP23017 on the waveshare. The
custom **RP2040 smart-knob daughterboard** (FOC gimbal motor, strain-gauge press,
4 MX buttons, battery gauge, LED ring, ambient sensor) is the planned physical
input — full hardware design in [`DESIGN_NOTES.md`](DESIGN_NOTES.md), reference in
`docs/smartknob-repo/`.

**Firmware is committed and gated** (not yet wired/flashed — no PCB yet):
- RP2040 firmware in [`../rp2040/`](../rp2040/): `motor_task` (SimpleFOC FOC +
  detent physics on core 1), `interface_task` (UART/sensors/LEDs on core 0).
- P4-side driver in `waveshare/esp-idf/main/knob.c` + `knob_input.c`, behind
  `#if KNOB_ENABLED` (default 0) — knob-less flashes are unaffected.
- UART protocol (nanopb + CRC32 + COBS), pin map, and SimpleFOC/MT6701 facts all
  documented in [`KNOB-NOTES.md`](KNOB-NOTES.md). The 2026-06-18 deep-research
  pass verified all pin assignments and protocol details; one critical bug (UART
  on the motor's GPIO0/1 PWM pins) was found and fixed.
- `knob_input.c` calls only the `ui_*` seam (self-locks, audit A), so it's
  backend-neutral and carries over to `waveshare/esp-idf-ha/` untouched.

**Remaining (needs the PCB in hand):** flash with `KNOB_ENABLED=1`, calibrate the
HX711 press threshold and motor pole pairs, verify the four-menu interaction model
on device. Checklist in [`KNOB-NOTES.md`](KNOB-NOTES.md).

---

## Capacity ceiling

`MAX_CARDS = 128` on waveshare (`ui.c`). Real limit: thumbnails are embedded in
the 8 MB app partition at 220×220 (~95 KB each), and several fonts have been added
since this note (PIXEL/GLYPH/PAPER/Arvo), so the app partition is the binding
constraint — **RE-MEASURE the headroom (`idf.py size`)** before adding many albums
rather than trusting the old "~96 % full" figure. Flash is 32 MB (8 MB app + 4 MB
`storage`). Options when you hit it:

- **Grow the app partition** in `partitions.csv` (cleanest, ~85+ more albums
  possible). Raise `MAX_CARDS` alongside.
- **Shrink the thumbs** — 160×160 ~51 KB each / 120×120 ~29 KB each. Visual
  quality drops at small sizes.
- **Move thumbs to a data partition** loaded at runtime. Cleanest long-term
  but adds a startup load step.

## Open — album catalogue management

- **Add albums from the controller itself** — replace or supplement the current
  compile-time `spotify-albums-list.txt` + embedded-thumb pipeline with a
  runtime catalogue path. Target UX: from the Albums screen, open an Add/Search
  flow, search Spotify albums and/or browse the user's saved library, preview
  results with cover art, then add the chosen album to the selection list without
  a laptop/reflash.
  - First slice landed 2026-07-06 (BUILD-VERIFIED, not hardware-tested): the
    shared P4 UI has a top-bar `+` screen that asks the direct-Spotify backend
    for the first page of saved albums (`/v1/me/albums`) and the HA backend for
    HA media-player media-browser albums (`media_player/browse_media`) by
    discovering all `media_player.*` entities and walking them until playable
    Spotify albums are found. It displays duplicate-aware ADD/ADDED rows and
    appends selected album metadata to a small NVS runtime catalogue layered
    after the compiled seed albums. The knob config now reads the combined
    catalogue count too. Direct
    Spotify requires `user-library-read`; HA depends on Music Assistant exposing
    playable Spotify albums in at least one HA media browser.
  - 2026-07-07: failures now explained ON SCREEN (403 missing-scope hint, HA
    browse error text, network); `get_spotify_token.py` (repo root) now
    requests `user-library-read` (scopes are frozen at authorisation time —
    the original playback-only token can NEVER read the library; mint + install
    a fresh one); Settings > SETUP lets a user enter WiFi + Spotify credentials
    on-device (NVS overrides secrets.h at boot, `p4_shared/creds.c`).
  - 2026-07-08: HA Add Albums no longer depends on Music Assistant exposing an
    album library. The shared `+` screen now has a SEARCH overlay; the HA build
    reads Spotify client id/secret from Settings > SETUP or flashed secrets.h,
    gets a client-credentials token, and searches Spotify's public catalogue
    with `/v1/search?type=album`. Empty/no-query requests keep the older HA
    media-browser walk as a fallback. Direct-Spotify still uses saved albums
    for now, with a compatibility adapter so the shared UI links.
  - Still open: direct-Spotify free-text catalogue search, result cover-art
    preview, and a richer controller-friendly text-entry/result filter flow.
  - Storage still open: downloaded/cached thumbnails in a data partition or
    LittleFS-style store. Runtime albums currently add metadata only, so they
    fall back to the no-art card paths instead of getting embedded thumbs.
  - UI/input still open: remove/reorder actions and clearer progress/error
    toasts for network failures.
  - Capacity: depends on the runtime-thumb/data-partition decision above; avoid
    growing `albums.c`/`album_thumbs.bin` for albums added on-device.

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
