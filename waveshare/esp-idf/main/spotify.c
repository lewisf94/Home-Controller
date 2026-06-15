/*
 * Spotify Web API client -- ESP-IDF implementation.
 *
 * HTTPS is provided by esp_http_client; TLS roots come from the IDF
 * certificate bundle (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y) so we don't
 * have to pin DigiCert manually.
 *
 * JSON: IDF v6.0 removed the bundled cJSON component, so we extract
 * the handful of fields we need with a small purpose-built scanner.
 * Spotify's responses are well-formed and the fields we use never
 * contain raw quotes (Spotify escapes them as \"), so a strstr-based
 * lookup with escape handling is sufficient and avoids pulling a
 * managed-component dependency for the sake of two values.
 *
 * Response bodies are accumulated in a heap buffer via the
 * ESP_HTTP_CLIENT_EVENT_ON_DATA callback so the JSON arrives as one
 * NUL-terminated string. esp_http_client_perform() drives the whole
 * exchange synchronously.
 */

#include "spotify.h"

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "nvs.h"
#include "mbedtls/base64.h"

static const char *TAG = "spotify";

static const char *s_client_id      = NULL;
static const char *s_client_secret  = NULL;
static const char *s_refresh_token  = NULL;

static char     s_access_token[256]   = {0};
static int64_t  s_token_expiry_us     = 0;
static char     s_album_art_url[256]  = {0};
static bool     s_is_playing          = false;
static bool     s_shuffle_state       = false;
/* Most-recent device id seen in /me/player. Spotify drops the active-device
 * association after a phone/speaker idles and returns 404 to the next play
 * command; we use this to wake it back up (PUT /me/player device_ids+play). */
static char     s_last_device_id[64]  = {0};

#define NVS_NAMESPACE   "spotify"
#define NVS_KEY_TOKEN   "access_token"

static void token_save_to_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, NVS_KEY_TOKEN, s_access_token);
    nvs_commit(h);
    nvs_close(h);
}

/* Pre-load whatever token we cached in flash. On success the token is
 * marked "untrusted" by leaving s_token_expiry_us at 0 -- the next API
 * call will use it speculatively; if it has expired Spotify will reply
 * 401 and the regular refresh path picks up. Worst case we waste one
 * round-trip; typical case we skip an entire token refresh on boot. */
static void token_load_from_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(s_access_token);
    if (nvs_get_str(h, NVS_KEY_TOKEN, s_access_token, &len) == ESP_OK) {
        /* Push expiry into the future so ensure_token() doesn't refresh
         * immediately. A 401 in spotify_fetch_player() zeros it again. */
        s_token_expiry_us = INT64_MAX;
        ESP_LOGI(TAG, "loaded cached access token from NVS (%u bytes)",
                 (unsigned)len);
    } else {
        s_access_token[0] = '\0';
    }
    nvs_close(h);
}

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} resp_buf_t;

/* A /me/player response is typically 6-15 KB, so a 16 KB first allocation
 * makes the every-5-s poll a single malloc+free (landing in PSRAM via the
 * >4 KB malloc policy) instead of a 1->2->4->8->16 KB realloc ladder each
 * poll. Small responses (token refresh ~0.5 KB) briefly over-allocate the
 * same 16 KB -- noise next to 31 MB of PSRAM. */
#define RESP_INITIAL_CAP  16384
#define RESP_MAX_CAP     262144  /* 256 KB -- fits a 640x640 album JPEG */

/* 429 rate-limit holdoff for the /me/player poll. s_retry_after_s is written
 * by the shared event handler below on ANY request that carries a Retry-After
 * header; the poll neutralises that sharing by zeroing it immediately before
 * its own perform and reading it only when its own status is 429. All
 * spotify.c HTTP runs on spotify_task, so there is no concurrent writer. */
static int64_t s_poll_holdoff_until_us = 0;
static int     s_retry_after_s         = 0;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_HEADER) {
        /* Retry-After is normally delta-seconds; an HTTP-date parses to 0 via
         * atoi and is treated as absent by the 429 branch in the poll. */
        if (evt->header_key && evt->header_value &&
            strcasecmp(evt->header_key, "Retry-After") == 0) {
            s_retry_after_s = atoi(evt->header_value);
        }
        return ESP_OK;
    }
    if (evt->event_id != HTTP_EVENT_ON_DATA) {
        return ESP_OK;
    }
    resp_buf_t *buf = (resp_buf_t *)evt->user_data;
    if (!buf) {
        return ESP_OK;
    }
    size_t need = buf->len + evt->data_len + 1;
    if (need > buf->cap) {
        size_t new_cap = buf->cap ? buf->cap : RESP_INITIAL_CAP;
        while (new_cap < need) new_cap *= 2;
        if (new_cap > RESP_MAX_CAP) {
            ESP_LOGW(TAG, "response truncated at %u bytes", (unsigned)RESP_MAX_CAP);
            return ESP_OK;
        }
        char *grown = realloc(buf->data, new_cap);
        if (!grown) return ESP_FAIL;
        buf->data = grown;
        buf->cap  = new_cap;
    }
    memcpy(buf->data + buf->len, evt->data, evt->data_len);
    buf->len += evt->data_len;
    buf->data[buf->len] = '\0';
    return ESP_OK;
}

/* Find "<key>" in `json` and return a pointer to the value byte that
 * follows. Tolerates whitespace on either side of the colon, which
 * Spotify's API uses (e.g. `"name" : "iPhone"`). NULL if not present
 * or if the next non-space byte after the key isn't a colon.
 *
 * Note: this is a flat strstr-based search and does NOT respect object
 * nesting. Fine for keys that are globally unique in the response (e.g.
 * "access_token" in the token response, "expires_in", "progress_ms",
 * "duration_ms"), but unsafe for keys like "name" that appear at many
 * different depths. For those, use json_obj_get(). */
static const char *json_find_key(const char *json, const char *key)
{
    char needle[64];
    int n = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (n <= 0 || n >= (int)sizeof(needle)) return NULL;
    const char *p = strstr(json, needle);
    if (!p) return NULL;
    p += n;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != ':') return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* Advance past a JSON string starting at `p` (must point at the opening
 * quote). Returns the byte after the closing quote. Handles backslash
 * escapes so an escaped quote does not end the string early. */
static const char *json_skip_string(const char *p)
{
    if (*p != '"') return p;
    p++;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) p += 2;
        else p++;
    }
    if (*p == '"') p++;
    return p;
}

/* Advance past a JSON value starting at `p`. Handles objects/arrays
 * (with nesting), strings (with escapes), and primitives (numbers,
 * true, false, null). Returns the byte after the value. */
static const char *json_skip_value(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p == '"') {
        return json_skip_string(p);
    }
    if (*p == '{' || *p == '[') {
        char open = *p;
        char close = (open == '{') ? '}' : ']';
        int depth = 0;
        while (*p) {
            if (*p == '"') {
                p = json_skip_string(p);
                continue;
            }
            if (*p == open) depth++;
            else if (*p == close) {
                depth--;
                if (depth == 0) { p++; return p; }
            }
            p++;
        }
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']' &&
           *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
    return p;
}

/* Find `key` at the TOP LEVEL of the JSON object beginning at `obj`
 * (which must start with '{'). Skips nested objects/arrays so we
 * don't accidentally match the same key name at a deeper level.
 * Returns pointer to the value (whitespace already consumed) or NULL. */
static const char *json_obj_get(const char *obj, const char *key)
{
    if (!obj || *obj != '{') return NULL;
    size_t key_len = strlen(key);

    const char *p = obj + 1;
    while (*p && *p != '}') {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',') p++;
        if (*p != '"') break;

        const char *key_start = p + 1;
        const char *key_end = json_skip_string(p);  /* points after closing quote */
        bool match = (key_end > key_start + 1) &&
                     ((size_t)(key_end - key_start - 1) == key_len) &&
                     (memcmp(key_start, key, key_len) == 0);
        p = key_end;

        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (*p != ':') break;
        p++;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;

        if (match) return p;
        p = json_skip_value(p);
    }
    return NULL;
}

/* Returns pointer to the first '{' inside a JSON array (which must
 * start with '['). NULL if the array is empty or malformed. Used to
 * descend into `artists[0]` and `images[0]`. */
static const char *json_arr_first_obj(const char *arr)
{
    if (!arr || *arr != '[') return NULL;
    arr++;
    while (*arr == ' ' || *arr == '\t' || *arr == '\n' || *arr == '\r') arr++;
    return (*arr == '{') ? arr : NULL;
}

/* Copy the JSON string value starting at `p` (which must point at the
 * opening quote) into `out`, decoding the common backslash escapes.
 * Returns true on success. */
static bool json_copy_string(const char *p, char *out, size_t out_len)
{
    if (!p || *p != '"' || out_len == 0) return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_len) {
        if (*p == '\\' && p[1]) {
            switch (p[1]) {
                case 'n':  out[i++] = '\n'; break;
                case 't':  out[i++] = '\t'; break;
                case 'r':  out[i++] = '\r'; break;
                case '"':  out[i++] = '"';  break;
                case '\\': out[i++] = '\\'; break;
                case '/':  out[i++] = '/';  break;
                default:   out[i++] = p[1]; break;
            }
            p += 2;
        } else {
            out[i++] = *p++;
        }
    }
    out[i] = '\0';
    return (*p == '"');
}

static bool json_get_string(const char *json, const char *key,
                            char *out, size_t out_len)
{
    const char *p = json_find_key(json, key);
    return json_copy_string(p, out, out_len);
}

static bool json_get_int(const char *json, const char *key, int *out)
{
    const char *p = json_find_key(json, key);
    if (!p) return false;
    *out = atoi(p);
    return true;
}

void spotify_init(const char *client_id,
                  const char *client_secret,
                  const char *refresh_token)
{
    s_client_id     = client_id;
    s_client_secret = client_secret;
    s_refresh_token = refresh_token;
    s_access_token[0] = '\0';
    s_token_expiry_us = 0;
    /* Prime s_access_token with whatever we cached in flash last time so
     * the first /v1/me/player request can use it directly. If it's stale
     * the regular 401 path triggers a refresh. */
    token_load_from_nvs();
}

static bool basic_auth_header(char *out, size_t out_len)
{
    char joined[256];
    int n = snprintf(joined, sizeof(joined), "%s:%s", s_client_id, s_client_secret);
    if (n <= 0 || n >= (int)sizeof(joined)) return false;

    unsigned char b64[384];
    size_t olen = 0;
    if (mbedtls_base64_encode(b64, sizeof(b64), &olen,
                              (const unsigned char *)joined, (size_t)n) != 0) {
        return false;
    }
    int m = snprintf(out, out_len, "Basic %.*s", (int)olen, b64);
    return (m > 0 && m < (int)out_len);
}

bool spotify_refresh_access_token(void)
{
    if (!s_refresh_token) {
        ESP_LOGE(TAG, "spotify_init() not called");
        return false;
    }

    resp_buf_t buf = {0};

    esp_http_client_config_t cfg = {
        .url               = "https://accounts.spotify.com/api/token",
        .method            = HTTP_METHOD_POST,
        .event_handler     = http_event_handler,
        .user_data         = &buf,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 5000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "http client init failed");
        return false;
    }

    char auth[512];
    if (!basic_auth_header(auth, sizeof(auth))) {
        ESP_LOGE(TAG, "basic auth encode failed");
        esp_http_client_cleanup(client);
        return false;
    }
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");

    char body[512];
    int  body_len = snprintf(body, sizeof(body),
                             "grant_type=refresh_token&refresh_token=%s",
                             s_refresh_token);
    /* snprintf returns the length it WOULD have written; if that exceeds the
     * buffer the body was truncated and body_len points past `body`, so
     * passing it to set_post_field would read out of bounds and leak adjacent
     * memory over the wire. Bail out instead. */
    if (body_len < 0 || body_len >= (int)sizeof(body)) {
        ESP_LOGE(TAG, "refresh token too long for request body");
        esp_http_client_cleanup(client);
        return false;
    }
    esp_http_client_set_post_field(client, body, body_len);

    bool ok = false;
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    if (err == ESP_OK && status == 200 && buf.data) {
        char tok[256];
        int  exp = 0;
        if (json_get_string(buf.data, "access_token", tok, sizeof(tok)) &&
            json_get_int(buf.data, "expires_in", &exp)) {
            strncpy(s_access_token, tok, sizeof(s_access_token) - 1);
            s_access_token[sizeof(s_access_token) - 1] = '\0';
            int64_t lifetime_us = (int64_t)exp * 1000000;
            s_token_expiry_us = esp_timer_get_time() + lifetime_us - 60 * 1000000;
            ESP_LOGI(TAG, "access token refreshed (expires in %d s)", exp);
            token_save_to_nvs();
            ok = true;
        } else {
            /* Do NOT log buf.data here: the token-endpoint body can contain the
             * access_token. Log only non-secret facts. */
            ESP_LOGE(TAG, "token response missing fields (len=%d)",
                     buf.data ? (int)strlen(buf.data) : 0);
        }
    } else {
        ESP_LOGE(TAG, "token request failed (err=%d status=%d)", (int)err, status);
    }

    esp_http_client_cleanup(client);
    free(buf.data);
    return ok;
}

static bool ensure_token(void)
{
    if (s_access_token[0] == '\0' || esp_timer_get_time() >= s_token_expiry_us) {
        return spotify_refresh_access_token();
    }
    return true;
}

/* Persistent keep-alive client for the /v1/me/player poll. The player is
 * polled every few seconds, so giving that hot path a reused connection means
 * the TLS session is negotiated once instead of re-handshaking (and
 * re-validating the cert bundle) on every poll. Single-threaded: only
 * spotify_task touches either client. */
static esp_http_client_handle_t s_poll_client = NULL;

static void poll_client_close(void)
{
    if (s_poll_client) {
        esp_http_client_cleanup(s_poll_client);
        s_poll_client = NULL;
    }
}

/* Persistent keep-alive client for playback commands. Same principle as
 * s_poll_client: reusing one TLS session across consecutive button presses
 * skips the handshake (~0.5-2 s) that would otherwise block the UI. Commands
 * always go to api.spotify.com so the connection stays eligible for reuse.
 * Cleared on any transport error or 401 so the next call opens a fresh handle. */
static esp_http_client_handle_t s_cmd_client = NULL;
static int _do_cmd(esp_http_client_method_t method, const char *url, const char *body);

static void cmd_client_close(void)
{
    if (s_cmd_client) {
        esp_http_client_cleanup(s_cmd_client);
        s_cmd_client = NULL;
    }
}

static esp_http_client_handle_t poll_client_get(resp_buf_t *buf)
{
    if (s_poll_client) {
        /* Point the shared event handler at this call's response buffer. */
        esp_http_client_set_user_data(s_poll_client, buf);
        return s_poll_client;
    }
    esp_http_client_config_t cfg = {
        .url               = "https://api.spotify.com/v1/me/player",
        .method            = HTTP_METHOD_GET,
        .event_handler     = http_event_handler,
        .user_data         = buf,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 5000,
        .keep_alive_enable = true,
    };
    s_poll_client = esp_http_client_init(&cfg);
    return s_poll_client;
}

bool spotify_fetch_player(spotify_track_t *info)
{
    if (!info) return false;
    memset(info, 0, sizeof(*info));

    /* Rate-limited: skip the poll (and the token refresh it would trigger)
     * until the server-requested wait has elapsed. The caller treats this
     * like "no active playback", keeping the last track on screen. */
    if (esp_timer_get_time() < s_poll_holdoff_until_us) return false;

    if (!ensure_token()) return false;

    resp_buf_t buf = {0};

    esp_http_client_handle_t client = poll_client_get(&buf);
    if (!client) return false;

    char bearer[320];
    snprintf(bearer, sizeof(bearer), "Bearer %s", s_access_token);
    esp_http_client_set_header(client, "Authorization", bearer);

    bool ok = false;
    /* Clear any Retry-After captured by an earlier request through the shared
     * event handler, so the 429 branch below only sees THIS response's value. */
    s_retry_after_s = 0;
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);

    if (err == ESP_OK && status == 200 && buf.data) {
        /* Top-level scalars: is_playing and progress_ms appear only at
         * the root of the response so flat strstr is safe. */
        const char *is_playing_v = json_obj_get(buf.data, "is_playing");
        if (is_playing_v) info->is_playing = (*is_playing_v == 't');

        const char *shuffle_v = json_obj_get(buf.data, "shuffle_state");
        if (shuffle_v) {
            info->shuffle_state = (*shuffle_v == 't');
            s_shuffle_state = info->shuffle_state;
        }

        const char *progress_v = json_obj_get(buf.data, "progress_ms");
        if (progress_v) info->progress_ms = (uint32_t)atoi(progress_v);

        /* Active device: is_restricted gates whether the Spotify Web API can
         * control it (Sonos etc. report true -- readable but not controllable). */
        info->volume_pct = -1;
        const char *dev = json_obj_get(buf.data, "device");
        if (dev && *dev == '{') {
            const char *restr_v = json_obj_get(dev, "is_restricted");
            if (restr_v) info->device_restricted = (*restr_v == 't');
            const char *dname_v = json_obj_get(dev, "name");
            if (dname_v) json_copy_string(dname_v, info->device_name, sizeof(info->device_name));
            const char *vol_v = json_obj_get(dev, "volume_percent");
            if (vol_v) info->volume_pct = atoi(vol_v);
            /* Cache the device id so toggle_play_pause can wake an idle
             * device (Spotify returns 404 after the phone times out). */
            const char *id_v = json_obj_get(dev, "id");
            if (id_v) json_copy_string(id_v, s_last_device_id, sizeof(s_last_device_id));
        }

        /* Drill into item.{name, duration_ms, album.name, artists[0].name,
         * album.images[0].url} using depth-aware lookups so we don't
         * accidentally match a "name" or "duration_ms" inside a nested
         * device/context block. */
        const char *item = json_obj_get(buf.data, "item");
        if (item && *item == '{') {
            const char *name_v = json_obj_get(item, "name");
            if (name_v) json_copy_string(name_v, info->title, sizeof(info->title));

            const char *duration_v = json_obj_get(item, "duration_ms");
            if (duration_v) info->duration_ms = (uint32_t)atoi(duration_v);

            const char *album_obj = json_obj_get(item, "album");
            if (album_obj && *album_obj == '{') {
                const char *album_name_v = json_obj_get(album_obj, "name");
                if (album_name_v) json_copy_string(album_name_v, info->album, sizeof(info->album));

                /* album.uri ("spotify:album:...") -- used by the UI to auto-snap
                 * the browser carousel to the currently playing album. */
                const char *album_uri_v = json_obj_get(album_obj, "uri");
                if (album_uri_v) json_copy_string(album_uri_v, info->album_uri, sizeof(info->album_uri));

                /* Spotify orders images widest-first, so [0] is the 640x640
                 * variant. The openFile decode path in album_art.cpp handles
                 * it. */
                const char *images_arr = json_obj_get(album_obj, "images");
                const char *first_img = json_arr_first_obj(images_arr);
                if (first_img) {
                    const char *url_v = json_obj_get(first_img, "url");
                    if (url_v) json_copy_string(url_v, info->album_art_url,
                                                sizeof(info->album_art_url));
                }
            }

            const char *artists_arr = json_obj_get(item, "artists");
            const char *first_artist = json_arr_first_obj(artists_arr);
            if (first_artist) {
                const char *artist_name_v = json_obj_get(first_artist, "name");
                if (artist_name_v) json_copy_string(artist_name_v, info->artist,
                                                    sizeof(info->artist));
            }

            /* Cache for spotify_get_album_art_url() callers. */
            strncpy(s_album_art_url, info->album_art_url, sizeof(s_album_art_url) - 1);
            s_album_art_url[sizeof(s_album_art_url) - 1] = '\0';

            ok = (info->title[0] != '\0');
            if (ok) s_is_playing = info->is_playing;
        } else {
            ESP_LOGW(TAG, "player response had no item object");
        }
    } else if (err == ESP_OK && status == 204) {
        /* Fires every poll while idle (15 s cadence) -- keep it at debug;
         * main.c logs the playing->idle transition once at INFO. */
        ESP_LOGD(TAG, "no active playback");
    } else if (status == 401) {
        /* Expired/invalid token. NOTE: esp_http_client_perform() returns a
         * non-ESP_OK err on 401 because it can't satisfy the Bearer auth
         * challenge, so we must key off `status` alone here -- gating on
         * err==ESP_OK would never fire and we'd loop forever on the stale
         * token. Clear it so ensure_token() refreshes on the next poll. */
        ESP_LOGW(TAG, "got 401, invalidating cached token; will refresh next poll");
        s_token_expiry_us = 0;
        s_access_token[0]  = '\0';
    } else if (status == 429) {
        /* Rate limited. Honour Retry-After when present (Spotify's app-level
         * penalties can run minutes to hours); default to 30 s when absent
         * (or an HTTP-date), and cap the wait so a bogus header can't park
         * the poll for good -- if we're still limited, the next 429 re-arms. */
        int wait_s = s_retry_after_s;
        if (wait_s <= 0)  wait_s = 30;
        if (wait_s > 900) wait_s = 900;
        s_poll_holdoff_until_us = esp_timer_get_time() + (int64_t)wait_s * 1000000;
        ESP_LOGW(TAG, "429 rate limited; pausing /me/player polls for %d s", wait_s);
    } else {
        ESP_LOGE(TAG, "/me/player failed (err=%d status=%d)", (int)err, status);
    }

    /* Keep the connection alive for the next poll on HTTP-layer errors (401,
     * 204, etc.). On transport failure (err != ESP_OK) the TLS session is
     * broken -- drop the client so the next poll opens a fresh connection. */
    if (err != ESP_OK) poll_client_close();
    free(buf.data);
    return ok;
}

bool spotify_fetch_now_playing(char *title_out, size_t title_len)
{
    if (!title_out || title_len == 0) return false;
    spotify_track_t info;
    bool ok = spotify_fetch_player(&info);
    if (ok) {
        strncpy(title_out, info.title, title_len - 1);
        title_out[title_len - 1] = '\0';
    } else {
        title_out[0] = '\0';
    }
    return ok;
}

const char *spotify_get_album_art_url(void)
{
    return s_album_art_url;
}

unsigned char *spotify_download_bytes(const char *url, size_t *out_len)
{
    if (!url || url[0] == '\0' || !out_len) return NULL;
    *out_len = 0;

    /* Album JPEGs are typically 30-120 KB and fit under RESP_MAX_CAP
     * (262144 = 256 KB). Pre-allocate 8 KB so the growing buffer in
     * http_event_handler doesn't realloc from scratch over the first chunks.
     * NOTE: this RAM-decode path (with album_art_decode) is the intended art
     * path on this PSRAM board; the build currently decodes via a LittleFS
     * file instead, so spotify_download_bytes is presently unused. */
    resp_buf_t buf = {0};
    buf.cap  = 8 * 1024;
    buf.data = malloc(buf.cap);
    if (!buf.data) return NULL;

    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_GET,
        .event_handler     = http_event_handler,
        .user_data         = &buf,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(buf.data);
        return NULL;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || buf.len == 0) {
        ESP_LOGW(TAG, "download failed (err=%d status=%d len=%u)",
                 (int)err, status, (unsigned)buf.len);
        free(buf.data);
        return NULL;
    }

    *out_len = buf.len;
    return (unsigned char *)buf.data;
}

/* Per-request state for the download-to-file event handler. We track the total
 * bytes written so the caller can surface it, the first fwrite failure so a
 * short-write error can abort the request rather than silently producing a
 * truncated file, and the first two bytes seen so the caller can reject a body
 * that isn't a JPEG (CDN error page, truncated stream) before feeding it to
 * the decoder. */
typedef struct {
    FILE   *fp;
    size_t  written;
    bool    write_failed;
    uint8_t magic[2];     /* first two bytes of the body, for JPEG SOI check */
} file_sink_t;

static esp_err_t http_file_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    file_sink_t *sink = (file_sink_t *)evt->user_data;
    if (!sink || !sink->fp || sink->write_failed) return ESP_OK;

    /* Capture the first two bytes of the body (regardless of chunk boundary)
     * so the caller can reject non-JPEG responses without re-reading the file. */
    size_t already = sink->written;
    const uint8_t *src = (const uint8_t *)evt->data;
    for (size_t i = 0; i < (size_t)evt->data_len && already + i < 2; i++) {
        sink->magic[already + i] = src[i];
    }

    size_t want = (size_t)evt->data_len;
    size_t got  = fwrite(evt->data, 1, want, sink->fp);
    sink->written += got;
    if (got != want) {
        sink->write_failed = true;
        ESP_LOGE(TAG, "fwrite short: wrote %u of %u", (unsigned)got, (unsigned)want);
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool spotify_download_to_file(const char *url, const char *path, size_t *out_len)
{
    if (out_len) *out_len = 0;
    if (!url || url[0] == '\0' || !path) return false;

    /* fopen with "wb" truncates any previous file at this path. */
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        ESP_LOGE(TAG, "fopen(%s, wb) failed", path);
        return false;
    }
    file_sink_t sink = { .fp = fp, .written = 0, .write_failed = false, .magic = {0,0} };

    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_GET,
        .event_handler     = http_file_event_handler,
        .user_data         = &sink,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        fclose(fp);
        return false;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    fclose(fp);

    if (err != ESP_OK || status != 200 || sink.write_failed || sink.written == 0) {
        ESP_LOGW(TAG, "download-to-file failed (err=%d status=%d wrote=%u write_failed=%d)",
                 (int)err, status, (unsigned)sink.written, (int)sink.write_failed);
        return false;
    }

    /* Reject anything that doesn't start with the JPEG Start-Of-Image marker
     * (FF D8). A CDN error page, truncated stream, or wrong-Content-Type body
     * would otherwise be saved + fed to the decoder + cached as "loaded",
     * leaving last-track art on screen forever. Discard the file so the next
     * poll can retry. */
    if (sink.written < 2 || sink.magic[0] != 0xFF || sink.magic[1] != 0xD8) {
        ESP_LOGW(TAG, "downloaded bytes are not JPEG (magic %02X %02X, %u bytes) -- discarding",
                 sink.magic[0], sink.magic[1], (unsigned)sink.written);
        remove(path);
        return false;
    }

    if (out_len) *out_len = sink.written;
    return true;
}

bool spotify_play_album(const char *context_uri)
{
    if (!context_uri || context_uri[0] == '\0') return false;

    char body[160];
    int body_len = snprintf(body, sizeof(body),
                            "{\"context_uri\":\"%s\"}", context_uri);
    if (body_len <= 0 || body_len >= (int)sizeof(body)) return false;

    /* Route through _do_cmd so the request reuses s_cmd_client's keep-alive
     * TLS session (saves the ~0.5-2 s handshake vs. opening a fresh handle).
     * 401/4xx handling and token invalidation are already inside _do_cmd. */
    int status = _do_cmd(HTTP_METHOD_PUT,
                         "https://api.spotify.com/v1/me/player/play", body);
    /* Spotify returns 204 No Content on success. 202 Accepted is also
     * reported on some devices. */
    bool ok = (status == 204 || status == 202);
    if (!ok)
        ESP_LOGW(TAG, "play_album(%s) failed status=%d", context_uri, status);
    return ok;
}

/* ── Playback controls ───────────────────────────────────────────────── */

/* HTTP status of the most recent _do_cmd() call (single-threaded: spotify_task
 * only). Lets the dispatcher distinguish a 403 "restricted device" from other
 * failures so it can surface the transfer hint. -1 on any pre-request failure. */
static int s_last_cmd_status = 0;

int spotify_last_cmd_status(void) { return s_last_cmd_status; }

static int _do_cmd(esp_http_client_method_t method, const char *url, const char *body)
{
    s_last_cmd_status = -1;
    if (!ensure_token()) return -1;

    if (!s_cmd_client) {
        esp_http_client_config_t cfg = {
            .url               = url,
            .method            = method,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms        = 5000,
            .keep_alive_enable = true,
        };
        s_cmd_client = esp_http_client_init(&cfg);
        if (!s_cmd_client) return -1;
    } else {
        esp_http_client_set_url(s_cmd_client, url);
        esp_http_client_set_method(s_cmd_client, method);
    }

    char bearer[320];
    snprintf(bearer, sizeof(bearer), "Bearer %s", s_access_token);
    esp_http_client_set_header(s_cmd_client, "Authorization", bearer);

    if (body && body[0]) {
        esp_http_client_set_header(s_cmd_client, "Content-Type", "application/json");
        esp_http_client_set_post_field(s_cmd_client, body, (int)strlen(body));
    } else {
        esp_http_client_delete_header(s_cmd_client, "Content-Type");
        esp_http_client_set_post_field(s_cmd_client, NULL, 0);
    }

    esp_err_t err = esp_http_client_perform(s_cmd_client);
    int status = esp_http_client_get_status_code(s_cmd_client);

    /* On transport failure the TLS session is broken -- drop the handle so
     * the next command re-establishes instead of reusing a dead socket. */
    if (err != ESP_OK) cmd_client_close();

    /* Mirror the poll's 401 handling: a server-side token invalidation would
     * otherwise make a button press fail silently until the next poll happens
     * to refresh. Clear the token so the next command/poll refreshes at once. */
    if (status == 401) {
        ESP_LOGW(TAG, "cmd %s got 401, invalidating cached token", url);
        s_token_expiry_us = 0;
        s_access_token[0]  = '\0';
        cmd_client_close();
    } else if (status >= 400) {
        ESP_LOGW(TAG, "cmd %s -> %d", url, status);
    }
    s_last_cmd_status = status;
    return status;
}

static inline bool _cmd_ok(int status)
{
    return status == 200 || status == 204 || status == 202;
}

bool spotify_toggle_play_pause(void)
{
    const char *url = s_is_playing
        ? "https://api.spotify.com/v1/me/player/pause"
        : "https://api.spotify.com/v1/me/player/play";
    int st = _do_cmd(HTTP_METHOD_PUT, url, NULL);
    /* Spotify drops the active-device association after the phone/speaker
     * idles and returns 404 "No active device found" on the next play. Wake
     * the last known device by transferring playback to it (also starts it). */
    if (st == 404 && !s_is_playing && s_last_device_id[0]) {
        char body[128];
        int n = snprintf(body, sizeof body,
                         "{\"device_ids\":[\"%s\"],\"play\":true}", s_last_device_id);
        if (n > 0 && n < (int)sizeof body) {
            ESP_LOGI(TAG, "play returned 404 -- waking last device");
            st = _do_cmd(HTTP_METHOD_PUT,
                         "https://api.spotify.com/v1/me/player", body);
        }
    }
    bool ok = _cmd_ok(st);
    if (ok) s_is_playing = !s_is_playing;
    return ok;
}

bool spotify_prev_track(void)
{
    return _cmd_ok(_do_cmd(HTTP_METHOD_POST,
                           "https://api.spotify.com/v1/me/player/previous", NULL));
}

bool spotify_next_track(void)
{
    return _cmd_ok(_do_cmd(HTTP_METHOD_POST,
                           "https://api.spotify.com/v1/me/player/next", NULL));
}

bool spotify_seek_position(uint32_t position_ms)
{
    char url[96];
    snprintf(url, sizeof(url),
             "https://api.spotify.com/v1/me/player/seek?position_ms=%lu",
             (unsigned long)position_ms);
    return _cmd_ok(_do_cmd(HTTP_METHOD_PUT, url, NULL));
}

bool spotify_set_volume(int pct)
{
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    char url[80];
    snprintf(url, sizeof(url),
             "https://api.spotify.com/v1/me/player/volume?volume_percent=%d", pct);
    return _cmd_ok(_do_cmd(HTTP_METHOD_PUT, url, NULL));
}

bool spotify_toggle_shuffle(void)
{
    bool new_state = !s_shuffle_state;
    char url[96];
    snprintf(url, sizeof(url),
             "https://api.spotify.com/v1/me/player/shuffle?state=%s",
             new_state ? "true" : "false");
    bool ok = _cmd_ok(_do_cmd(HTTP_METHOD_PUT, url, NULL));
    if (ok) s_shuffle_state = new_state;
    return ok;
}

bool spotify_transfer_playback(const char *device_id)
{
    if (!device_id || !device_id[0]) return false;
    char body[128];
    int n = snprintf(body, sizeof body,
                     "{\"device_ids\":[\"%s\"],\"play\":true}", device_id);
    if (n <= 0 || n >= (int)sizeof body) return false;
    return _cmd_ok(_do_cmd(HTTP_METHOD_PUT,
                           "https://api.spotify.com/v1/me/player", body));
}

bool spotify_get_devices(spotify_device_t *out, int max, int *count)
{
    if (count) *count = 0;
    if (!out || max <= 0) return false;
    if (!ensure_token()) return false;

    resp_buf_t resp = {0};
    esp_http_client_config_t cfg = {
        .url               = "https://api.spotify.com/v1/me/player/devices",
        .method            = HTTP_METHOD_GET,
        .event_handler     = http_event_handler,
        .user_data         = &resp,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 5000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;

    char bearer[320];
    snprintf(bearer, sizeof(bearer), "Bearer %s", s_access_token);
    esp_http_client_set_header(client, "Authorization", bearer);

    esp_err_t err = esp_http_client_perform(client);
    int status   = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    int n = 0;
    if (err == ESP_OK && status == 200 && resp.data) {
        const char *obj = json_arr_first_obj(json_obj_get(resp.data, "devices"));
        while (obj && n < max) {
            spotify_device_t *d = &out[n];
            memset(d, 0, sizeof *d);
            const char *v;
            if ((v = json_obj_get(obj, "id")))   json_copy_string(v, d->id,   sizeof d->id);
            if ((v = json_obj_get(obj, "name"))) json_copy_string(v, d->name, sizeof d->name);
            if ((v = json_obj_get(obj, "type"))) json_copy_string(v, d->type, sizeof d->type);
            v = json_obj_get(obj, "is_active");
            d->is_active = (v && *v == 't');
            if (d->id[0] && d->name[0]) n++;        /* skip id-less / nameless rows */

            const char *after = json_skip_value(obj);
            while (*after==' '||*after=='\t'||*after=='\n'||*after=='\r'||*after==',') after++;
            obj = (*after == '{') ? after : NULL;
        }
    } else if (status == 401) {
        /* Same fix as the other call paths -- clear the token so the next
         * call refreshes instead of looping on the stale Bearer. */
        ESP_LOGW(TAG, "get_devices got 401, invalidating cached token");
        s_token_expiry_us = 0;
        s_access_token[0]  = '\0';
    } else {
        ESP_LOGW(TAG, "get_devices err=%d status=%d", (int)err, status);
    }
    free(resp.data);
    if (count) *count = n;
    return (err == ESP_OK && status == 200);
}
