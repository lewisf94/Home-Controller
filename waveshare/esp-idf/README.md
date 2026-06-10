# waveshare/esp-idf — ESP32-P4, direct Spotify (lead build)

Music Controller on the **Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3** (ESP32-P4
RISC-V, 4.3" 480×800 IPS, **ST7701** MIPI-DSI, **GT911** touch, WiFi6/BLE5 via an
onboard **ESP32-C6** over SDIO, PSRAM, 32 MB flash). Talks **directly to the
Spotify Web API**. A future `waveshare/esp-idf-ha/` will swap the backend to
Home Assistant, exactly like the CYD split.

> **STATUS: cp1–3 hardware-verified; cp4+ + Sonos + brightness + reliability/UX
> batches committed, board in hand, needs hardware verify.** Display (cp1)
> renders at 800×480 landscape, the board associates to WiFi through the onboard
> ESP32-C6 (`esp_wifi_remote` + `esp_hosted` over SDIO) and pulls a DHCP lease
> (cp2), and the Spotify task refreshes the OAuth token (cached in NVS),
> validates the TLS cert bundle, and polls `/me/player` every 5 s (adaptive
> 15 s when paused) over a persistent keep-alive connection — boot log shows
> `now playing: <artist> -- <title>`. The UI (`ui.c`) is committed in full:
> browser + now-playing + Settings screen in two tabs — **DISPLAY** (Mode /
> Colour accent / Browser Style / Font / Selection Line / Brightness / FPS /
> Menu Transition) and **SOUND** (on-off / Volume / Sound set), all
> NVS-persisted — three browser styles (Carousel / Focus / Cover Flow), five
> MODE options (Dark / Black / Light / **GLYPH** dot theme / **PIXEL** retro),
> synthesised UI sound effects (ES8311 speaker), scrolling long titles,
> charcoal palette, flat buttons, tiny_ttf kerning crash fix, auto-snap-to-
> playing-album with accent border, OFFLINE indicator, generic toast for
> play-failures, on-screen `MAX_CARDS` warning, auto-dim/sleep, and a Sonos
> integration (direct UPnP control + album-start + device selector). **All of
> that still needs a hardware verification pass** — see
> [`../../docs/PENDING.md`](../../docs/PENDING.md) for the rolling list and
> [`../../docs/TESTING.md`](../../docs/TESTING.md) for the sanity-check menu.
> Most board-agnostic logic (Spotify client, album data, art decode, LittleFS)
> started as a copy of `../../cyd/esp-idf/` and has diverged where the P4's
> extras require it (Sonos integration, settings UI, larger display layout).

## Cover Flow geometry (canonical — do not regress)
Drawn by a PSRAM column rasteriser (`cf_render`/`cf_render_card` in `main/ui.c`),
blitted as one `lv_image` (no LVGL per-cover scaling). The intended look:

- **Centre** album: flat, facing the viewer, on top, largest.
- **Side albums face the centre**: the **OUTER** edge is nearest the viewer (drawn
  tallest), the inner edge recedes (shortest) and tucks **behind** the more-central
  neighbour. Left covers → left edge near; right covers → right edge near.
- **Z-order**: centre on top, each cover under the one nearer centre (stack
  outward + overlap). Art is perspective-foreshortened toward the far/inner edge.
- `CF_LEAN_FLIP` flips the lean if the rotated panel mirrors it.
- **Perf:** `cf_render` runs in the scroll handler, so its cost does **not** show
  in the FPS readout (which only times the blit). The converging fan keeps every
  cover on-screen, so `CF_MAX_SIDE` caps how many covers rasterise per scroll
  event — without it all albums draw each event and scrolling goes sluggish.
- Tuning dials: `CF_FAN_SPREAD`, `CF_FAN_RATE`, `CF_WIDTH_SHRINK`,
  `CF_HEIGHT_SHRINK`, `CF_MAX_SIDE`, `CF_CARD_SCALE`.

## Reference
Waveshare official component + demos:
https://github.com/waveshareteam/ESP32-P4-WIFI6-Touch-LCD-4.3
- LVGL bring-up copied from its `08_lvgl_demo_v9` (`bsp_display_start_with_config`,
  `bsp_display_lock/unlock`, rotation flag, triple-partial tear-avoid).
- WiFi config from its `04_wifistation` (`esp_wifi_remote` + `esp_hosted`).

## ONE-TIME SETUP (do this before building)

1. **Step 0 — validate the board (fresh out of box).** Clone the Waveshare repo
   and flash a demo to confirm the toolchain + display + C6 WiFi work:
   `idf.py set-target esp32p4` (installs the RISC-V toolchain), then build/flash
   `08_lvgl_demo_v9` (display) and `04_wifistation` (WiFi). If WiFi fails,
   re-flash the ESP32-C6 ESP-Hosted slave to match the IDF `esp_wifi_remote`/
   `esp_hosted` versions (see the repo / Espressif `esp-hosted-mcu` docs).
   **Don't proceed until both demos run.**

2. **Vendor the BSP** (it is *not* on the component registry). Copy the board
   component into this project:
   `cp -r <waveshare-repo>/examples/esp-idf/08_lvgl_demo_v9/components/esp32_p4_wifi6_touch_lcd_4_3  waveshare/esp-idf/components/`
   (Audio `bsp_extra` is not needed — this is a remote, not a player.)
   The BSP is also checked in at `components/esp32_p4_wifi6_touch_lcd_4_3/`.

3. **Secrets:** `cp include/secrets.h.example include/secrets.h` and fill in WiFi
   + Spotify credentials (gitignored).

4. Build: `idf.py set-target esp32p4` then `idf.py build flash monitor`.

## Board facts (confirmed from the repo manifests)
- Panel **ST7701** MIPI-DSI, 480×800 native; **rotated to 800×480 landscape** via
  the adapter `rotation` flag in `main.c`.
- Touch **GT911** (I2C). After rotating the display, `touch_flags`
  (swap_xy/mirror_x/mirror_y) in `main.c` may need adjusting so touch aligns.
- LVGL **9.4** via **`esp_lvgl_adapter`** (NOT `esp_lvgl_port` like the CYD) —
  the lock is `bsp_display_lock(timeout)` / `bsp_display_unlock()`. When porting
  `ui.c`, map `lvgl_port_lock(0)` → `bsp_display_lock(0)`.
- WiFi via `esp_wifi_remote` `0.14.*` + `esp_hosted` `1.4.*`, slave = esp32c6,
  SDIO. The `esp_wifi_*` API is routed to the C6, so `wifi_init_sta` ports nearly
  unchanged from the CYD build.
- PSRAM on. **Use ESP-IDF 5.5.x — NOT 5.4, NOT 6.0** (verified by build). Two
  constraints pin it to 5.5:
  - The BSP uses the USB-host API (`usb/usb_host.h`, for `bsp_usb_host_start`),
    provided by the `usb` component. **6.0 removed that component**, so 6.0.x
    fails configure with `Failed to resolve component 'usb'`.
  - The BSP's `esp_lvgl_adapter` dependency requires **IDF ≥5.5**, so **5.4.x is
    too old** (`no versions of idf match >=5.5.0`).
  - 5.5.x is the only line that satisfies both. Install via EIM
    (`eim install -i v5.5.4 -t esp32p4 -n true`) or the VS Code extension
    (Configure → Express → v5.5.x). Keep 6.0.x for the CYD builds.
- **SRAM budget (measured at cp2):** the P4 has 768 KB internal SRAM. Adding the
  WiFi path overflowed the fixed **IRAM** segment by ~2 KB — fixed by
  `CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM=n` (keeps LVGL hot functions out of
  IRAM). It was *not* general SRAM exhaustion: `.text`/`.rodata` XIP from PSRAM,
  and after cp2 there's ~390 KB internal heap free + 31 MB PSRAM. The real
  runtime risk is large buffers (256 KB Spotify response, album art) landing in
  internal heap — they must be allocated from PSRAM at cp3/cp5. Full budget
  analysis and the PSRAM-first policy live in `docs/PORT-NOTES.md`.

## Checkpoint roadmap
1. **Display skeleton** — `bsp_display_start_with_config` + hello label. *(hardware-verified)*
2. **WiFi** — add `esp_wifi_remote`; port `wifi_init_sta`; log the IP. *(hardware-verified)*
3. **Spotify** — Spotify task + `scmd_t` command queue (port the structure from
   `cyd/esp-idf/main.c`); log the track title; persistent keep-alive poll. *(hardware-verified)*
4. **UI** — ported `cyd/esp-idf/main/ui.c`; `lvgl_port_lock` → `bsp_display_lock`;
   constants re-laid-out for 800×480; touch scroll + tap-to-play; on-screen
   playback controls; Settings screen (Menu Transition / Mode / Colour / Browser
   Style / Brightness / Selection Line). *(committed — needs hardware verify)*
5. **Assets** — `album_thumbs.bin`, now-playing art (album_art.cpp), runtime
   tiny_ttf fonts (Montserrat + DejaVu fallback). *(committed — needs hardware verify)*
6. **Touch controls** — on-screen prev/play-pause/next + volume → `ui_request_*()`.
   *(committed — needs hardware verify; `input.c` seam left for physical controls)*
7. **Parity** — WiFi indicator, volume HUD, progress bar, view toggle, three
   browser styles, colour accents. *(committed — needs hardware verify)*
8. **Sonos** — direct UPnP/SOAP control (port 1400) of a Sonos speaker, including
   full album-start (enqueue cpcontainer → point transport at queue → Play).
   Combined device selector (Spotify Connect transfer + Sonos UPnP). Now-playing
   fallback reads UPnP `GetPositionInfo` when Spotify can't see the speaker.
   *(committed — needs hardware verify)*
9. **Reliability/UX** — WiFi background reconnect, 404 wake-on-play, dispatcher
   logging, OFFLINE title, toast on play failure, auto-snap browser to playing
   album, on-screen `MAX_CARDS` warning, empty-list message, volume-HUD guard
   before first poll, JPEG SOI marker check, auto-dim/sleep. *(committed — needs
   hardware verify)*
10. **PIXEL retro theme** — a MODE option: 1bpp Press Start 2P bitmap font
    (16 px body / 24 px heading), Bayer-dithered pixelated art + all browser
    thumbnails, dark-CRT palette, chunky flat UI. PSRAM thumb pool (~0.5 MB)
    allocated on PIXEL activation, freed on switch-away. `lv_font_pixel_16/24.c`
    committed as generated build inputs (Press Start 2P + FontAwesome5 symbols,
    `npx lv_font_conv --bpp 1`). *(committed — needs hardware verify)*
11. **Code quality** — `_do_cmd` forward-declared + `spotify_play_album` routed
    through it for keep-alive reuse; `MAX_DEVICES` constant replaces magic `16`s;
    `scmd_meta_t` table + `_Static_assert` replaces fragile exclusion chain;
    `copy_str` used consistently throughout `main.c`. *(committed)*
12. **GLYPH dot-matrix theme** — replaced the old Yudho/Fuhrer VFX-backdrop themes
    (the whole `lv_canvas` particle system is deleted) with a single MODE where
    every element is drawn in round dots: a dot text font baked from unscii-8
    (`scripts/gen_lvgl_font.py --dots`, sizes `lv_font_dot_20/24/28.c`), a dotted
    sparse-cmap FontAwesome font as its fallback (`lv_font_dot_sym_*`, so the cog /
    transport / chevron icons dot too), a "gas-tank" progress bar (Brownian-motion
    accent dots + a playhead bar), and a 4-dot WiFi strength meter. Font is fixed in
    GLYPH (FONT setting hidden). *(committed — needs hardware verify; cog has a
    first on-device check, reads a touch muddy at dot size — noted, deferred)*
13. **UI sound + tabbed Settings** — synthesised SFX via the onboard ES8311 speaker
    (`audio.c`/`audio.h`, `esp_codec_dev`): TICK/SELECT/BACK/CONNECT on a dedicated
    task + queue, named sound sets (SINE/CHIP/AMBIENT/MARIMBA/ARCADE/BELL) or AUTO,
    user volume with a square-law taper, all NVS-persisted. Settings reorganised into
    DISPLAY + SOUND tabs. Long browser/now-playing titles scroll horizontally
    (`LV_LABEL_LONG_SCROLL_CIRCULAR`) instead of ellipsising. Cover-Flow centre-tap
    play fix. Album-art `JPEGIMAGE` moved to internal SRAM (intermittent
    `JPEGDecodeMCU` store-fault fix). *(committed; sound + tabs need hardware verify,
    titles + decode-fix have a first on-device check)*

After all of the above is confirmed on hardware:
- **PPA hardware acceleration** — `enable_ppa_accel = true` in
  `bsp_display_cfg_t`. The P4 PPA does the 90° rotation/blit in hardware
  (currently software every frame).
- **RAM art decode** — switch from LittleFS round-trip to the existing
  `spotify_download_bytes` + `album_art_decode` RAM path (PSRAM-resident,
  no flash wear). The RAM path already exists; unused today.
- **Adaptive poll backoff** — already done (5 s playing, 15 s paused/idle).

See [`../../docs/P4-TODO.md`](../../docs/P4-TODO.md) for the rolling backlog
and [`../../docs/PENDING.md`](../../docs/PENDING.md) for the verify-pending list.

## Already in this folder
- Copied board-agnostic, unchanged: `spotify.c/.h`, `albums.c/.h`,
  `album_art.cpp/.h`, `littlefs.c/.h`, `album_thumbs.c/.h/.bin`.
- New: `main.c` (skeleton), `sdkconfig.defaults`, `partitions.csv`, `CMakeLists.txt`,
  `main/CMakeLists.txt`, `main/idf_component.yml`, `include/secrets.h.example`.
- **Vendored BSP:** `components/esp32_p4_wifi6_touch_lcd_4_3/` (checked in).
