# Home Assistant Setup Guide — Music Controller (CYD)

This guide covers everything needed to run the Home Assistant backend
(`cyd/esp-idf-ha/`) end-to-end: installing HA OS on a Pi 5, wiring in
Music Assistant (recommended) or the native Spotify integration, creating
the credentials the firmware needs, and flashing the device.

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

## Part 3 — Install Music Assistant (recommended)

Music Assistant is a separate add-on that gives the firmware proper album
playback via `play_media`. Without it the album browser still builds
correctly, but tapping an album to play it will not work (play/pause, next,
prev, and volume all still function via the native Spotify integration).

1. In HA: **Settings → Add-ons → ADD-ON STORE** (bottom right).

2. Search for **Music Assistant**. If it doesn't appear, first install HACS
   (Home Assistant Community Store) by following the HACS documentation, then
   find Music Assistant through HACS.

3. Install the add-on, enable **Start on boot**, click **Start**.

4. Open the Music Assistant UI (sidebar or via the add-on page).

5. In MA Settings → Music providers → **+ Add** → **Spotify**:
   - Enter your Spotify `client_id` and `client_secret` (from
     developer.spotify.com → your app → Settings).
   - Authenticate via the OAuth popup.

6. In MA Settings → Player providers → **+ Add** → pick your playback
   target (e.g. Spotify Connect player, Snapcast, etc.).

7. Music Assistant creates `media_player` entities in HA automatically.
   Find them at **Developer Tools → States**, filter by `media_player`.
   Note the entity ID (e.g. `media_player.music_assistant_lewis`). This
   goes into `HA_ENTITY` in `secrets.h`.

---

## Part 4 — Native Spotify integration (alternative to / alongside MA)

If you want the native Spotify integration for play/pause/next/prev/volume
(without Music Assistant album playback), or alongside MA:

1. Settings → Devices & Services → **+ Add Integration** → search **Spotify**.

2. Authenticate via OAuth in the popup. HA stores the tokens and refreshes
   them automatically — the firmware never needs to deal with OAuth.

3. The entity created is typically `media_player.spotify_<your_username>`.

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

1. **Copy the secrets template:**
   ```bash
   cp cyd/esp-idf-ha/include/secrets.h.example cyd/esp-idf-ha/include/secrets.h
   ```

2. **Fill in your values** (`cyd/esp-idf-ha/include/secrets.h`):
   ```c
   #define WIFI_SSID     "YourNetworkName"
   #define WIFI_PASSWORD "YourPassword"

   #define HA_HOST    "192.168.1.50"   // Pi 5 static IP (not hostname)
   #define HA_PORT    8123
   #define HA_TOKEN   "eyJhbGci..."    // Long-lived token from Part 5
   #define HA_ENTITY  "media_player.music_assistant_lewis"  // from Part 3/4
   ```
   Use the IP address rather than `homeassistant.local` — mDNS resolution
   is not reliable on all networks and the firmware uses a plain IP string.

3. **Set the ESP-IDF target** (first time only):
   ```bash
   cd cyd/esp-idf-ha
   idf.py set-target esp32
   ```

4. **Build, flash, and monitor:**
   ```bash
   idf.py build
   idf.py -p COM5 flash monitor    # replace COM5 with your port
   ```
   On Linux: `/dev/ttyUSB0` or `/dev/ttyACM0`. On macOS: `/dev/cu.usbserial-*`.

5. **Expected serial output on a successful first boot:**
   ```
   I (main): wifi connecting to "YourNetwork"...
   I (main): wifi connected, IP: 192.168.1.xxx
   I (ha): ws started -> ws://192.168.1.50:8123/api/websocket
   I (ha): authenticated
   I (ha): state: Artist -- Title [playing]
   ```

---

## Part 8 — VS Code / ESP-IDF extension

If you prefer the IDE workflow:

1. Open the **`cyd/esp-idf-ha/`** folder directly as the VS Code workspace
   (File → Open Folder → select `cyd/esp-idf-ha`). The extension needs the
   IDF project at the workspace root.

2. Set the ESP-IDF path in the extension settings if prompted.

3. Use the status-bar buttons: **Set target** (esp32) → **Build** → **Flash** →
   **Monitor**.

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

### Controls unresponsive but display works
The MCP23017 IO expander may not have been found at boot. Check the serial log
for `mcp_input: MCP23017 not found`. The driver re-probes every 5 s — wait a
few seconds. If it never appears, check the I2C wiring (SDA=GPIO27, SCL=GPIO22)
and the 4.7 kΩ pull-ups to 3.3 V.

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

The ESP32 never calls Spotify directly in this build. Spotify tokens, OAuth,
and CDN calls are all handled by HA/MA on the Pi. This means:

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
# Then rebuild and reflash cyd/esp-idf-ha
cd cyd/esp-idf-ha && idf.py build flash
```

See `scripts/` and `CLAUDE.md` (Coding conventions) for the full pipeline.
