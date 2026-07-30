# Home Assistant Setup

This guide configures the Home Assistant backend for the music controller.

The guide applies to `cyd/esp-idf-ha/`. The Waveshare HA build uses the same
Home Assistant connection values.

## Requirements

| Item | Requirement |
|---|---|
| Raspberry Pi | Raspberry Pi 4 or 5 |
| Storage | 32 GB A2 microSD card or USB SSD |
| Power supply | Correct supply for the selected Raspberry Pi |
| Network | Ethernet during initial setup |
| Spotify | Spotify Premium account |
| Development computer | ESP-IDF and a USB data cable |

Use a Raspberry Pi 5 for the best response time.

## Install Home Assistant OS

1. Install Raspberry Pi Imager.
2. Connect the storage device to the computer.
3. Select your Raspberry Pi model.
4. Select Home Assistant OS.
5. Select the storage device.

> WARNING: The next step erases the selected storage device.

6. Write the image.
7. Install the storage device in the Raspberry Pi.
8. Connect the Raspberry Pi to Ethernet.
9. Connect power.
10. Wait for the initial installation to finish.
11. Open `http://homeassistant.local:8123`.
12. Complete the onboarding process.

If the local name does not work, use the Raspberry Pi IP address.

See the official
[Home Assistant Raspberry Pi installation guide](https://www.home-assistant.io/installation/raspberrypi/)
for current installation details.

## Reserve The IP Address

The firmware uses a fixed Home Assistant address.

1. Open the router configuration page.
2. Find the Home Assistant device.
3. Create a DHCP reservation for that device.
4. Record the reserved IP address.

You can also configure a static address in Home Assistant.

1. Open **Settings > System > Network**.
2. Select the active network interface.
3. Set the IPv4 method to static.
4. Enter the address, gateway, and DNS values.
5. Restart Home Assistant.

Use one method only.

## Install Music Assistant

Music Assistant provides album playback and player control.

1. Open **Settings > Apps**.
2. Open the app store.
3. Find Music Assistant.
4. Install the Music Assistant server app.
5. Enable start on boot.
6. Start the app.
7. Open the Music Assistant interface.
8. Add Spotify as a music provider.
9. Complete Spotify authentication.
10. Add the required player provider.

Install the official Home Assistant integration after the server works.

1. Open **Settings > Devices & services**.
2. Select **Add Integration**.
3. Find Music Assistant.
4. Complete the integration setup.

Music Assistant creates `media_player` entities in Home Assistant.

Use the official
[Music Assistant installation guide](https://www.music-assistant.io/installation/).

Use the official
[Home Assistant integration guide](https://www.music-assistant.io/integration/installation/).

## Optional Native Spotify Integration

The native integration can provide basic Spotify controls.

1. Open **Settings > Devices & services**.
2. Select **Add Integration**.
3. Find Spotify.
4. Complete Spotify authentication.

The native integration does not replace Music Assistant album playback.

## Create An Access Token

> WARNING: The token gives access to your Home Assistant instance.

1. Open your Home Assistant user profile.
2. Open the security settings.
3. Find **Long-Lived Access Tokens**.
4. Select **Create Token**.
5. Enter `music-controller` as the token name.
6. Create the token.
7. Store the token in the private firmware configuration.

Home Assistant shows the token only once.

Do not commit the token to Git.

See the official
[Home Assistant authentication API](https://developers.home-assistant.io/docs/auth_api/)
for token details.

## Find The Player Entity

1. Open **Developer Tools > States**.
2. Filter the list for `media_player`.
3. Select the required Music Assistant player.
4. Record the exact entity ID.
5. Start playback on that player.
6. Confirm that the state and track data change.

Example entity:

```text
media_player.music_assistant_lewis
```

## Test The WebSocket

Install `wscat` when it is not available:

```powershell
npm install -g wscat
```

Connect to Home Assistant:

```powershell
wscat -c ws://192.168.1.50:8123/api/websocket
```

Home Assistant must return an `auth_required` message.

Send this message:

```json
{"type":"auth","access_token":"YOUR_TOKEN"}
```

Home Assistant must return an `auth_ok` message.

Replace `192.168.1.50` with the reserved address.

## Configure The Firmware

Set these private configuration values:

| Name | Value |
|---|---|
| `WIFI_SSID` | Wi-Fi network name |
| `WIFI_PASSWORD` | Wi-Fi password |
| `HA_HOST` | Reserved Home Assistant IP address |
| `HA_PORT` | `8123` |
| `HA_TOKEN` | Long-lived access token |
| `HA_ENTITY` | Exact `media_player` entity ID |

Use the numeric IP address for `HA_HOST`.

Do not use `homeassistant.local` in the firmware configuration.

## Build And Flash

Open an ESP-IDF terminal.

```powershell
Set-Location "C:\Users\lewis\Documents\home-controller\cyd\esp-idf-ha"
idf.py set-target esp32
idf.py build
idf.py -p COM5 flash monitor
```

Replace `COM5` with the device port.

Run `idf.py set-target esp32` only for a new build directory.

## First Test

Complete these checks in sequence:

- [ ] Wi-Fi connection completes.
- [ ] The WebSocket connection opens.
- [ ] Home Assistant accepts authentication.
- [ ] The configured entity exists.
- [ ] Track data appears.
- [ ] The progress bar advances.
- [ ] Play and pause work.
- [ ] Next and previous work.
- [ ] Volume control works.
- [ ] Album art appears.
- [ ] Album selection starts playback.
- [ ] The Wi-Fi indicator updates.

Stop the test after the first failure.

## Troubleshooting

### Authentication Fails

Create a new long-lived access token.

Confirm that the complete token is in the private configuration.

### The WebSocket Disconnects

Check the Home Assistant logs.

Confirm that both devices use the same local network.

Confirm that `HA_HOST` contains the reserved IP address.

### The Entity Does Not Exist

Open **Developer Tools > States**.

Copy the exact entity ID.

Restart Home Assistant after you add a new integration.

### Track Data Is Empty

Start playback on the configured player.

Confirm that the player state is not `idle` or `unavailable`.

### Album Art Is Blank

Confirm that Home Assistant is reachable from the controller.

Confirm that the entity has an `entity_picture` attribute.

Erase and reflash the device only when the storage partition is corrupt.

```powershell
idf.py erase-flash
idf.py build
idf.py -p COM5 flash monitor
```

> WARNING: `erase-flash` removes saved settings and credentials from the device.

### Album Selection Does Not Start Playback

Confirm that the Music Assistant server app is running.

Confirm that the Music Assistant integration is loaded.

Confirm that the selected player is available.

### Hardware Controls Do Not Respond

Check the serial log for the MCP23017 status.

Confirm the I2C wiring.

Confirm the external pull-up resistors.

### The Offline Indicator Appears

Check the Wi-Fi signal level.

Move the controller closer to the access point.

## Network Path

```text
Controller -> local WebSocket -> Home Assistant
Controller -> local HTTP      -> Home Assistant album art
Home Assistant                -> music services
```

The HA firmware does not call Spotify directly.

Home Assistant manages authentication and service communication.

## Update The Album List

Run the album pipeline from the repository root:

```powershell
python scripts/add_albums.py spotify:album:ALBUM_ID
```

Then rebuild and flash the required HA firmware.
