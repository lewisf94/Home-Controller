# Pending — verification debt + deferred work

What's been written but not yet flashed/verified, plus the architecture work
that's been deliberately deferred. Keep this current as items get verified or
landed.

---

## Hardware-verify pending

### Waveshare ESP32-P4 (board in hand)

Everything on `main` past commit `42405ab` (Sonos album playback + device
selector) is committed but **not flashed**. Owed verification before the build
is "done":

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
| VFX canvas particle system | Yudho: 200 Keplerian vortex particles on 400×240 PSRAM canvas (192 KB), 2× scaled, per-frame fade trails. Fuhrer: 300 art-sourced emission particles sampling album art pixels. Replaces the old 20-dot random-jump LVGL-object system. | `05137ae` |
| FONT setting | Settings → FONT → SANS (Montserrat) or SLAB (Arvo Bold, OFL). NVS-persisted. PIXEL theme still overrides to Press Start 2P regardless. | `c64f543` |
| PPA hardware acceleration | `.enable_ppa_accel = true` in vendored BSP; offloads 90° software rotation to the P4 hardware 2D accelerator. | pending |
| Art theme scroller transparency | Yudho/Fuhrer: browser scroller `bg_opa = LV_OPA_TRANSP` so VFX canvas shows through card gaps and padding bands. | pending |
| Cover Flow 2-side | CF card slot 220→180 px, gap 28→16 px (step 248→196). Card ±2 centres at 792/8 px — fully on-screen. Squash rate 150→100, floor 70→85, dim_rise 150→80 so second side card still reads. | pending |
| FPS display | Settings → FPS DISPLAY toggle (ON/OFF); live `N FPS` label in browser top bar updated every 1 s via `LV_EVENT_FLUSH_READY` counter. NVS-persisted. | pending |

Sanity-check menu for next flash (waveshare):
1. **Sonos album-start** — pick a Sonos in device selector, tap an album from
   the browser; queue gets the tracks; transport advances; serial log shows
   the `x-rincon-queue:#0 SetAVTransportURI -> 200` then `Play -> 200`.
2. **WiFi reconnect** — power on with the router off, wait 30 s, power on the
   router; controller should reconnect within ~20 s without a power-cycle.
3. **404 wake-on-play** — let active Spotify device (phone) sit idle until
   it drops the slot (~30 min), press play; should wake the phone and start
   the last track.
4. **Auto-dim** — sit idle 1 min, screen should dim to ~30 %; idle 5 min,
   ~10 %; touch should snap back to your chosen brightness within 1 s.
5. **OFFLINE** — pull WiFi, now-playing title flips to "OFFLINE" within ~5 s
   (next wifi-timer tick); restore WiFi, title comes back.
6. **Auto-snap** — change track from the phone, open the browser, carousel
   should land on the album currently playing with an accent-coloured border.
7. **WiFi init-fail UI** (`25d2ab8`) — boot with a wrong password in
   `secrets.h`; the browser should still come up, serial log shows "wifi did
   not connect -- continuing; background reconnect will keep trying". Fix the
   password, reflash; "wifi connected" should appear within ~20 s with the UI
   already live, no power-cycle.
8. **401 token-clear** (`25d2ab8`) — corrupt `s_access_token` just before a
   `perform` in `_do_cmd` / `spotify_play_album` / `spotify_get_devices`,
   confirm the log line `got 401, invalidating cached token` and that the next
   press of the same control works. Remove the corruption afterwards.
9. **PIXEL theme** — Settings → MODE → PIXEL: all text crisp pixel glyphs (Press
   Start 2P 16/24 px); palette dark-CRT; all browser thumbnails are blocky/dithered;
   now-playing art is pixelated; transport icons are pixel shapes. Switch back to
   DARK: full-res art and smooth fonts return, no crash. Check PSRAM heap log to
   confirm thumb pool allocated then freed correctly.
10. **Yudho vortex** — Settings → MODE → YUDHO: browser and NP screens show white
    spiral particles on pure black, spinning inward with glowing trails. PSRAM heap
    +192 KB. Switch away: canvas freed, no crash. Idle ~60 s: auto-dim still fires.
11. **Fuhrer art emission** — Settings → MODE → FUHRER, play a track with album art:
    NP background fills with coloured drifting dots matching the art palette. No art:
    rainbow fallback visible. Art changes (new track): particle colours shift within
    ~5–10 s. PSRAM heap +192 KB while active.
12. **SLAB font** — Settings → FONT → SLAB: all title/artist/settings labels switch
    to Arvo Bold immediately. SANS reverts to Montserrat. NVS persists across reboot.
    PIXEL theme still overrides to Press Start 2P regardless of FONT setting.
13. **PPA rotation** — boot; confirm display renders correctly (no corruption or
    colour shift). Watch serial log for any PPA errors. Compare idle render speed
    vs before: should run noticeably cooler / faster in VFX themes.
14. **Art theme scroller transparency** — Settings → MODE → YUDHO, open browser:
    the VFX vortex should be visible through the gaps between cards and the padding
    bands on the left/right of the carousel. Cards themselves remain opaque (album
    art visible). Same for FUHRER.
15. **Cover Flow 2-side** — Settings → BROWSER STYLE → COVER FLOW: two covers
    should be visible on each side of the centre card (total 5 visible). Side cards
    squash and dim as before but the 2nd card peeks in from the edge.
16. **FPS display** — Settings → FPS DISPLAY → ON: a `N FPS` readout appears in
    the browser top bar (right of the WiFi bars). Should update every ~1 s. NVS
    persists across reboot. Toggle OFF: label disappears.

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

**Plan when revived:**
- New top-level component (e.g. `components/app_core/`) covering
  `connectivity.{c,h}` (WiFi + reconnect timer), `cmd_queue.{c,h}` (the
  command vocabulary), `art_pipeline.{c,h}` (the JPEG scratch path).
- Per-build `main.c` keeps only board-specific bring-up (BSP/display, NVS
  init) and the backend-specific dispatch switch.
- `scmd_type_t` becomes a union (waveshare's 3 Sonos commands sit alongside
  CYD's 6 commands; unused values are harmless).

**Trigger:** after CYD verification clears, or before the next feature that
would otherwise need editing the scaffold in three `main.c` files.

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
