# CYD ESP-IDF Home Assistant Build

This firmware uses the CYD interface with a Home Assistant backend. It receives
player state through a WebSocket connection.

The build is not hardware-verified. Complete the first-flash checks in
[TESTING.md](../../docs/TESTING.md).

## Differences from the Spotify Build

| Function | Direct Spotify | Home Assistant |
|---|---|---|
| Backend source | `main/spotify.c` | `main/ha_client.c` |
| Transport | HTTPS | Home Assistant WebSocket |
| Authorization | Spotify refresh token | Home Assistant access token |
| State updates | Periodic request | `state_changed` event |
| Album art | Spotify CDN | Home Assistant proxy |
| Volume control | Spotify device limits apply | Home Assistant selects the device |

The file `main/spotify.h` includes the shared player contract. Its name remains
for compatibility.

## Shared Functions

The [`cyd_shared`](../components/cyd_shared/README.md) component supplies:

- LVGL album browser and now-playing view.
- Volume and network overlays.
- MCP23017 input handling.
- Button and encoder debounce.
- LittleFS storage.
- JPEG album-art decoding.

The Home Assistant client must publish a complete `spotify_track_t` value after
each applicable state event.

The client must implement the control functions that `main.c` requests. Keep
all network calls outside the LVGL and input tasks.

## Home Assistant Setup

1. Install Home Assistant OS.
2. Install Music Assistant or the required media integration.
3. Find the applicable `media_player` entity.
4. Create a long-lived Home Assistant access token.
5. Record the server address and port.
6. Complete [HA-SETUP.md](../../docs/HA-SETUP.md).

## Device Setup

1. Create the private credential file from its example.
2. Set `WIFI_SSID`.
3. Set `WIFI_PASSWORD`.
4. Set `HA_HOST`.
5. Set `HA_PORT`.
6. Set `HA_TOKEN`.
7. Set `HA_ENTITY`.

Do not commit the credential file.

## Build Procedure

1. Open an ESP-IDF 6.0 terminal.
2. Change to this folder.
3. Set the target:

   ```powershell
   idf.py set-target esp32
   ```

4. Build:

   ```powershell
   idf.py build
   ```

5. Flash and monitor:

   ```powershell
   idf.py -p COM5 flash monitor
   ```

Replace `COM5` with the correct port.

The first build can download the WebSocket component and other managed
components.

## First Hardware Check

Confirm these results in order:

1. The display starts.
2. Touch input operates.
3. Wi-Fi gets an address.
4. The WebSocket connects.
5. Home Assistant accepts the token.
6. The configured entity exists.
7. Player state appears.
8. Album art appears.
9. Each control sends the expected service call.
10. The controller reconnects after a network interruption.

## Related Documents

- [Home Assistant setup](../../docs/HA-SETUP.md)
- [Hardware tests](../../docs/TESTING.md)
- [Project memory](../../CLAUDE.md)
- [Roadmap](../../docs/ROADMAP.md)
- [Pending work](../../docs/PENDING.md)
