#include "spotify.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include "mbedtls/base64.h"
#include <SD.h>
#include "ui.h"

static const char* wifi_ssid;
static const char* wifi_password;
static const char* client_id;
static const char* client_secret;
static const char* refresh_token;

static String access_token = "";
static unsigned long token_expiry = 0;

// Most recently observed playback device id, captured from /v1/me/player polls.
// Used to wake a paused-but-idle device when Play is pressed (Spotify drops
// the active-device association after a short idle and returns 404 to
// /me/player/play until playback is transferred back).
static String last_device_id = "";

SpotifyTrackInfo current_track_info = {false, "", "", "", 0, 0, "", -1, false, 50};
bool track_info_updated = false;
int  current_volume_pct = 50;

// Verify TLS against the Arduino-ESP32 built-in root CA bundle (the same
// trusted-roots approach the IDF build uses via esp_crt_bundle). The bundle is
// embedded by the core; this symbol marks its start. Used by every
// WiFiClientSecure below via client->setCACertBundle(...).
extern const uint8_t rootca_crt_bundle_start[] asm("_binary_certs_x509_crt_bundle_start");

static String base64_encode(String text) {
    unsigned char output[256];
    size_t olen;
    mbedtls_base64_encode(output, 256, &olen,
                          (const unsigned char*)text.c_str(), text.length());
    return String((char*)output, olen);
}

static void download_album_art(const char* url) {
    // Stage 1 of the LVGL port: now-playing art is drawn from the embedded
    // album thumbnail (set by ui on play), so the dynamic SD-cached JPEG path
    // is disabled. It also shared the VSPI bus with the LVGL display flush
    // across tasks; dynamic art will return in a later stage that decodes to
    // an RGB565 RAM buffer (no SD) and publishes via ui_art_refresh().
    (void)url;
    return;

    if (WiFi.status() != WL_CONNECTED) return;

    ui_suspend_sprite();
    WiFiClientSecure *client = new WiFiClientSecure;
    client->setCACertBundle(rootca_crt_bundle_start);
    HTTPClient https;
    https.setTimeout(2000);
    if (https.begin(*client, url)) {
        int httpCode = https.GET();
        if (httpCode == HTTP_CODE_OK) {
            SD.remove("/sd_card_albums/nowplaying.jpg");
            File f = SD.open("/sd_card_albums/nowplaying.jpg", FILE_WRITE);
            if (f) {
                https.writeToStream(&f);
                f.close();
                Serial.println("Album art downloaded to /sd_card_albums/nowplaying.jpg");
            } else {
                Serial.println("Failed to open /sd_card_albums/nowplaying.jpg for writing");
            }
        }
    }
    https.end();
    delete client;
    ui_resume_sprite();
}

// ── Auth ───────────────────────────────────────────────────────────────────
void spotify_init(const char* ssid, const char* password, const char* clientId,
                  const char* clientSecret, const char* refreshToken)
{
    wifi_ssid     = ssid;
    wifi_password = password;
    client_id     = clientId;
    client_secret = clientSecret;
    refresh_token = refreshToken;

    Serial.print("Connecting to WiFi: ");
    Serial.println(wifi_ssid);
    WiFi.begin(wifi_ssid, wifi_password);
}

static void refresh_access_token() {
    if (WiFi.status() != WL_CONNECTED) return;

    ui_suspend_sprite();
    WiFiClientSecure *client = new WiFiClientSecure;
    client->setCACertBundle(rootca_crt_bundle_start);

    HTTPClient https;
    https.setTimeout(2000);
    if (https.begin(*client, "https://accounts.spotify.com/api/token")) {
        https.addHeader("Content-Type", "application/x-www-form-urlencoded");
        String auth_str = String(client_id) + ":" + String(client_secret);
        https.addHeader("Authorization", "Basic " + base64_encode(auth_str));

        String payload = "grant_type=refresh_token&refresh_token=" + String(refresh_token);
        int httpCode = https.POST(payload);

        if (httpCode == HTTP_CODE_OK) {
            String response = https.getString();
            JsonDocument doc;
            deserializeJson(doc, response);
            access_token = doc["access_token"].as<String>();
            token_expiry = millis() + (doc["expires_in"].as<int>() * 1000) - 60000;
            Serial.println("Spotify token refreshed");
        } else {
            // Don't print the response body: the token-endpoint reply can
            // contain credentials. Status code only.
            Serial.printf("Spotify auth failed: %d\n", httpCode);
        }
        https.end();
    }
    delete client;
    ui_resume_sprite();
}

// ── Periodic update ────────────────────────────────────────────────────────
void spotify_update() {
    if (WiFi.status() != WL_CONNECTED) return;

    static bool printed_connected = false;
    if (!printed_connected) {
        Serial.println("WiFi connected!");
        printed_connected = true;
    }

    static unsigned long last_auth_attempt = 0;
    static bool auth_attempted = false;

    if (access_token == "" || millis() > token_expiry) {
        if (!auth_attempted || millis() - last_auth_attempt > 5000) {
            auth_attempted = true;
            last_auth_attempt = millis();
            refresh_access_token();
        }
    }

    static unsigned long last_fetch = 0;
    if (access_token != "" && millis() - last_fetch > 2000) {
        last_fetch = millis();
        spotify_fetch_player_state();
    }
}

// ── Player state poll ──────────────────────────────────────────────────────
// Uses /v1/me/player (superset of /currently-playing) — includes shuffle_state
// and device.volume_percent which are not available on the /currently-playing endpoint.
void spotify_fetch_player_state() {
    if (access_token == "" || WiFi.status() != WL_CONNECTED) return;

    ui_suspend_sprite();
    WiFiClientSecure *client = new WiFiClientSecure;
    client->setCACertBundle(rootca_crt_bundle_start);

    HTTPClient https;
    https.setTimeout(2000);
    if (https.begin(*client, "https://api.spotify.com/v1/me/player")) {
        https.addHeader("Authorization", "Bearer " + access_token);
        int httpCode = https.GET();

        if (httpCode == HTTP_CODE_OK) {
            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, https.getStream());
            if (!error && doc["item"]) {
                current_track_info.is_playing   = doc["is_playing"].as<bool>();
                current_track_info.shuffle_state = doc["shuffle_state"].as<bool>();

                // Cache the device id so Play can wake an idle device later.
                if (doc["device"]["id"]) {
                    last_device_id = doc["device"]["id"].as<String>();
                }

                int vol = doc["device"]["volume_percent"] | current_volume_pct;
                current_track_info.volume_pct = vol;
                current_volume_pct = vol;

                const char* new_title  = doc["item"]["name"] | "Unknown Track";
                const char* new_artist = doc["item"]["artists"][0]["name"] | "Unknown Artist";
                const char* new_album  = doc["item"]["album"]["name"] | "Unknown Album";
                const char* new_url    = doc["item"]["album"]["images"][0]["url"] | "";

                current_track_info.progress_ms   = doc["progress_ms"].as<uint32_t>();
                current_track_info.duration_ms   = doc["item"]["duration_ms"].as<uint32_t>();
                current_track_info.local_album_idx = -1;

                bool song_changed = strncmp(current_track_info.title, new_title, 63) != 0;

                strncpy(current_track_info.title,  new_title,  63); current_track_info.title[63]  = '\0';
                strncpy(current_track_info.artist, new_artist, 63); current_track_info.artist[63] = '\0';
                strncpy(current_track_info.album,  new_album,  63); current_track_info.album[63]  = '\0';

                if (song_changed || strncmp(current_track_info.album_art_url, new_url, 127) != 0) {
                    strncpy(current_track_info.album_art_url, new_url, 127);
                    current_track_info.album_art_url[127] = '\0';
                    if (new_url[0] != '\0') {
                        ui_resume_sprite();
                        download_album_art(new_url);
                        ui_suspend_sprite();
                    }
                }
                track_info_updated = true;
            }
        } else if (httpCode == 204) {
            current_track_info.is_playing = false;
        } else {
            Serial.printf("Player state error: %d\n", httpCode);
        }
    }
    https.end();
    delete client;
    ui_resume_sprite();
}

// ── Playback controls ──────────────────────────────────────────────────────
// Shared helper: send a command with optional JSON body. Returns the HTTP
// status code (or 0 on connection failure). Callers check via _is_ok().
static int _spotify_command(const char* method, const char* path,
                            const char* body = nullptr)
{
    if (access_token == "" || WiFi.status() != WL_CONNECTED) return 0;

    ui_suspend_sprite();
    WiFiClientSecure *client = new WiFiClientSecure;
    client->setCACertBundle(rootca_crt_bundle_start);

    HTTPClient https;
    https.setTimeout(2000);
    String url = String("https://api.spotify.com") + path;
    int code = 0;

    if (https.begin(*client, url)) {
        https.addHeader("Authorization", "Bearer " + access_token);
        if (body) {
            https.addHeader("Content-Type", "application/json");
            https.addHeader("Content-Length", String(strlen(body)));
        } else {
            https.addHeader("Content-Length", "0");
        }

        if (strcmp(method, "POST") == 0) {
            code = https.POST(body ? String(body) : String(""));
        } else {
            // PUT
            code = https.PUT(body ? String(body) : String(""));
        }
        if (!(code == 200 || code == 204 || code == 202)) {
            Serial.printf("Spotify %s %s → %d\n", method, path, code);
        }
        https.end();
    }

    delete client;
    ui_resume_sprite();
    return code;
}

static inline bool _is_ok(int code) {
    return code == 200 || code == 204 || code == 202;
}

bool spotify_next_track() {
    return _is_ok(_spotify_command("POST", "/v1/me/player/next"));
}

bool spotify_prev_track() {
    return _is_ok(_spotify_command("POST", "/v1/me/player/previous"));
}

bool spotify_toggle_play_pause() {
    if (current_track_info.is_playing) {
        bool ok = _is_ok(_spotify_command("PUT", "/v1/me/player/pause"));
        if (ok) current_track_info.is_playing = false;
        return ok;
    } else {
        int code = _spotify_command("PUT", "/v1/me/player/play");
        // Spotify returns 404 "No active device found" when the previously
        // active device (e.g. the phone) has gone idle and dropped its
        // Connect session. Recover by transferring playback back to the
        // last-known device, which also starts playback in the same call.
        if (code == 404 && last_device_id.length() > 0) {
            String body = String("{\"device_ids\":[\"") + last_device_id +
                          "\"],\"play\":true}";
            code = _spotify_command("PUT", "/v1/me/player", body.c_str());
        }
        bool ok = _is_ok(code);
        if (ok) current_track_info.is_playing = true;
        return ok;
    }
}

bool spotify_toggle_shuffle() {
    bool new_state = !current_track_info.shuffle_state;
    String path = String("/v1/me/player/shuffle?state=") + (new_state ? "true" : "false");
    bool ok = _is_ok(_spotify_command("PUT", path.c_str()));
    if (ok) current_track_info.shuffle_state = new_state;
    return ok;
}

bool spotify_set_volume(int pct) {
    pct = constrain(pct, 0, 100);
    String path = String("/v1/me/player/volume?volume_percent=") + pct;
    bool ok = _is_ok(_spotify_command("PUT", path.c_str()));
    if (ok) {
        current_volume_pct = pct;
        current_track_info.volume_pct = pct;
    }
    return ok;
}

bool spotify_seek_position(int32_t pos_ms) {
    if (pos_ms < 0) pos_ms = 0;
    String path = String("/v1/me/player/seek?position_ms=") + pos_ms;
    bool ok = _is_ok(_spotify_command("PUT", path.c_str()));
    if (ok) current_track_info.progress_ms = (uint32_t)pos_ms;
    return ok;
}

bool spotify_play_album(const char* album_uri) {
    String body = String("{\"context_uri\": \"") + album_uri + "\"}";
    return _is_ok(_spotify_command("PUT", "/v1/me/player/play", body.c_str()));
}
