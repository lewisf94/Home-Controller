/*
 * Home Assistant WebSocket client -- see ha_client.h.
 *
 * Reuses the small purpose-built JSON scanner from the Spotify backend (HA's
 * WS frames are well-formed and compact). esp_websocket_client delivers frames
 * to s_ws_event_handler, which runs the auth handshake, then maps the target
 * media_player entity's state into the shared spotify_track_t and pushes it to
 * the UI. Outbound commands and album-art download are called from main.c's ha
 * task.
 */

#include "ha_client.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_http_client.h"

#include "player.h"    /* spotify_track_t -- backend-neutral contract from p4_shared */
#include "ui.h"        /* ui_set_track_info */

static const char *TAG = "ha";

/* ── Config ──────────────────────────────────────────────────────────────── */
static const char *s_host   = NULL;
static int         s_port   = 8123;
static const char *s_token  = NULL;
static const char *s_entity = NULL;

static esp_websocket_client_handle_t s_ws = NULL;
static int s_msg_id = 1;                 /* incrementing WS command id */
static int s_states_req_id = 0;          /* id of our get_states request */

static spotify_track_t s_track = {0};

/* Pending album-art relative URL, set by the WS task, consumed by the ha task. */
static portMUX_TYPE s_art_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_pending_art[256] = {0};
static bool s_art_pending = false;
static char s_art_loaded[256] = {0};     /* last URL we already fetched */

/* Inbound frame reassembly (WS frames can arrive in chunks). */
static char  *s_rx     = NULL;
static size_t s_rx_cap = 0;
#define RX_MAX_CAP (64 * 1024)

/* ── JSON scanner (copied from spotify.c) ────────────────────────────────── */
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

static const char *json_skip_value(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p == '"') return json_skip_string(p);
    if (*p == '{' || *p == '[') {
        char open = *p, close = (open == '{') ? '}' : ']';
        int depth = 0;
        while (*p) {
            if (*p == '"') { p = json_skip_string(p); continue; }
            if (*p == open) depth++;
            else if (*p == close) { depth--; if (depth == 0) return p + 1; }
            p++;
        }
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']' &&
           *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
    return p;
}

/* Find `key` at the top level of the object beginning at `obj` ('{'). */
static const char *json_obj_get(const char *obj, const char *key)
{
    if (!obj || *obj != '{') return NULL;
    size_t key_len = strlen(key);
    const char *p = obj + 1;
    while (*p && *p != '}') {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',') p++;
        if (*p != '"') break;
        const char *key_start = p + 1;
        const char *key_end = json_skip_string(p);
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

static bool json_copy_string(const char *p, char *out, size_t out_len)
{
    if (!p || *p != '"' || out_len == 0) return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_len) {
        if (*p == '\\' && p[1]) {
            switch (p[1]) {
                case 'n': out[i++] = '\n'; break;
                case 't': out[i++] = '\t'; break;
                case 'r': out[i++] = '\r'; break;
                case '"': out[i++] = '"';  break;
                case '\\':out[i++] = '\\'; break;
                case '/': out[i++] = '/';  break;
                default:  out[i++] = p[1]; break;
            }
            p += 2;
        } else {
            out[i++] = *p++;
        }
    }
    out[i] = '\0';
    return (*p == '"');
}

static bool json_obj_get_str(const char *obj, const char *key,
                             char *out, size_t out_len)
{
    return json_copy_string(json_obj_get(obj, key), out, out_len);
}

static bool json_obj_get_double(const char *obj, const char *key, double *out)
{
    const char *p = json_obj_get(obj, key);
    if (!p || *p == '"' || *p == '{' || *p == '[') return false;  /* expect number */
    if (strncmp(p, "null", 4) == 0) return false;
    *out = atof(p);
    return true;
}

/* ── Outbound (WebSocket sends) ──────────────────────────────────────────── */
static bool ws_send(const char *json)
{
    if (!s_ws || !esp_websocket_client_is_connected(s_ws)) {
        ESP_LOGW(TAG, "ws_send: not connected, dropping: %.40s...", json);
        return false;
    }
    int ret = esp_websocket_client_send_text(s_ws, json, strlen(json), pdMS_TO_TICKS(2000));
    return (ret >= 0);
}

static void send_auth(void)
{
    char buf[320];
    snprintf(buf, sizeof(buf), "{\"type\":\"auth\",\"access_token\":\"%s\"}",
             s_token ? s_token : "");
    ws_send(buf);
}

static void send_subscribe(void)
{
    char buf[256];
    /* Seed current state (one big array, parsed once). */
    s_states_req_id = s_msg_id++;
    snprintf(buf, sizeof(buf), "{\"id\":%d,\"type\":\"get_states\"}", s_states_req_id);
    ws_send(buf);

    /* Push only this entity's future state changes. */
    int sub_id = s_msg_id++;
    snprintf(buf, sizeof(buf),
             "{\"id\":%d,\"type\":\"subscribe_trigger\","
             "\"trigger\":{\"platform\":\"state\",\"entity_id\":\"%s\"}}",
             sub_id, s_entity ? s_entity : "");
    ws_send(buf);
}

/* call_service with optional service_data object body (without braces).
 * Returns true if the WebSocket send succeeded. */
static bool call_service(const char *domain, const char *service,
                         const char *service_data /* e.g. "\"x\":1" or NULL */)
{
    char buf[384];
    int id = s_msg_id++;
    if (service_data && service_data[0]) {
        snprintf(buf, sizeof(buf),
                 "{\"id\":%d,\"type\":\"call_service\",\"domain\":\"%s\","
                 "\"service\":\"%s\",\"target\":{\"entity_id\":\"%s\"},"
                 "\"service_data\":{%s}}",
                 id, domain, service, s_entity ? s_entity : "", service_data);
    } else {
        snprintf(buf, sizeof(buf),
                 "{\"id\":%d,\"type\":\"call_service\",\"domain\":\"%s\","
                 "\"service\":\"%s\",\"target\":{\"entity_id\":\"%s\"}}",
                 id, domain, service, s_entity ? s_entity : "");
    }
    return ws_send(buf);
}

bool ha_toggle_play_pause(void) { return call_service("media_player", "media_play_pause",     NULL); }
bool ha_prev_track(void)        { return call_service("media_player", "media_previous_track", NULL); }
bool ha_next_track(void)        { return call_service("media_player", "media_next_track",     NULL); }
bool ha_toggle_shuffle(void)
{
    bool new_state = !s_track.shuffle_state;
    char data[24];
    snprintf(data, sizeof(data), "\"shuffle\":%s", new_state ? "true" : "false");
    bool ok = call_service("media_player", "shuffle_set", data);
    if (ok) s_track.shuffle_state = new_state;
    return ok;
}

bool ha_seek_position(uint32_t position_ms)
{
    char data[48];
    snprintf(data, sizeof(data), "\"seek_position\":%u", (unsigned)(position_ms / 1000));
    return call_service("media_player", "media_seek", data);
}

bool ha_set_volume(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    char data[48];
    snprintf(data, sizeof(data), "\"volume_level\":%.2f", pct / 100.0);
    return call_service("media_player", "volume_set", data);
}

bool ha_play_album(const char *spotify_uri)
{
    /* "spotify:album:ID" -> "spotify://album/ID" for Music Assistant. */
    char media_id[160];
    const char *c1 = spotify_uri ? strchr(spotify_uri, ':') : NULL;       /* after "spotify" */
    const char *c2 = c1 ? strchr(c1 + 1, ':') : NULL;
    if (c1 && c2) {
        snprintf(media_id, sizeof(media_id), "spotify://%.*s/%s",
                 (int)(c2 - (c1 + 1)), c1 + 1, c2 + 1);
    } else {
        snprintf(media_id, sizeof(media_id), "%s", spotify_uri ? spotify_uri : "");
    }
    char data[256];
    snprintf(data, sizeof(data),
             "\"media_id\":\"%s\",\"media_type\":\"album\"", media_id);
    return call_service("music_assistant", "play_media", data);
}

/* ── Inbound (state parsing) ─────────────────────────────────────────────── */
static void apply_state_object(const char *st)
{
    if (!st || *st != '{') return;

    char state[24] = {0};
    json_obj_get_str(st, "state", state, sizeof(state));
    s_track.is_playing = (strcmp(state, "playing") == 0);

    const char *attrs = json_obj_get(st, "attributes");
    if (attrs && *attrs == '{') {
        json_obj_get_str(attrs, "media_title",      s_track.title,  sizeof(s_track.title));
        json_obj_get_str(attrs, "media_artist",     s_track.artist, sizeof(s_track.artist));
        json_obj_get_str(attrs, "media_album_name", s_track.album,  sizeof(s_track.album));

        double pos = 0, dur = 0;
        if (json_obj_get_double(attrs, "media_position", &pos))
            s_track.progress_ms = (uint32_t)(pos * 1000.0);
        if (json_obj_get_double(attrs, "media_duration", &dur))
            s_track.duration_ms = (uint32_t)(dur * 1000.0);

        char art[256];
        if (json_obj_get_str(attrs, "entity_picture", art, sizeof(art)) &&
            strcmp(art, s_track.album_art_url) != 0) {
            strncpy(s_track.album_art_url, art, sizeof(s_track.album_art_url) - 1);
            s_track.album_art_url[sizeof(s_track.album_art_url) - 1] = '\0';
            taskENTER_CRITICAL(&s_art_mux);
            strncpy(s_pending_art, art, sizeof(s_pending_art) - 1);
            s_pending_art[sizeof(s_pending_art) - 1] = '\0';
            s_art_pending = true;
            taskEXIT_CRITICAL(&s_art_mux);
        }

        s_track.volume_pct = -1;
        double vol = 0.0;
        if (json_obj_get_double(attrs, "volume_level", &vol))
            s_track.volume_pct = (int)(vol * 100.0 + 0.5);

        const char *shuf_p = json_obj_get(attrs, "shuffle");
        if (shuf_p)
            s_track.shuffle_state = (strncmp(shuf_p, "true", 4) == 0);

        /* Populate album_uri so the browser auto-snaps to the playing album.
         * Music Assistant reports the album URI under media_album_id;
         * the native Spotify integration uses media_content_id (track URI)
         * from which we can extract the album portion if it's an album type.
         * Leave empty when neither is present -- auto-snap silently no-ops. */
        char content_id[128] = {0};
        char content_type[32] = {0};
        json_obj_get_str(attrs, "media_content_id",   content_id,   sizeof(content_id));
        json_obj_get_str(attrs, "media_content_type", content_type, sizeof(content_type));
        if (strncmp(content_id, "spotify:album:", 14) == 0) {
            snprintf(s_track.album_uri, sizeof(s_track.album_uri), "%s", content_id);
        } else if (strncmp(content_id, "spotify://album/", 16) == 0) {
            /* Music Assistant format: spotify://album/<id> -> spotify:album:<id> */
            snprintf(s_track.album_uri, sizeof(s_track.album_uri),
                     "spotify:album:%s", content_id + 16);
        } else {
            s_track.album_uri[0] = '\0';
        }
    }

    ESP_LOGI(TAG, "state: %s -- %s [%s]", s_track.artist, s_track.title, state);
    ui_set_track_info(&s_track);
}

/* Iterate the state-array from get_states; find our entity and apply it. */
static const char *find_entity_in_array(const char *arr)
{
    if (!arr || *arr != '[') return NULL;
    const char *p = arr + 1;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',') p++;
        if (*p != '{') break;
        const char *obj = p;
        char eid[96];
        if (json_obj_get_str(obj, "entity_id", eid, sizeof(eid)) &&
            s_entity && strcmp(eid, s_entity) == 0) {
            return obj;
        }
        p = json_skip_value(p);  /* skip this object */
    }
    return NULL;
}

static void handle_message(const char *msg)
{
    char type[24] = {0};
    if (!json_obj_get_str(msg, "type", type, sizeof(type))) return;

    if (strcmp(type, "auth_required") == 0) {
        send_auth();
    } else if (strcmp(type, "auth_ok") == 0) {
        ESP_LOGI(TAG, "authenticated");
        send_subscribe();
    } else if (strcmp(type, "auth_invalid") == 0) {
        ESP_LOGE(TAG, "auth_invalid -- check HA_TOKEN");
    } else if (strcmp(type, "event") == 0) {
        /* subscribe_trigger payload: event.variables.trigger.to_state */
        const char *ev   = json_obj_get(msg, "event");
        const char *vars = ev   ? json_obj_get(ev,   "variables") : NULL;
        const char *trig = vars ? json_obj_get(vars, "trigger")   : NULL;
        const char *to   = trig ? json_obj_get(trig, "to_state")  : NULL;
        if (to) apply_state_object(to);
    } else if (strcmp(type, "result") == 0) {
        int id = 0;
        const char *idp = json_obj_get(msg, "id");
        if (idp) id = atoi(idp);
        if (id == s_states_req_id) {
            const char *arr = json_obj_get(msg, "result");
            const char *st  = find_entity_in_array(arr);
            if (st) apply_state_object(st);
            else ESP_LOGW(TAG, "entity %s not found in get_states", s_entity);
        }
    }
}

/* ── WebSocket event handler (reassembly + dispatch) ─────────────────────── */
static void ws_event_handler(void *arg, esp_event_base_t base,
                             int32_t event_id, void *event_data)
{
    (void)arg; (void)base;
    esp_websocket_event_data_t *d = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "ws connected");
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "ws disconnected");
            s_states_req_id = 0;
            break;
        case WEBSOCKET_EVENT_DATA: {
            /* op_code 1 = text, 0 = continuation. Ignore ping/pong/binary/close. */
            if (d->op_code != 1 && d->op_code != 0) break;
            if (d->payload_len <= 0 || d->payload_len > RX_MAX_CAP) break;

            if ((size_t)d->payload_len + 1 > s_rx_cap) {
                char *grown = realloc(s_rx, d->payload_len + 1);
                if (!grown) break;
                s_rx = grown;
                s_rx_cap = d->payload_len + 1;
            }
            memcpy(s_rx + d->payload_offset, d->data_ptr, d->data_len);
            if (d->payload_offset + d->data_len >= d->payload_len) {
                s_rx[d->payload_len] = '\0';
                handle_message(s_rx);
            }
            break;
        }
        default:
            break;
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */
void ha_client_init(const char *host, int port, const char *token, const char *entity)
{
    s_host   = host;
    s_port   = port;
    s_token  = token;
    s_entity = entity;
}

void ha_client_start(void)
{
    char uri[160];
    snprintf(uri, sizeof(uri), "ws://%s:%d/api/websocket", s_host, s_port);

    esp_websocket_client_config_t cfg = {
        .uri                 = uri,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms   = 10000,
        .buffer_size          = 4096,
        .task_stack           = 8192,
    };
    s_ws = esp_websocket_client_init(&cfg);
    if (!s_ws) {
        ESP_LOGE(TAG, "ws init failed");
        return;
    }
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);
    esp_websocket_client_start(s_ws);
    ESP_LOGI(TAG, "ws started -> %s", uri);
}

bool ha_take_pending_art(char *rel_out, size_t out_len)
{
    bool got = false;
    taskENTER_CRITICAL(&s_art_mux);
    if (s_art_pending) {
        strncpy(rel_out, s_pending_art, out_len - 1);
        rel_out[out_len - 1] = '\0';
        s_art_pending = false;
        got = true;
    }
    taskEXIT_CRITICAL(&s_art_mux);
    /* De-dupe: skip if we already loaded this URL. */
    if (got && strcmp(rel_out, s_art_loaded) == 0) return false;
    if (got) { strncpy(s_art_loaded, rel_out, sizeof(s_art_loaded) - 1);
               s_art_loaded[sizeof(s_art_loaded) - 1] = '\0'; }
    return got;
}

void ha_art_full_url(const char *rel, char *out, size_t out_len)
{
    /* entity_picture is an absolute-path URL on the HA host. */
    snprintf(out, out_len, "http://%s:%d%s", s_host, s_port, rel ? rel : "");
}

/* ── Album-art HTTP download to file (no TLS; local network) ─────────────── */
static esp_err_t art_file_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data) {
        FILE *f = (FILE *)evt->user_data;
        fwrite(evt->data, 1, evt->data_len, f);
    }
    return ESP_OK;
}

bool ha_download_to_file(const char *url, const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "wb");
    if (!f) { ESP_LOGE(TAG, "open %s failed", path); return false; }

    esp_http_client_config_t cfg = {
        .url           = url,
        .event_handler = art_file_event,
        .user_data     = f,
        .timeout_ms    = 8000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { fclose(f); return false; }

    /* HA's /api/media_player_proxy/ endpoints require authentication even on
     * the local network. Without the Bearer token they return 401. */
    if (s_token && s_token[0]) {
        char bearer[320];
        snprintf(bearer, sizeof(bearer), "Bearer %s", s_token);
        esp_http_client_set_header(c, "Authorization", bearer);
    }

    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    int len    = esp_http_client_get_content_length(c);
    esp_http_client_cleanup(c);
    fclose(f);

    if (out_len) *out_len = (len > 0) ? (size_t)len : 0;
    bool ok = (err == ESP_OK && status == 200);
    if (!ok) ESP_LOGW(TAG, "art GET failed (err=%d status=%d)", (int)err, status);
    return ok;
}
