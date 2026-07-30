# Spotify API Setup

Use this procedure for each firmware build that connects directly to Spotify.
The Home Assistant builds use different credentials.

> **WARNING:** Keep all credentials private. A published secret can give another
> person access to your Spotify account or network.

> **CAUTION:** Put credentials only in the applicable `secrets.h` file. Do not
> put credentials in a tracked file.

## Credential Files

| Build | Credential file |
|---|---|
| CYD Arduino | `cyd/platformio/include/secrets.h` |
| CYD ESP-IDF | `cyd/esp-idf/include/secrets.h` |
| CYD Home Assistant | `cyd/esp-idf-ha/include/secrets.h` |
| Waveshare ESP32-P4 | `waveshare/esp-idf/include/secrets.h` |

Each folder has a `secrets.h.example` template. The real credential file is
not tracked by Git.

## Create the Credential File

1. Copy `secrets.h.example` to `secrets.h` in the same folder.
2. Open the new `secrets.h` file.
3. Add each value when this procedure tells you to add it.

## Create a Spotify App

1. Open the [Spotify Developer Dashboard](https://developer.spotify.com/dashboard).
2. Sign in to Spotify.
3. Select **Create app**.
4. Enter a name for the app.
5. Enter a description for the app.
6. Set the redirect URI to `http://127.0.0.1:8888/callback`.
7. Save the app.
8. Open **Settings** for the app.
9. Copy the client ID to `SPOTIFY_CLIENT_ID`.
10. Copy the client secret to `SPOTIFY_CLIENT_SECRET`.

Use `127.0.0.1` in the redirect URI. Spotify does not accept `localhost` for
this procedure.

## Get a Refresh Token

The controller cannot open the Spotify authorization page. Use a computer to
get one refresh token.

1. Open a terminal in the repository root.
2. Install Spotipy:

   ```powershell
   pip install spotipy
   ```

3. Run the token tool:

   ```powershell
   python get_spotify_token.py
   ```

4. Enter the client ID when the tool requests it.
5. Enter the client secret when the tool requests it.
6. Complete the authorization in the browser.
7. Copy the displayed token to `SPOTIFY_REFRESH_TOKEN`.
8. Delete the `.cache` file after the procedure.

The `.cache` file can contain the refresh token. Do not publish or share this
file.

## Add Wi-Fi Credentials

1. Set `WIFI_SSID` in `secrets.h`.
2. Set `WIFI_PASSWORD` in `secrets.h`.
3. Save the file.

## Build and Flash

For a CYD Arduino build, run:

```powershell
pio run -t upload
```

For a CYD ESP-IDF build, run:

```powershell
idf.py set-target esp32
idf.py build flash monitor
```

For a Waveshare ESP32-P4 build, run:

```powershell
idf.py set-target esp32p4
idf.py build flash monitor
```

You only have to set the target during the first setup or after a clean
configuration.

## Home Assistant Builds

The Home Assistant builds do not use Spotify API credentials. They use
`HA_HOST`, `HA_PORT`, `HA_TOKEN`, and `HA_ENTITY`.

Refer to [docs/HA-SETUP.md](docs/HA-SETUP.md) for the Home Assistant
procedure.
