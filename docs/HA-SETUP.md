# Home Assistant Setup Guide — Music Controller

This guide covers everything needed to run the Home Assistant backend
end-to-end on either the **CYD** (`cyd/esp-idf-ha/`) or the **Waveshare
ESP32-P4** (`waveshare/esp-idf-ha/`) board: installing HA OS on a Pi 5,
wiring in Music Assistant, creating the credentials the firmware needs, and
flashing the device. Parts 1–6 are identical for both boards. Parts 7–8
note any board-specific differences.

---

## Prerequisites

| What | Minimum | Notes |
|---|---|---|
| Raspberry Pi | Pi 4 (2 GB) | Pi 5 recommended for responsiveness |
| microSD card | 32 GB class A2 | Or USB SSD for Pi 5 |
| USB-C power supply | 27 W (Pi 5) | Official PSU strongly recommended |
| Router access | — | Need to assign a static IP or DHCP reservation |
| Spotify Premium | — | Required by Spotify integration |
| Active Spotify device | — | Phone / speaker on the same account |

---

## Part 1 — Install Home Assistant OS on the Pi 5

1. Download and install **Raspberry Pi Imager** from raspberrypi.com.

2. Insert the microSD card (or connect USB SSD).

3. In Imager:
   - **Device:** Raspberry Pi 5
   - **OS:** Other specific-purpose OS → Home Assistants and Home Automation →
     **Home Assistant**
   - **Storage:** your SD / SSD

4. Click **Write**. Do **not** apply any OS customisation (HA manages its own
   first-boot setup).

5. Insert the card into the Pi 5, connect Ethernet, and power on. Wait
   3–5 minutes for first boot (it expands the partition and downloads updates).

6. Navigate to **http://homeassistant.local:8123** in a browser. If the
   `.local` name doesn't resolve, find the Pi's IP from your router and use
   that directly (e.g. `http://192.168.1.50:8123`).

7. Complete the onboarding wizard (create your account, set your location/timezone).

---

## Part 2 — Assign a static IP to the Pi

The firmware hard-codes `HA_HOST` at compile time. A DHCP lease can change,
so give the Pi a fixed address now.

**Option A — DHCP reservation (router side):**
Find the Pi's MAC address in your router's DHCP client list and bind it to a
fixed IP (e.g. `192.168.1.50`). This is the simplest option.

**Option B — Static IP in HA OS:**
Settings → System → Network → IPv4 → change from DHCP to **Static**, enter
the address/gateway/DNS. Requires HA restart.

---

## Part 3 — Install Music Assistant

Music Assistant is a separate add-on that gives the firmware proper album
playback. The firmware calls `music_assistant.play_media` when you tap an
album — without MA installed, album selection does nothing (play/pause, next,
prev, and volume still work via the entity state).

Music Assistant is not in the default add-on store, so you need to add its
repository first.

1. In HA: **Settings → Add-ons → ADD-ON STORE** (bottom right).

2. Click the **three-dot menu** (top right of the store page) → **Repositories**.

3. Paste this URL and click **Add**:
   ```
   https://music-assistant.io/hassio-repository/
   ```

4. Close the dialog. The store reloads — search for **Music Assistant** and
   install it.

5. Once installed: enable **Start on boot** and **Watchdog**, then click **Start**.

6. Click **Open Web UI** (or go to `http://<your-ha-ip>:8095`).

7. Complete the setup wizard:
   - Add **Spotify** as a Music Provider and log in with your Spotify account.
   - Your Sonos (or other) speakers should auto-discover as Player Providers —
     select whichever you want to use.

8. After setup, go to **Settings → Devices & Services → Add Integration** in HA
   and add the **Music Assistant** integration. This is a separate step from the
   add-on and is what creates the `media_player` entities in HA.

9. Go to **Developer Tools → States**, filter by `media_player`. New entities
   managed by Music Assistant will appear — named after your speakers
   (e.g. `media_player.living_room`). Note the entity ID; it goes into
   `HA_ENTITY` in `secrets.h`.

---

## Part 4 — Native Spotify integration and Spotify Connect targets

The controller uses the native HA Spotify integration to list and transfer to
Spotify Connect targets such as a phone, laptop, or Spotify speaker. Music
Assistant remains responsible for the Home Controller's native Sendspin player
and for normal player management.

1. In the [Spotify Developer Dashboard](https://developer.spotify.com/dashboard),
   open the app whose Client ID and Client Secret you will give to HA. Add this
   exact Redirect URI and save it:
   ```
   https://my.home-assistant.io/redirect/oauth
   ```
   If My Home Assistant is disabled, use
   `https://<your-ha-url>/auth/external/callback` instead; it must exactly match
   the URL shown by HA during authorisation.

2. Settings → Devices & Services → **+ Add Integration** → search **Spotify**.
   Enter that Spotify app's Client ID and Client Secret, then complete the OAuth
   popup. Spotify Premium is required.

3. HA stores the tokens and refreshes them automatically — the firmware never
   needs to handle Spotify OAuth. The entity created is typically
   `media_player.spotify_<your_username>`.

4. Its `source_list` contains Spotify Connect devices currently visible to
   Spotify. The controller expands those into rows labelled **SPOTIFY CONNECT**.
   A phone normally appears only while the Spotify app has been opened recently;
   Spotify does not expose sleeping/inactive phones as targets.

### Spotify Connect plugin versus controller output selection

Music Assistant's **Spotify Connect** plugin is useful for making a Music
Assistant player appear inside the Spotify phone app. Configure it with the
native MA player named **Home Controller** to send music from Spotify to the
device speaker.

That plugin does not provide the controller's list of other Spotify Connect
devices. For that, keep the native HA Spotify integration from this section
authenticated. After flashing, tap the output name on Now Playing: the list
shows Music Assistant players, the Spotify account, and its current Spotify
Connect sources separately.

> **Note:** With the native integration, tapping an album in the browser sends
> `music_assistant.play_media` which will fail if MA is not installed. You can
> still use the browser to browse albums visually and control playback with the
> buttons/encoder.

---

## Part 5 — Create a Long-Lived Access Token

1. In HA, click your **profile picture** (bottom left of the sidebar).

2. Scroll down to **Long-Lived Access Tokens** → **Create Token**.

3. Give it a name (e.g. `music-controller`) and click **OK**.

4. **Copy the token immediately** — it is shown only once.

5. Paste it into `cyd/esp-idf-ha/include/secrets.h` as `HA_TOKEN` (see Part 7).

---

## Part 6 — Verify the entity ID and WebSocket reachability

Before flashing, confirm the setup from a computer on the same network.

**Test WebSocket connection:**
```bash
# Install wscat if needed: npm install -g wscat
wscat -c ws://192.168.1.50:8123/api/websocket
```
You should immediately receive:
```json
{"type":"auth_required","ha_version":"..."}
```
Send:
```json
{"type":"auth","access_token":"YOUR_TOKEN_HERE"}
```
Expected response:
```json
{"type":"auth_ok","ha_version":"..."}
```
If you get `auth_invalid`, the token is wrong or was not copied in full.

**Confirm the entity ID:**
In HA Developer Tools → States, filter by `media_player`. Find your player
entity (Music Assistant or Spotify) and note the exact `entity_id` string.
Play something in Spotify so the entity has a current state — if it reads
`unavailable` or `idle`, the state push will have no track info to show.

**Test art download (optional):**
After playing a track, find `entity_picture` in the state attributes. Fetch it:
```bash
curl -H "Authorization: Bearer YOUR_TOKEN" \
     http://192.168.1.50:8123$(entity_picture_path) --output test.jpg
```
If you get a JPEG, art download will work on the device.

---

## Part 7 — Configure and flash the firmware

### 7a — CYD board (`cyd/esp-idf-ha/`)

1. **Copy the secrets template** (in the private build folder):
   ```
   cyd/esp-idf-ha/include/secrets.h.example  →  cyd/esp-idf-ha/include/secrets.h
   ```

2. **Fill in your values:**
   ```c
   #define WIFI_SSID     "YourNetworkName"
   #define WIFI_PASSWORD "YourPassword"

   #define HA_HOST    "192.168.1.50"   // Pi 5 static IP (not hostname)
   #define HA_PORT    8123
   #define HA_TOKEN   "eyJhbGci..."    // Long-lived token from Part 5
   #define HA_ENTITY  "media_player.living_room"  // entity ID from Part 3
   ```

3. **Open the ESP-IDF terminal** in VS Code (not plain PowerShell).

4. **Set the target** (first time only):
   ```
   idf.py set-target esp32
   ```

5. **Build and flash:**
   ```
   idf.py build flash monitor
   ```

### 7b — Waveshare ESP32-P4 board (`waveshare/esp-idf-ha/`)

1. **Copy the secrets template** (in the private build folder):
   ```
   waveshare/esp-idf-ha/include/secrets.h.example  →  waveshare/esp-idf-ha/include/secrets.h
   ```
   Never edit the `include/` folder in the public repo — credentials live only
   in the private build copy.

2. **Fill in your values:**
   ```c
   #define WIFI_SSID     "YourNetworkName"
   #define WIFI_PASSWORD "YourPassword"

   #define HA_HOST    "192.168.1.50"   // Pi 5 static IP (not hostname)
   #define HA_PORT    8123
   #define HA_TOKEN   "eyJhbGci..."    // Long-lived token from Part 5
   #define HA_ENTITY  "media_player.living_room"  // entity ID from Part 3
   ```
   Use the IP address, not `homeassistant.local` — mDNS is not reliable on
   all networks and the firmware uses a plain IP string.

3. **Open the ESP-IDF terminal** in VS Code. The Waveshare build requires
   **ESP-IDF 5.5.x** — use the IDF 5.5.4 terminal profile, not IDF 6.x.

4. **Navigate to your private build folder:**
   ```
   cd "C:\Users\User\Documents\home-controller - Private\waveshare\esp-idf-ha"
   ```

5. **Set the target** (first time only):
   ```
   idf.py set-target esp32p4
   ```

6. **Build and flash:**
   ```
   idf.py build flash monitor
   ```
   The board enumerates as a CH343 USB-serial adapter (COM3 or COM4 on Windows).

### Expected serial output on a successful boot

```
I (main): display up
I (main): WiFi connected
I (ha): ws started -> ws://192.168.1.50:8123/api/websocket
I (ha): authenticated
I (ha): state: Artist Name - Track Title [playing]
```

---

## Part 8 — VS Code / ESP-IDF extension

If you prefer the IDE workflow instead of the terminal:

1. Open the build folder directly as the VS Code workspace
   (File → Open Folder → select `cyd/esp-idf-ha` or `waveshare/esp-idf-ha`).
   The extension needs the IDF project at the workspace root.

2. Set the ESP-IDF path in the extension settings if prompted.

3. Use the status-bar buttons: **Set target** (esp32 for CYD, esp32p4 for
   Waveshare) → **Build** → **Flash** → **Monitor**.

---

## Part 9 — First-run checks

Work through this list in order. Stop at the first failure and consult the
troubleshooting section below.

- [ ] WiFi connected (IP printed in log)
- [ ] WebSocket connected (`I (ha): ws started`)
- [ ] Authenticated (`I (ha): authenticated`)
- [ ] Entity state received (`I (ha): state: ...`)
- [ ] Now-playing screen updates when you play something in Spotify
- [ ] Progress bar advances while playing
- [ ] Play/pause button (SW2) toggles Spotify playback
- [ ] Next (SW3) and previous (SW1) buttons work
- [ ] RE1 encoder adjusts volume (now-playing screen) and scrolls the browser
- [ ] Album art appears and changes on track change
- [ ] Tapping an album in the browser plays it (requires Music Assistant)
- [ ] Volume HUD appears on encoder turn; "MUTED" shows on RE1 push
- [ ] WiFi bars update in the top-left corner

### Waveshare Sendspin check

- [ ] Music Assistant discovers **Home Controller** as a Sendspin player.
- [ ] If discovery has not appeared after a minute, add it manually in Music
  Assistant using `ws://<controller-ip>:8928/sendspin`.
- [ ] Expose that MA player to Home Assistant so it appears in the controller's
  output list. Selecting it routes MA -> Sendspin -> the built-in ES8311 speaker.
- [ ] Open Spotify on the phone before opening the controller's output picker;
  the phone should then appear as a **SPOTIFY CONNECT** row once HA Spotify is
  authenticated.

---

## Troubleshooting

### `auth_invalid` in serial log
The token in `secrets.h` is wrong, truncated, or the token was revoked.
Create a new long-lived token in HA (Part 5) and reflash.

### WebSocket connects then immediately disconnects
HA may be restarting or the entity is misconfigured. Check the HA log
(Settings → System → Logs). Confirm the Pi is on the same LAN as the ESP32.

### `entity X not found in get_states`
The entity ID in `HA_ENTITY` doesn't match any entity in HA. Go to
Developer Tools → States and check the exact string, including underscores
and case. Restart HA after adding a new integration if the entity is new.

### Devices stays on `Scanning...` or reports an error
The current firmware times out after ten seconds and reports whether HA
disconnected, did not respond, or sent a state snapshot too large for the
device. Check the serial log for `ha: device discovery`, `ws disconnected`, or
`ws frame too large`. Confirm the long-lived token has not been revoked and
that the HA Spotify integration and Music Assistant integration have completed
their setup. The controller lists only available `media_player.*` entities;
unavailable entities are deliberately skipped.

### Now-playing screen shows nothing / "Nothing playing"
The entity exists but has state `idle` or `unavailable`. Play something in
Spotify from any device first. If using Music Assistant, confirm the player
provider is configured and online in MA settings.

### Album art missing or blank
Most likely the Authorization header issue — but that's already fixed in the
firmware (Part 7). Other causes:
- The entity_picture URL requires HA to be reachable (check `HA_HOST`/`HA_PORT`).
- The LittleFS partition may not have been formatted. On first boot it
  auto-formats, but a corrupt flash state can prevent it. Run `idf.py erase-flash`
  then reflash.

### Tapping an album does nothing (no playback starts)
Music Assistant is not installed or the `music_assistant.play_media` service
is unavailable. Check HA Developer Tools → Services, search for
`music_assistant.play_media`. If it doesn't appear, install the MA add-on
(Part 3).

### Volume changes have no effect (phone/Android/iOS)
This is expected with the **native** Spotify integration — Spotify's API
does not relay volume changes to mobile clients. Music Assistant routes volume
through the player provider instead and does not have this restriction.

### Controls unresponsive but display works (CYD only)
The MCP23017 IO expander may not have been found at boot. Check the serial log
for `mcp_input: MCP23017 not found`. The driver re-probes every 5 s — wait a
few seconds. If it never appears, check the I2C wiring (SDA=GPIO27, SCL=GPIO22)
and the 4.7 kΩ pull-ups to 3.3 V.

The Waveshare build is touch-only (no MCP23017); unresponsive touch is usually
a misconfigured BSP touch-flag or a dead digitiser cable.

### `OFFLINE` banner appears despite WiFi being connected
The WiFi RSSI dropped to 0 (the WiFi-bars indicator drives this). The device
is still connected but signal is weak. Move it closer to the router or add
an access point.

---

## Network topology notes

The firmware talks to HA on the **local network only** — no cloud, no Spotify
API calls. All traffic is:

```
ESP32 ──WebSocket (ws:// port 8123)──► Pi 5 (HA OS)
ESP32 ──HTTP (port 8123, album art)──► Pi 5 (HA OS)
Pi 5  ──HTTPS──────────────────────► Spotify / Music providers
```

Playback control and Spotify Connect output switching stay local through HA and
Music Assistant. The one exception is the Add Albums search: it uses Spotify
client credentials to search Spotify's public album catalogue directly from the
controller, because HA/MA libraries need not contain every Spotify album. This
means:

- The device works even if Spotify changes its API.
- Volume control works on mobile (HA routes it correctly).
- Multiple music sources (local files, Tidal, etc.) are possible by adding
  providers to Music Assistant without changing the firmware.

---

## Updating the album list

The album browser thumbnails are baked in at compile time. To add albums:

```bash
# From repo root
python scripts/add_albums.py spotify:album:<ID>

# Then rebuild and reflash — pick your build:
cd cyd/esp-idf-ha && idf.py build flash         # CYD
cd waveshare/esp-idf-ha && idf.py build flash   # Waveshare P4
```

See `scripts/` and `CLAUDE.md` (Coding conventions) for the full pipeline.
