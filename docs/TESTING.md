# Hardware Verification Checklist

The active list of what to check after flashing each build. Items are grouped
by build; tick them off as you go. If something fails, capture the symptom +
serial log and we'll fix before the next pass.

For the rolling list of which commits have been written but not yet flashed,
see [`PENDING.md`](PENDING.md) — that doc is the verification debt; this one
is the test script you run against it.

---

## Waveshare ESP32-P4 (`waveshare/esp-idf/`)

Latest commits to verify against: `434d3ea` (auto-dim) on top of all the
05-30 / 05-29 / 05-28 / 05-27 / 05-26 / 05-24 work. Board in hand, just
needs a fresh flash.

### Smoke test — must pass before anything else

- [ ] Boot: serial shows `now playing: <artist> -- <title>` within ~10 s
      of WiFi connect (or `no active playback (or fetch failed)` if nothing
      is playing).
- [ ] Touch responds: tap an album card and the carousel snaps it centre;
      tap again and now-playing opens.
- [ ] Display: full 800×480, no tearing, no black cards on scroll
      (Cover Flow specifically).

### Reliability (05-27 / 05-28 batches)

- [ ] **WiFi reconnect** — power on with the router off, wait 30 s, then
      switch the router on. Controller reconnects within ~20 s without a
      power-cycle. Used to be a dead end after 5 retries; serial should now
      show "arming background reconnect every 20 s" then a successful join.
- [ ] **404 wake-on-play** — let your active Spotify device (phone) sit idle
      until it drops the slot (~30 min). Tap a play button on the controller;
      should wake the same device and resume. Serial: `play returned 404 --
      waking last device`.
- [ ] **Failed-decode tracking** — hard to trigger in the wild; if you spot
      a "art decode failed, not retrying this url" log, confirm the device
      doesn't keep re-fetching the same broken JPEG every 5 s.
- [ ] **Cmd queue alloc** — not testable in normal use (heap exhaustion at
      boot). If it ever fires, expect a clean `cmd queue alloc failed` log
      with no panic backtrace.
- [ ] **`esp_littlefs_info` warning** — boot log shows real KB total/used,
      not "0 KB total, 0 KB used". If you see the latter with a distinct
      warning about `esp_littlefs_info failed`, something's wrong with the
      partition table.

### Sonos integration (05-26 + recent fixes)

- [ ] **Album-on-Sonos** — pick a Sonos from device selector, tap an album
      from the browser. Serial shows the queue populating + transport
      pointing at `x-rincon-queue:<uuid>#0` (the `#0` fix; was `:0` before),
      then `Play -> 200`. Sonos plays the album.
- [ ] **Sonos transport** — once playing, prev/next/seek/volume on the
      controller drive the Sonos (UPnP, not Spotify Web API).
- [ ] **Sonos now-playing** — the title/artist/album/progress reflect what
      the Sonos is actually playing (from UPnP `GetPositionInfo`), even
      though Spotify's `/me/player` returns 204.

### UX (05-30 batches)

- [ ] **OFFLINE** — pull WiFi during use. Within ~5 s the now-playing title
      flips to "OFFLINE". Restore WiFi; title comes back on the next
      successful poll.
- [ ] **Empty album list** — temporarily ship an empty
      `spotify-albums-list.txt`. Browser shows "No albums configured / edit
      spotify-albums-list.txt + reflash" instead of a blank screen.
- [ ] **Volume HUD before first poll** — boot, immediately try the volume
      slider before any Spotify poll lands. Should not nudge the speaker from
      a guessed 50 %. After the first poll, normal behaviour.
- [ ] **JPEG SOI marker** — hard to trigger; if a CDN ever serves a non-JPEG
      body, the log says "downloaded bytes are not JPEG (magic XX XX) --
      discarding" and the next poll retries (instead of showing last-track
      art forever).
- [ ] **MAX_CARDS warning** — add 65+ albums (the cap is 64); top of the
      browser shows an amber `+N more (raise MAX_CARDS)` label.
- [ ] **No-device toast** — disconnect all Spotify devices, tap an album
      from the browser. Toast appears at the bottom of now-playing: "No
      active Spotify device" (or "Sonos play failed" if routed to Sonos).
- [ ] **Auto-snap to playing album** — change track from your phone. Open
      the browser; carousel should land on the currently-playing album with
      an accent-coloured border. Continues to track as the album changes.
- [ ] **Auto-dim** — sit idle 60 s, screen should dim to ~30 % of your
      Settings brightness; idle 5 min, ~10 %. Any touch should snap back
      within 1 s. Floored at 2 % so the screen never reads as dead.

### Settings (commits `540df95`, plus earlier theme/transition work)

- [ ] **Brightness slider** — drag the Settings brightness slider 10-100 %;
      panel dims live. Reboot; level is restored from NVS.
- [ ] **Theme / Accent / Browser Style / Transition** — change each in
      Settings; persists across reboot.
- [ ] **`ledc: GPIO 26 is not usable`** warning at boot — benign (BSP
      double-init of the backlight LEDC channel); dimming still works.

---

## CYD ESP-IDF (`cyd/esp-idf/`) — board not currently available

When the CYD is back, run [`PENDING.md`](PENDING.md)'s CYD checklist. The
verify-pending memory has the full per-finding sanity tests. Headline items:

- [ ] **Builds at all** — `idf.py reconfigure && idf.py build` after the
      `cyd_shared` extraction (`2f7accd` + `1731a6a`). If configure fails on
      a missing `cyd_shared` source or unresolved component, see
      [`PENDING.md`](PENDING.md).
- [ ] **Volume base from device** — start the speaker at e.g. 80 %, boot the
      controller, confirm the HUD/encoder base is 80 % not 50 %. Used to
      always start at 50.
- [ ] **WiFi reconnect** — same as the waveshare check above.
- [ ] **MCP re-probe** — unplug the MCP I2C wires during boot; plug back
      after 10 s. Buttons/encoder should come online within ~5 s without
      rebooting (5 s retry tick in `mcp_input_update`).
- [ ] **404 wake-on-play** — same as the waveshare check above.
- [ ] **Empty album list / OFFLINE / no-device toast / auto-snap** — same
      as waveshare (they live in shared `ui.c`).
- [ ] **MAX_CARDS warning** — CYD cap is 32; same on-screen label.
- [ ] **Auto-dim** — *not yet implemented on CYD* (needs LEDC PWM init on
      the BL pin; deferred — see [`PENDING.md`](PENDING.md)).

---

## CYD ESP-IDF HA (`cyd/esp-idf-ha/`) — never hardware-tested

A first-flash on a CYD with a Pi 5 running HA OS is owed. The UI / input /
art code is byte-shared with `cyd/esp-idf/` via `cyd_shared` so all the
reliability/UX features above land here too. Only the backend differs:
`ha_client.c` instead of `spotify.c`.

- [ ] **Builds** — `idf.py reconfigure && idf.py build` after the
      `cyd_shared` extraction.
- [ ] **WebSocket connect** — boot log shows the WebSocket handshake to
      `ws://<HA_HOST>:<HA_PORT>/api/websocket`, then `auth_ok`, then
      subscribe to `state_changed` for `HA_ENTITY`.
- [ ] **First state push** — within seconds, the UI shows the current track
      from `HA_ENTITY` (Music Assistant or Spotify integration via HA).
- [ ] **Commands round-trip** — buttons / touch trigger HA `media_player.*`
      service calls; play/pause/next/prev/seek/volume reflected on the
      speaker.
- [ ] **Album art** — fetched from the HA-proxied `entity_picture` URL
      (local network, no TLS round-trip).

---

## CYD Arduino (`cyd/platformio/`) — never re-flashed since LVGL port

Phase 1 + 1.5 features were shipped on TFT_eSPI direct draw. The build was
since rewritten on top of LVGL 9.5 + perf/reliability fixes from the
2026-05-24 / 05-26 reviews. The new code compiles and fits but has never
been on hardware. Smoke test first, then run the full Phase 1.5 list below.

- [ ] **Smoke test** — boots, display lights, LVGL screen renders without
      colour swap or rotation issues. Touch works for scrolling the
      carousel and tapping cards.
- [ ] **TLS handshake** — `setCACertBundle(...)` is in use (not the old
      `setInsecure()`); Spotify HTTPS connects, token refreshes.
- [ ] **Perf changes** — `setReuse(true)` keeps the poll connection alive
      between cycles; `DeserializationOption::Filter` keeps the parsed JSON
      small; adaptive 2 s / 15 s poll cadence.
- [ ] **MCP inputs** — buttons + encoder work on dedicated `mcp_input_task`.

### Phase 1.5 features (originally shipped on TFT_eSPI; verify still work
on the LVGL port)

- [ ] **Volume HUD** — turn the volume encoder, "VOL XX%" bar at top
      auto-hides after 2 s.
- [ ] **Mute badge** — press the encoder switch; HUD shows "MUTED" red for
      2 s, persistent badge in the top-right of now-playing while muted.
- [ ] **Play/pause flash** — press SW2; ▶ or ⏸ overlays the art for 1.5 s.
- [ ] **WiFi indicator** — 4-bar icon top-left, updates with RSSI.
- [ ] **SW4 seek preview** — hold SW4 + turn RE1 for `SEEK +M:SS` overlay.
      *(05-30 review flagged this might have been lost in the LVGL port —
      verify against the documented behaviour or remove from the doc.)*

---

## Cross-build — edge cases worth poking

- [ ] **Rapid play-pause spam** — press SW2 10 times in 2 s; no UI lag, no
      duplicate flashes, Spotify keeps up.
- [ ] **Volume during pause** — adjust volume while paused; HUD updates,
      PUT fires 300 ms after last detent.
- [ ] **Seek to end** — drag the progress bar (touch) or hold SW4 + RE1 to
      seek past the track end; should clamp to duration, not overflow.
- [ ] **Seek before start** — same, before 0; should clamp at 0.
- [ ] **WiFi disconnect mid-press** — yank WiFi just as you press next;
      command fails cleanly, dispatcher log fires, no crash.
- [ ] **Spotify token expiry** — let the token expire (~1 h), press a
      button; first attempt may return 401, `_do_cmd` clears the cached
      token, next call refreshes — press should work shortly after with no
      visible failure.

---

## When something fails

1. Note the symptom (what you saw, what you expected) and grab the serial
   log around the failure.
2. Open Claude Code in VS Code, paste both into chat, point at the relevant
   feature in this checklist.
3. The session will read `CLAUDE.md`, `docs/PENDING.md`, `docs/ROADMAP.md`
   automatically and know the codebase.
