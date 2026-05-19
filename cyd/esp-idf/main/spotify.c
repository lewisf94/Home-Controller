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
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"

static const char *TAG = "spotify";

static const char *s_client_id      = NULL;
static const char *s_client_secret  = NULL;
static const char *s_refresh_token  = NULL;

static char     s_access_token[256]   = {0};
static int64_t  s_token_expiry_us     = 0;
static char     s_album_art_url[256]  = {0};

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} resp_buf_t;

#define RESP_INITIAL_CAP   1024
#define RESP_MAX_CAP     262144  /* 256 KB -- fits a 640x640 album JPEG */

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
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
 * or if the next non-space byte after the key isn't a colon. */
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
            ok = true;
        } else {
            ESP_LOGE(TAG, "token response missing fields: %.200s", buf.data);
        }
    } else {
        ESP_LOGE(TAG, "token request failed (err=%d status=%d body=%.200s)",
                 (int)err, status, buf.data ? buf.data : "");
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

bool spotify_fetch_now_playing(char *title_out, size_t title_len)
{
    if (!title_out || title_len == 0) return false;
    title_out[0] = '\0';

    if (!ensure_token()) return false;

    resp_buf_t buf = {0};

    esp_http_client_config_t cfg = {
        .url               = "https://api.spotify.com/v1/me/player",
        .method            = HTTP_METHOD_GET,
        .event_handler     = http_event_handler,
        .user_data         = &buf,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 5000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;

    char bearer[320];
    snprintf(bearer, sizeof(bearer), "Bearer %s", s_access_token);
    esp_http_client_set_header(client, "Authorization", bearer);

    bool ok = false;
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);

    if (err == ESP_OK && status == 200 && buf.data) {
        /* Scope the "name" lookup to the "item":{...} object so we get
         * the track title, not the album name or first artist name.
         * Use json_find_key so we tolerate Spotify's "item" : { ... }
         * pretty-printed spacing. */
        const char *item = json_find_key(buf.data, "item");
        if (item && json_get_string(item, "name", title_out, title_len)) {
            ok = true;

            /* Capture the first url inside item.album.images[]. Spotify
             * orders the array widest-first, so this is the 640x640
             * variant -- same one the Arduino build downloads. We rely
             * on the file-callback decode path (album_art_decode_file)
             * to handle it; the openRAM path in JPEGDEC 1.6.2 reliably
             * fails on Spotify's mozjpeg-encoded streams, but openFile
             * handles them fine. */
            const char *images = json_find_key(item, "images");
            s_album_art_url[0] = '\0';
            if (images) {
                json_get_string(images, "url",
                                s_album_art_url, sizeof(s_album_art_url));
            }
        } else {
            ESP_LOGW(TAG, "player response had no item.name");
        }
    } else if (err == ESP_OK && status == 204) {
        ESP_LOGI(TAG, "no active playback");
    } else if (err == ESP_OK && status == 401) {
        ESP_LOGW(TAG, "got 401, invalidating cached token");
        s_token_expiry_us = 0;
    } else {
        ESP_LOGE(TAG, "/me/player failed (err=%d status=%d)", (int)err, status);
    }

    esp_http_client_cleanup(client);
    free(buf.data);
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

    /* Album JPEGs are typically 30-120 KB. The growing-buffer in
     * http_event_handler caps at RESP_MAX_CAP (16 KB) which is too
     * small -- pre-allocate a larger buffer here so we don't trip
     * that ceiling. The handler will realloc up if needed, but its
     * own MAX still applies; we therefore set buf.cap up front. */
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

/* Per-request state for the download-to-file event handler. We track
 * the total bytes written so the caller can surface it, and the first
 * fwrite failure so a short-write error can abort the request rather
 * than silently producing a truncated file. */
typedef struct {
    FILE   *fp;
    size_t  written;
    bool    write_failed;
} file_sink_t;

static esp_err_t http_file_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    file_sink_t *sink = (file_sink_t *)evt->user_data;
    if (!sink || !sink->fp || sink->write_failed) return ESP_OK;

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
    file_sink_t sink = { .fp = fp, .written = 0, .write_failed = false };

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

    if (out_len) *out_len = sink.written;
    return true;
}
