# Spotify API Setup Guide

To control Spotify directly from the device (without a Home-Assistant
intermediary), the device needs Spotify Web API credentials and a refresh
token. Same Spotify steps for every direct-Spotify build — only the file
path for `secrets.h` differs per build.

> **Keep your credentials private.** Your WiFi password, Spotify client secret,
> and refresh token are real, reusable credentials. Put them **only** in the
> build's gitignored `secrets.h` — listed below per build. Do **not** paste
> them into any tracked source file; that would publish them to git history.
> Every build uses TLS against the device's embedded root-CA bundle, but
> still: only set this up on a network you trust.

## Where `secrets.h` lives, per build

| Build | Path to `secrets.h` (gitignored) | Example template |
|---|---|---|
| CYD Arduino (`cyd/platformio/`) | `cyd/platformio/include/secrets.h` | `secrets.h.example` alongside |
| CYD ESP-IDF (`cyd/esp-idf/`) | `cyd/esp-idf/include/secrets.h` | `secrets.h.example` alongside |
| CYD ESP-IDF HA (`cyd/esp-idf-ha/`) | `cyd/esp-idf-ha/include/secrets.h` — *needs HA_HOST/HA_PORT/HA_TOKEN/HA_ENTITY instead of SPOTIFY_*, see `secrets.h.example`* | as the file says |
| Waveshare ESP32-P4 (`waveshare/esp-idf/`) | `waveshare/esp-idf/include/secrets.h` | `secrets.h.example` alongside |

## Step 0: Create your secrets file

1. Copy the build's `include/secrets.h.example` to `include/secrets.h` in the
   same folder. (The example shows exactly which `#define`s the build expects.)
2. Fill in the values as you obtain them in the steps below.

> The `include/` folder is gitignored as a whole on the IDF builds, which is
> why `secrets.h` is safe to put there. On Arduino, only `secrets.h` itself
> is gitignored — don't move other files into `include/` expecting them to be
> ignored.

## Step 1: Create a Spotify App
1. Go to the [Spotify Developer Dashboard](https://developer.spotify.com/dashboard).
2. Log in and click **Create app**.
3. Fill in the required details:
    *   **App Name:** (e.g., "CYD Music Controller")
    *   **App Description:** (Whatever you like)
    *   **Redirect URI:** Enter exactly `http://127.0.0.1:8888/callback` (no quotes).
        > *Note: use `127.0.0.1`, not `localhost` — Spotify rejects `localhost`.*
4. Once created, click **Settings** on your app page.
5. Copy the **Client ID** and **Client Secret** (click "View client secret").
    * Put these in `SPOTIFY_CLIENT_ID` and `SPOTIFY_CLIENT_SECRET` in `secrets.h`.

## Step 2: Get a Refresh Token
Since the CYD cannot open a web browser to log you in, generate a "Refresh
Token" once on your computer.

1. Open a terminal in this project folder.
2. Install the required library:
   `pip install spotipy`
3. Run the helper script:
   `python get_spotify_token.py`
4. The script will ask for your **Client ID** and **Client Secret**. Paste them in.
5. A browser opens asking you to connect to Spotify. Click **Agree**.
6. The script prints your **Refresh Token**.
   * Put it in `SPOTIFY_REFRESH_TOKEN` in `secrets.h`.
   * The script may leave a `.cache` file in the folder — it contains the
     refresh token, so delete it when done (it is gitignored, but don't share it).

## Step 3: Add WiFi & Build
1. Set `WIFI_SSID` and `WIFI_PASSWORD` in `secrets.h`.
2. Build + flash whichever build you're using:
   - Arduino: PlatformIO build/upload button (or `pio run -t upload`).
   - ESP-IDF (CYD or Waveshare): `idf.py build flash monitor` from the build's
     folder. For the waveshare, make sure you've run
     `idf.py set-target esp32p4` first; for CYD, `idf.py set-target esp32`.

## HA build doesn't need any of this

The HA-backend build (`cyd/esp-idf-ha/`) talks to Home Assistant, not Spotify
directly. It needs `HA_HOST` / `HA_PORT` / `HA_TOKEN` / `HA_ENTITY` in its
`secrets.h`, not the Spotify credentials above. See its README for setup.
