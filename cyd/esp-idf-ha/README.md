# cyd/esp-idf-ha — ESP-IDF build, Home Assistant backend

The Home Assistant variant of the CYD-IDF firmware. Shares its UI, input,
album-art and storage code with `../esp-idf/` via the
[`cyd_shared`](../components/cyd_shared/README.md) component — the only
build-local code is `main.c` (board bring-up + WebSocket task) and
`ha_client.{c,h}` (the WebSocket client to Music Assistant). Hardware is
identical to the direct-Spotify build.

> **STATUS: never hardware-tested.** All the UI/input/reliability features
> that landed on `cyd/esp-idf/` ride along here for free (because they're in
> `cyd_shared`), but no one has yet flashed this build, brought up a Pi 5
> with HA OS, and confirmed the WebSocket handshake / state push / service
> calls actually work end-to-end. First-flash checklist is in
> [`../../docs/TESTING.md`](../../docs/TESTING.md) under "CYD ESP-IDF HA".

## What's different from `cyd/esp-idf/`

| Concern | `cyd/esp-idf/` (Spotify) | `cyd/esp-idf-ha/` (HA) |
|---|---|---|
| Backend file | `main/spotify.c` | `main/ha_client.c` |
| Backend header | `main/spotify.h` (struct + API) | `main/spotify.h` (thin wrapper — see below) + `main/ha_client.h` |
| Transport | HTTPS to `api.spotify.com` (`esp_http_client` + cert bundle) | WebSocket to `ws://<HA_HOST>:<HA_PORT>/api/websocket` (`esp_websocket_client`) |
| Auth | OAuth refresh-token flow, persisted to NVS | One static long-lived access token in `secrets.h` |
| State updates | `GET /me/player` poll every 5 s (adaptive 15 s when paused) | `state_changed` push from HA — real-time |
| Album art | Spotify CDN over TLS | HA-proxied `entity_picture` over local HTTP (faster, no TLS) |
| Volume on phones | Limited by Spotify Web API on mobile (knob no-ops on Android/iOS) | Works (HA handles device targeting) |
| Secrets file | `WIFI_*` + `SPOTIFY_*` | `WIFI_*` + `HA_HOST` / `HA_PORT` / `HA_TOKEN` / `HA_ENTITY` |
| Extra IDF requirements | `esp_http_client` | `esp_websocket_client` |

The `main/spotify.h` here is a thin wrapper that just `#include "player.h"`
(from `cyd_shared`) so `ha_client.c` and the shared UI both see the same
`spotify_track_t` struct. The HA build doesn't actually link any Spotify Web
API client — the file's filename is historical.

## Shared with `cyd/esp-idf/` (via `cyd_shared`)

Everything in the table below comes for free in this build because it lives
in [`../components/cyd_shared/`](../components/cyd_shared/README.md):

- LVGL UI: album browser, now-playing screen, volume HUD, OFFLINE indicator,
  auto-snap to playing album, MAX_CARDS truncation warning, toast widget,
  WiFi-bars indicator.
- Input: MCP23017 driver (with re-probe-on-failure), debounced buttons +
  RE1 encoder, volume base seeded from device, mute toggle.
- Storage: LittleFS scratch partition mount.
- Album art: JPEGDEC decode of the now-playing image to RGB565.

What the HA `ha_client.c` is responsible for filling in:
- The `spotify_track_t` struct (call `ui_set_track_info` after every
  `state_changed` event).
- The album-art URL field, pointing at the HA-proxied `entity_picture`.
- Implementing the same set of `*_play_album` / `*_toggle_play_pause` /
  `*_next` / `*_prev` / `*_seek` / `*_set_volume` / `*_get_devices` /
  `*_transfer_playback` semantics that the dispatcher in `main.c` expects.

## Setup (HA side)

1. Install HA OS on a Pi 5. Add the Spotify (or Music Assistant) integration.
   Note the entity ID: typically `media_player.spotify_<username>` or
   `media_player.<mass_user>`.
2. Create a long-lived access token: HA Profile → Long-Lived Access Tokens
   → Create. Store in `include/secrets.h` as `HA_TOKEN`.
3. Confirm Pi 5 is reachable on the LAN at a stable IP or hostname. Set
   `HA_HOST` + `HA_PORT` (default 8123) + `HA_ENTITY` to your media-player
   entity id.

## Setup (device side)

1. `cp include/secrets.h.example include/secrets.h` and fill in
   `WIFI_SSID` / `WIFI_PASSWORD` / `HA_HOST` / `HA_PORT` / `HA_TOKEN` /
   `HA_ENTITY`. File is gitignored.
2. Make sure `idf.py set-target esp32` has been run at least once.
3. `idf.py build flash monitor` — first build downloads `esp_websocket_client`
   and the rest of the managed-component graph.

## Build / flash / monitor

Same as `cyd/esp-idf/` — open this folder in VS Code with the ESP-IDF
extension (the extension wants the IDF project at the workspace root, not
the repo root), set target `esp32`, pick the COM port, click the flame icon
to build + flash + monitor.

From the IDF PowerShell:

```powershell
idf.py set-target esp32           # first time only
idf.py build
idf.py -p COM5 flash monitor      # replace COM5 with your port
```

## Build configuration

The CMake wiring matches `cyd/esp-idf/` after the `cyd_shared` extraction:

- Top-level `CMakeLists.txt` adds `list(APPEND EXTRA_COMPONENT_DIRS ../components)`
  before the `project()` call so IDF discovers `cyd_shared`.
- `main/CMakeLists.txt` lists `cyd_shared` in `REQUIRES`; the only sources
  it builds itself are `main.c`, `ha_client.c`, `albums.c` (generated), and
  `album_thumbs.c` (with `EMBED_FILES "album_thumbs.bin"`).

## Project memory

- Hardware pin map, I2C addresses, architecture, coding conventions:
  [`../../CLAUDE.md`](../../CLAUDE.md).
- Phased plan (incl. Phase 3 HA backend rationale):
  [`../../docs/ROADMAP.md`](../../docs/ROADMAP.md).
- IDF port gotchas: [`../../docs/PORT-NOTES.md`](../../docs/PORT-NOTES.md).
- What's not yet verified and deferred work:
  [`../../docs/PENDING.md`](../../docs/PENDING.md).
- Test plan: [`../../docs/TESTING.md`](../../docs/TESTING.md).
