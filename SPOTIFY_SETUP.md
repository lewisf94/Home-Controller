# Spotify API Setup Guide

To control Spotify from your CYD without a complex server, the device needs
direct access to your account.

> **Keep your credentials private.** Your WiFi password, Spotify client secret,
> and refresh token are real, reusable credentials. Put them **only** in
> `cyd/platformio/include/secrets.h`, which is gitignored so it never gets
> committed. Do **not** paste them into `src/main.cpp` or any other tracked
> source file — that would publish them to git history. Also note the
> PlatformIO build currently sends them over the network using TLS verified
> against the device's built-in root-CA bundle; still, only set this up on a
> network you trust.

## Step 0: Create your secrets file

1. Copy `cyd/platformio/include/secrets.h.example` to
   `cyd/platformio/include/secrets.h`.
2. Fill in the five values as you obtain them in the steps below.

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
2. Click the **PlatformIO Build** button (the tick at the bottom of VS Code) and
   Upload to your CYD!
