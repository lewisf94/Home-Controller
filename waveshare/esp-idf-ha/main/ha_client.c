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
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "mbedtls/base64.h"

#include "player.h"    /* spotify_track_t -- backend-neutral contract from p4_shared */
#include "ui.h"        /* ui_set_track_info */

static const char *TAG = "ha";

/* ── Config ──────────────────────────────────────────────────────────────── */
static const char *s_host   = NULL;
static int         s_port   = 8123;
static const char *s_token  = NULL;
static char        s_entity_buf[96] = {0};
static const char *s_entity = s_entity_buf;   /* points at the buffer; runtime-switchable */

static const char *s_sp_client_id     = NULL;
static const char *s_sp_client_secret = NULL;
static const char *s_sp_refresh_token = NULL; /* retained for future saved-library fallback */
static char        s_sp_access_token[256] = {0};
static int64_t     s_sp_token_expiry_us = 0;

static esp_websocket_client_handle_t s_ws = NULL;
static int s_msg_id = 1;                 /* incrementing WS command id */
static int s_states_req_id = 0;          /* id of our now-playing get_states request */
static int s_devices_req_id = 0;         /* id of a device-list get_states request */
static int s_lights_req_id = 0;          /* id of a light-list get_states request */
static int s_album_entities_req_id = 0;  /* id of get_states for album-source discovery */
static int s_album_browse_req_id = 0;    /* id of a media_player/browse_media request */
static int s_album_browse_depth = 0;     /* follow at most a few folder layers */
static int s_sub_id = 0;                 /* id of the active subscribe_trigger (for unsubscribe) */

static spotify_track_t s_track = {0};

/* Pending album-art relative URL, set by the WS task, consumed by the ha task. */
static portMUX_TYPE s_art_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_pending_art[256] = {0};
static bool s_art_pending = false;
static char s_art_loaded[256] = {0};     /* last URL we already fetched */

/* Inbound frame reassembly (WS frames can arrive in chunks). */
static char  *s_rx     = NULL;
static size_t s_rx_cap = 0;
static bool   s_rx_dropping_oversize = false;
static bool   s_rx_drop_was_album = false;
#define RX_MAX_CAP (256 * 1024)

/* Must match ui.c's private ui_album_candidate_t layout. Kept local so the
 * shared public include/ folder does not need a private-folder include sync. */
#define HA_ALBUM_CANDIDATE_MAX 16
#define HA_ALBUM_BROWSE_MAX_DEPTH 3
#define HA_ALBUM_BROWSE_ENTITY_MAX 16
typedef struct {
    char title[80];
    char artist[56];
    char uri[64];
} ha_album_candidate_t;

static char s_album_browse_entities[HA_ALBUM_BROWSE_ENTITY_MAX][96];
static int  s_album_browse_entity_count = 0;
static int  s_album_browse_entity_next = 0;
static char s_album_browse_item_id[160] = {0};
static char s_album_browse_item_title[80] = {0};
static char s_album_browse_item_artist[56] = {0};
static int64_t s_album_pending_since_us = 0;
#define HA_ALBUM_REQ_TIMEOUT_US (10LL * 1000LL * 1000LL)

/* `err` NULL = success; else a short human reason shown on the add screen. */
void ui_set_album_candidates(const void *list, int count, const char *err);
void ui_show_toast(const char *msg, uint32_t ms_dur);
void ui_set_lights_ext(const ui_light_t *list, const int *hues, const int *sats, int count);

static bool media_player_is_unavailable(const char *obj);
static bool media_player_is_renderer(const char *eid, const char *attrs);

/* ── JSON scanner ────────────────────────────────────────────────────────── */
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

static bool json_array_get_two_doubles(const char *arr, double *a, double *b)
{
    if (!arr || *arr != '[') return false;
    const char *p = arr + 1;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p == ']') return false;
    *a = atof(p);
    while (*p && *p != ',' && *p != ']') p++;
    if (*p != ',') return false;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p == ']') return false;
    *b = atof(p);
    return true;
}

/* ── Outbound (WebSocket sends) ──────────────────────────────────────────── */
static bool ws_send(const char *json);

static bool ascii_contains_ci(const char *s, const char *needle)
{
    if (!s || !needle || !needle[0]) return false;
    size_t nl = strlen(needle);
    for (; *s; s++) {
        size_t i = 0;
        while (i < nl) {
            char a = s[i], b = needle[i];
            if (!a) return false;
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
            i++;
        }
        if (i == nl) return true;
    }
    return false;
}

static bool json_bool_is_true(const char *p)
{
    return p && strncmp(p, "true", 4) == 0;
}

static const char *json_array_first_obj(const char *arr)
{
    if (!arr || *arr != '[') return NULL;
    const char *p = arr + 1;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',') p++;
    return (*p == '{') ? p : NULL;
}

static const char *json_array_next_obj(const char *obj)
{
    const char *p = json_skip_value(obj);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',') p++;
    return (*p == '{') ? p : NULL;
}

static bool json_get_int(const char *json, const char *key, int *out)
{
    double v = 0;
    if (!json_obj_get_double(json, key, &v)) return false;
    *out = (int)v;
    return true;
}

static void json_escape(char *out, size_t out_len, const char *src)
{
    if (!out || out_len == 0) return;
    size_t i = 0;
    if (src) {
        while (*src && i + 1 < out_len) {
            char c = *src++;
            if ((c == '"' || c == '\\') && i + 2 < out_len) {
                out[i++] = '\\';
                out[i++] = c;
            } else if ((unsigned char)c < 0x20) {
                out[i++] = ' ';
            } else {
                out[i++] = c;
            }
        }
    }
    out[i] = '\0';
}

static bool is_spotify_id_char(char c)
{
    return (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9');
}

static bool spotify_album_uri_from_id(const char *id, char *out, size_t out_len)
{
    char clean[50];
    size_t i = 0;
    while (id && is_spotify_id_char(*id) && i + 1 < sizeof(clean))
        clean[i++] = *id++;
    clean[i] = '\0';
    if (i == 0) return false;
    snprintf(out, out_len, "spotify:album:%s", clean);
    return true;
}

static bool to_spotify_album_uri(const char *media_id, char *out, size_t out_len)
{
    if (!media_id || !out || out_len == 0) return false;
    if (strncmp(media_id, "spotify:album:", 14) == 0) {
        return spotify_album_uri_from_id(media_id + 14, out, out_len);
    }
    const char *p = strstr(media_id, "spotify://album/");
    if (p) {
        return spotify_album_uri_from_id(p + 16, out, out_len);
    }
    p = strstr(media_id, "://album/");
    if (p && ascii_contains_ci(media_id, "spotify")) {
        return spotify_album_uri_from_id(p + 9, out, out_len);
    }
    return false;
}

static void start_next_album_browse_entity(void);

static bool send_album_browse_for_entity(const char *entity, const char *media_id,
                                         const char *media_type, const char *title,
                                         const char *artist, int depth)
{
    char ent[128], id[192], typ[64], buf[560];
    json_escape(ent, sizeof(ent), entity ? entity : "");
    json_escape(id, sizeof(id), media_id ? media_id : "");
    json_escape(typ, sizeof(typ), media_type ? media_type : "");

    s_album_browse_req_id = s_msg_id++;
    s_album_browse_depth = depth;
    s_album_pending_since_us = esp_timer_get_time();
    snprintf(s_album_browse_item_id, sizeof(s_album_browse_item_id), "%s", media_id ? media_id : "");
    snprintf(s_album_browse_item_title, sizeof(s_album_browse_item_title), "%s", title ? title : "");
    snprintf(s_album_browse_item_artist, sizeof(s_album_browse_item_artist), "%s", artist ? artist : "");
    if (id[0]) {
        snprintf(buf, sizeof(buf),
                 "{\"id\":%d,\"type\":\"media_player/browse_media\","
                 "\"entity_id\":\"%s\",\"media_content_id\":\"%s\","
                 "\"media_content_type\":\"%s\"}",
                 s_album_browse_req_id, ent, id, typ[0] ? typ : "album");
    } else {
        snprintf(buf, sizeof(buf),
                 "{\"id\":%d,\"type\":\"media_player/browse_media\","
                 "\"entity_id\":\"%s\"}",
                 s_album_browse_req_id, ent);
    }

    ESP_LOGI(TAG, "browse albums source=%s depth=%d%s",
             entity ? entity : "", depth, id[0] ? " (+media_id)" : "");
    if (id[0]) {
        ESP_LOGI(TAG, "album browse item: title=%s artist=%s id=%s type=%s",
                 s_album_browse_item_title[0] ? s_album_browse_item_title : "(none)",
                 s_album_browse_item_artist[0] ? s_album_browse_item_artist : "(none)",
                 s_album_browse_item_id,
                 typ[0] ? typ : "(none)");
    }
    if (!ws_send(buf)) {
        s_album_browse_req_id = 0;
        s_album_pending_since_us = 0;
        ui_set_album_candidates(NULL, 0, "Home Assistant is not connected");
        return false;
    }
    return true;
}

static bool send_album_browse(const char *media_id, const char *media_type,
                              const char *title, const char *artist, int depth)
{
    int active = s_album_browse_entity_next - 1;
    const char *entity = (active >= 0 && active < s_album_browse_entity_count)
        ? s_album_browse_entities[active]
        : s_entity;
    return send_album_browse_for_entity(entity, media_id, media_type, title, artist, depth);
}

typedef struct {
    char media_id[160];
    char media_type[48];
    char title[80];
    char artist[56];
    int score;
} browse_follow_t;

static void consider_album_follow(const char *child, browse_follow_t *best)
{
    char title[80] = {0}, artist[56] = {0}, media_class[40] = {0}, media_type[48] = {0}, media_id[160] = {0};
    json_obj_get_str(child, "title", title, sizeof(title));
    json_obj_get_str(child, "subtitle", artist, sizeof(artist));
    if (!artist[0]) json_obj_get_str(child, "artist",  artist, sizeof(artist));
    if (!artist[0]) json_obj_get_str(child, "creator", artist, sizeof(artist));
    json_obj_get_str(child, "media_class", media_class, sizeof(media_class));
    json_obj_get_str(child, "media_content_type", media_type, sizeof(media_type));
    json_obj_get_str(child, "media_content_id", media_id, sizeof(media_id));

    bool expandable = json_bool_is_true(json_obj_get(child, "can_expand"));
    if (!expandable && !ascii_contains_ci(media_class, "directory")) return;

    int score = 0;
    if (ascii_contains_ci(title, "album") ||
        ascii_contains_ci(media_class, "album") ||
        ascii_contains_ci(media_type, "album") ||
        ascii_contains_ci(media_id, "album")) {
        score += 100;
    }
    if (ascii_contains_ci(title, "spotify") ||
        ascii_contains_ci(media_id, "spotify")) {
        score += 30;
    }
    if (ascii_contains_ci(title, "library") ||
        ascii_contains_ci(media_id, "library")) {
        score += 20;
    }

    if (score > best->score && media_id[0]) {
        best->score = score;
        snprintf(best->media_id, sizeof(best->media_id), "%.159s", media_id);
        snprintf(best->media_type, sizeof(best->media_type), "%.47s", media_type);
        snprintf(best->title, sizeof(best->title), "%.79s", title);
        snprintf(best->artist, sizeof(best->artist), "%.55s", artist);
    }
}

static bool album_candidate_from_child(const char *child, ha_album_candidate_t *out)
{
    char media_id[160] = {0}, media_class[40] = {0}, media_type[48] = {0};
    json_obj_get_str(child, "media_content_id", media_id, sizeof(media_id));
    json_obj_get_str(child, "media_class", media_class, sizeof(media_class));
    json_obj_get_str(child, "media_content_type", media_type, sizeof(media_type));

    bool album_like = ascii_contains_ci(media_class, "album") ||
                      ascii_contains_ci(media_type, "album") ||
                      ascii_contains_ci(media_id, "album");
    const char *can_play = json_obj_get(child, "can_play");
    if (!album_like || (can_play && !json_bool_is_true(can_play))) return false;

    memset(out, 0, sizeof(*out));
    if (!to_spotify_album_uri(media_id, out->uri, sizeof(out->uri))) return false;
    json_obj_get_str(child, "title", out->title, sizeof(out->title));
    json_obj_get_str(child, "subtitle", out->artist, sizeof(out->artist));
    if (!out->artist[0]) json_obj_get_str(child, "artist",  out->artist, sizeof(out->artist));
    if (!out->artist[0]) json_obj_get_str(child, "creator", out->artist, sizeof(out->artist));
    if (!out->artist[0]) snprintf(out->artist, sizeof(out->artist), "Music Assistant");
    return out->title[0] && out->uri[0];
}

static bool album_candidate_from_current_folder(const char *children, ha_album_candidate_t *out)
{
    if (!s_album_browse_item_id[0] || !s_album_browse_item_title[0]) return false;
    memset(out, 0, sizeof(*out));
    if (!to_spotify_album_uri(s_album_browse_item_id, out->uri, sizeof(out->uri))) return false;

    bool has_spotify_track = false;
    const char *child = json_array_first_obj(children);
    while (child) {
        char media_id[160] = {0}, media_class[40] = {0};
        json_obj_get_str(child, "media_content_id", media_id, sizeof(media_id));
        json_obj_get_str(child, "media_class", media_class, sizeof(media_class));
        if (ascii_contains_ci(media_id, "spotify") &&
            (ascii_contains_ci(media_id, "://track/") ||
             ascii_contains_ci(media_id, ":track:") ||
             ascii_contains_ci(media_class, "track"))) {
            has_spotify_track = true;
            break;
        }
        child = json_array_next_obj(child);
    }
    if (!has_spotify_track) return false;

    snprintf(out->title, sizeof(out->title), "%.79s", s_album_browse_item_title);
    snprintf(out->artist, sizeof(out->artist), "%.55s",
             s_album_browse_item_artist[0] ? s_album_browse_item_artist : "Music Assistant");
    return true;
}

static void log_album_child_sample(const char *children)
{
    const char *child = json_array_first_obj(children);
    int i = 0;
    while (child && i < 4) {
        char title[64] = {0}, media_class[32] = {0}, media_type[40] = {0}, media_id[96] = {0};
        json_obj_get_str(child, "title", title, sizeof(title));
        json_obj_get_str(child, "media_class", media_class, sizeof(media_class));
        json_obj_get_str(child, "media_content_type", media_type, sizeof(media_type));
        json_obj_get_str(child, "media_content_id", media_id, sizeof(media_id));
        ESP_LOGI(TAG, "album child[%d]: title=%s class=%s type=%s id=%s",
                 i,
                 title[0] ? title : "(none)",
                 media_class[0] ? media_class : "(none)",
                 media_type[0] ? media_type : "(none)",
                 media_id[0] ? media_id : "(none)");
        child = json_array_next_obj(child);
        i++;
    }
}

static void collect_album_browse_entities(const char *arr)
{
    s_album_browse_entity_count = 0;
    s_album_browse_entity_next = 0;

    if (arr && *arr == '[') {
        const char *p = arr + 1;
        while (*p && s_album_browse_entity_count < HA_ALBUM_BROWSE_ENTITY_MAX) {
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',') p++;
            if (*p != '{') break;
            const char *obj = p;
            char eid[96];
            if (json_obj_get_str(obj, "entity_id", eid, sizeof(eid)) &&
                strncmp(eid, "media_player.", 13) == 0) {
                if (!media_player_is_unavailable(obj)) {
                    const char *attrs = json_obj_get(obj, "attributes");
                    bool renderer = media_player_is_renderer(eid, attrs);
                    int dst = s_album_browse_entity_count++;
                    if (renderer && dst > 0) {
                        memmove(&s_album_browse_entities[1],
                                &s_album_browse_entities[0],
                                (size_t)dst * sizeof(s_album_browse_entities[0]));
                        dst = 0;
                    }
                    snprintf(s_album_browse_entities[dst],
                             sizeof(s_album_browse_entities[0]), "%s", eid);
                }
            }
            p = json_skip_value(p);
        }
    }

    /* Prefer the currently-selected player if it exists, but do not depend on
     * it. Add Albums is catalogue management, not playback-device management. */
    for (int i = 1; i < s_album_browse_entity_count; i++) {
        if (s_entity && strcmp(s_album_browse_entities[i], s_entity) == 0) {
            char tmp[96];
            snprintf(tmp, sizeof(tmp), "%s", s_album_browse_entities[0]);
            snprintf(s_album_browse_entities[0], sizeof(s_album_browse_entities[0]),
                     "%s", s_album_browse_entities[i]);
            snprintf(s_album_browse_entities[i], sizeof(s_album_browse_entities[i]),
                     "%s", tmp);
            break;
        }
    }

    ESP_LOGI(TAG, "album browse sources: %d media_player entities",
             s_album_browse_entity_count);
}

static void start_next_album_browse_entity(void)
{
    s_album_browse_req_id = 0;
    s_album_browse_depth = 0;
    while (s_album_browse_entity_next < s_album_browse_entity_count) {
        const char *entity = s_album_browse_entities[s_album_browse_entity_next++];
        if (send_album_browse_for_entity(entity, NULL, NULL, NULL, NULL, 0)) return;
    }
    s_album_pending_since_us = 0;
    ESP_LOGW(TAG, "album browse exhausted all HA media_player sources");
    ui_set_album_candidates(NULL, 0,
        "No Spotify albums found in HA media libraries - "
        "check Music Assistant exposes albums");
}

static void handle_album_browse_result(const char *result)
{
    static ha_album_candidate_t cands[HA_ALBUM_CANDIDATE_MAX];
    browse_follow_t follow = {0};
    int n = 0, seen = 0;
    char first_id[96] = {0}, first_class[40] = {0};

    const char *children = result ? json_obj_get(result, "children") : NULL;
    const char *child = json_array_first_obj(children);
    while (child) {
        if (seen++ == 0) {
            json_obj_get_str(child, "media_content_id", first_id, sizeof(first_id));
            json_obj_get_str(child, "media_class", first_class, sizeof(first_class));
        }
        if (n < HA_ALBUM_CANDIDATE_MAX &&
            album_candidate_from_child(child, &cands[n])) {
            n++;
        } else {
            consider_album_follow(child, &follow);
        }
        child = json_array_next_obj(child);
    }

    if (n == 0 && album_candidate_from_current_folder(children, &cands[n])) {
        ESP_LOGI(TAG, "album browse: current folder is album candidate id=%s title=%s",
                 s_album_browse_item_id, cands[n].title);
        n++;
    }

    if (n > 0) {
        ESP_LOGI(TAG, "album browse: %d candidates", n);
        log_album_child_sample(children);
        s_album_browse_req_id = 0;
        s_album_pending_since_us = 0;
        ui_set_album_candidates(cands, n, NULL);
        return;
    }

    if (follow.score > 0 && s_album_browse_depth < HA_ALBUM_BROWSE_MAX_DEPTH) {
        send_album_browse(follow.media_id, follow.media_type,
                          follow.title, follow.artist,
                          s_album_browse_depth + 1);
        return;
    }

    /* Dead end: log what this level actually contained so the follow heuristic
     * can be tuned against the real Music Assistant browse tree (its media_id
     * scheme varies by MA version -- see P4-TODO "album catalogue management"). */
    ESP_LOGW(TAG, "album browse: no playable Spotify albums found "
                  "(source=%d/%d depth=%d children=%d first_id=%s first_class=%s)",
             s_album_browse_entity_next, s_album_browse_entity_count,
             s_album_browse_depth, seen,
             first_id[0] ? first_id : "(none)",
             first_class[0] ? first_class : "(none)");
    log_album_child_sample(children);
    start_next_album_browse_entity();
}

static bool ws_send(const char *json)
{
    if (!s_ws || !esp_websocket_client_is_connected(s_ws)) {
        ESP_LOGW(TAG, "ws_send: not connected, dropping: %.40s...", json);
        return false;
    }
    int ret = esp_websocket_client_send_text(s_ws, json, strlen(json), pdMS_TO_TICKS(2000));
    return (ret >= 0);
}

/* Spotify HTTPS helpers for Add Albums search. Kept in the HA backend because
 * playback still goes through Home Assistant; only catalogue search talks
 * directly to Spotify. */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} spotify_resp_buf_t;

#define SPOTIFY_RESP_INITIAL_CAP 4096
#define SPOTIFY_RESP_MAX_CAP     65536
#define SPOTIFY_SEARCH_LIMIT_MAX 10

static esp_err_t spotify_http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    spotify_resp_buf_t *buf = (spotify_resp_buf_t *)evt->user_data;
    if (!buf) return ESP_OK;

    size_t need = buf->len + evt->data_len + 1;
    if (need > buf->cap) {
        size_t new_cap = buf->cap ? buf->cap : SPOTIFY_RESP_INITIAL_CAP;
        while (new_cap < need) new_cap *= 2;
        if (new_cap > SPOTIFY_RESP_MAX_CAP) {
            ESP_LOGW(TAG, "spotify response too large (%u B cap)",
                     (unsigned)SPOTIFY_RESP_MAX_CAP);
            return ESP_FAIL;
        }
        char *grown = realloc(buf->data, new_cap);
        if (!grown) return ESP_FAIL;
        buf->data = grown;
        buf->cap = new_cap;
    }
    memcpy(buf->data + buf->len, evt->data, evt->data_len);
    buf->len += evt->data_len;
    buf->data[buf->len] = '\0';
    return ESP_OK;
}

static bool spotify_basic_auth_header(char *out, size_t out_len)
{
    if (!s_sp_client_id || !s_sp_client_id[0] ||
        !s_sp_client_secret || !s_sp_client_secret[0]) {
        return false;
    }

    char joined[320];
    int n = snprintf(joined, sizeof(joined), "%s:%s",
                     s_sp_client_id, s_sp_client_secret);
    if (n <= 0 || n >= (int)sizeof(joined)) return false;

    unsigned char b64[448];
    size_t olen = 0;
    if (mbedtls_base64_encode(b64, sizeof(b64), &olen,
                              (const unsigned char *)joined, (size_t)n) != 0) {
        return false;
    }
    int m = snprintf(out, out_len, "Basic %.*s", (int)olen, b64);
    return (m > 0 && m < (int)out_len);
}

static bool spotify_client_token(char *err_out, size_t err_len)
{
    if (err_out && err_len) err_out[0] = '\0';
    if (s_sp_access_token[0] && esp_timer_get_time() < s_sp_token_expiry_us)
        return true;

    char auth[560];
    if (!spotify_basic_auth_header(auth, sizeof(auth))) {
        if (err_out) snprintf(err_out, err_len,
                              "Spotify credentials missing - check SETUP");
        return false;
    }

    spotify_resp_buf_t resp = {0};
    esp_http_client_config_t cfg = {
        .url               = "https://accounts.spotify.com/api/token",
        .method            = HTTP_METHOD_POST,
        .event_handler     = spotify_http_event_handler,
        .user_data         = &resp,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 6000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        if (err_out) snprintf(err_out, err_len, "Out of memory");
        return false;
    }

    const char body[] = "grant_type=client_credentials";
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    bool ok = false;
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    if (err == ESP_OK && status == 200 && resp.data) {
        char tok[256];
        int exp = 0;
        if (json_obj_get_str(resp.data, "access_token", tok, sizeof(tok)) &&
            json_get_int(resp.data, "expires_in", &exp)) {
            snprintf(s_sp_access_token, sizeof(s_sp_access_token), "%s", tok);
            int64_t lifetime_us = (int64_t)exp * 1000000LL;
            s_sp_token_expiry_us = esp_timer_get_time() + lifetime_us - 60LL * 1000000LL;
            ESP_LOGI(TAG, "spotify search token refreshed (expires in %d s)", exp);
            ok = true;
        } else {
            if (err_out) snprintf(err_out, err_len,
                                  "Spotify token response was unreadable");
        }
    } else {
        ESP_LOGW(TAG, "spotify token request failed (err=%d status=%d)",
                 (int)err, status);
        if (err_out) snprintf(err_out, err_len,
                              "Spotify sign-in failed - check client id/secret");
    }

    esp_http_client_cleanup(client);
    free(resp.data);
    return ok;
}

static bool url_is_unreserved(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' ||
           c == '.' || c == '~';
}

static bool url_encode(char *out, size_t out_len, const char *src)
{
    static const char hex[] = "0123456789ABCDEF";
    if (!out || out_len == 0) return false;
    size_t i = 0;
    for (const unsigned char *p = (const unsigned char *)(src ? src : ""); *p; p++) {
        if (url_is_unreserved(*p)) {
            if (i + 1 >= out_len) return false;
            out[i++] = (char)*p;
        } else if (*p == ' ') {
            if (i + 3 >= out_len) return false;
            out[i++] = '%'; out[i++] = '2'; out[i++] = '0';
        } else {
            if (i + 3 >= out_len) return false;
            out[i++] = '%';
            out[i++] = hex[*p >> 4];
            out[i++] = hex[*p & 0x0F];
        }
    }
    out[i] = '\0';
    return true;
}

static bool search_query_trim_copy(char *out, size_t out_len, const char *src)
{
    if (!out || out_len == 0) return false;
    out[0] = '\0';
    if (!src) return false;

    while (*src == ' ' || *src == '\t' || *src == '\n' || *src == '\r') src++;
    size_t len = strlen(src);
    while (len > 0) {
        char c = src[len - 1];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
        len--;
    }
    if (len == 0) return false;
    if (len >= out_len) len = out_len - 1;
    memcpy(out, src, len);
    out[len] = '\0';
    return true;
}

static void spotify_response_error_message(const char *json, char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    out[0] = '\0';
    const char *err = json_obj_get(json, "error");
    if (err && *err == '{') {
        json_obj_get_str(err, "message", out, out_len);
    }
}

static bool spotify_search_albums(const char *query, ha_album_candidate_t *out,
                                  int max, int *count, char *err_out,
                                  size_t err_len)
{
    if (err_out && err_len) err_out[0] = '\0';
    if (count) *count = 0;
    if (!query || !query[0] || !out || max <= 0) return false;
    if (max > SPOTIFY_SEARCH_LIMIT_MAX) max = SPOTIFY_SEARCH_LIMIT_MAX;

    if (!spotify_client_token(err_out, err_len)) return false;

    char trimmed[80];
    if (!search_query_trim_copy(trimmed, sizeof(trimmed), query)) {
        if (err_out) snprintf(err_out, err_len, "Enter an album or artist");
        return false;
    }

    char q[260];
    if (!url_encode(q, sizeof(q), trimmed)) {
        if (err_out) snprintf(err_out, err_len, "Search text is too long");
        return false;
    }

    char url[384];
    snprintf(url, sizeof(url),
             "https://api.spotify.com/v1/search?q=%s&type=album&limit=%d",
             q, max);

    spotify_resp_buf_t resp = {0};
    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_GET,
        .event_handler     = spotify_http_event_handler,
        .user_data         = &resp,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 7000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        if (err_out) snprintf(err_out, err_len, "Out of memory");
        return false;
    }

    char bearer[320];
    snprintf(bearer, sizeof(bearer), "Bearer %s", s_sp_access_token);
    esp_http_client_set_header(client, "Authorization", bearer);

    esp_err_t herr = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    int n = 0;
    bool ok = false;
    if (herr == ESP_OK && status == 200 && resp.data) {
        const char *albums = json_obj_get(resp.data, "albums");
        const char *item = json_array_first_obj(json_obj_get(albums, "items"));
        while (item && n < max) {
            ha_album_candidate_t *a = &out[n];
            memset(a, 0, sizeof(*a));
            const char *v;
            if ((v = json_obj_get(item, "name")))
                json_copy_string(v, a->title, sizeof(a->title));
            if ((v = json_obj_get(item, "uri")))
                json_copy_string(v, a->uri, sizeof(a->uri));
            const char *artist = json_array_first_obj(json_obj_get(item, "artists"));
            if (artist && (v = json_obj_get(artist, "name")))
                json_copy_string(v, a->artist, sizeof(a->artist));
            if (!a->artist[0])
                snprintf(a->artist, sizeof(a->artist), "Unknown artist");
            if (a->title[0] && strncmp(a->uri, "spotify:album:", 14) == 0)
                n++;

            item = json_array_next_obj(item);
        }
        ok = true;
    } else if (status == 401) {
        ESP_LOGW(TAG, "spotify album search got 401, invalidating search token");
        s_sp_access_token[0] = '\0';
        s_sp_token_expiry_us = 0;
        if (err_out) snprintf(err_out, err_len,
                              "Spotify rejected the search token");
    } else {
        char api_msg[96] = {0};
        spotify_response_error_message(resp.data, api_msg, sizeof(api_msg));
        ESP_LOGW(TAG, "spotify album search failed (err=%d status=%d msg=%s)",
                 (int)herr, status, api_msg[0] ? api_msg : "-");
        if (err_out) snprintf(err_out, err_len, "%s",
                              api_msg[0] ? api_msg : "Spotify album search failed");
    }

    free(resp.data);
    if (count) *count = n;
    if (ok) ESP_LOGI(TAG, "spotify album search: %d results for \"%s\"", n, trimmed);
    return ok;
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

    /* Push only this entity's future state changes. Remember the id so a later
     * entity switch can unsubscribe this trigger. */
    s_sub_id = s_msg_id++;
    snprintf(buf, sizeof(buf),
             "{\"id\":%d,\"type\":\"subscribe_trigger\","
             "\"trigger\":{\"platform\":\"state\",\"entity_id\":\"%s\"}}",
             s_sub_id, s_entity ? s_entity : "");
    ws_send(buf);
}

/* call_service targeting an explicit entity_id, with an optional service_data
 * object body (without braces). Returns true if the WebSocket send succeeded. */
static bool call_service_entity(const char *domain, const char *service,
                                const char *entity_id,
                                const char *service_data /* e.g. "\"x\":1" or NULL */)
{
    char buf[384];
    int id = s_msg_id++;
    if (service_data && service_data[0]) {
        snprintf(buf, sizeof(buf),
                 "{\"id\":%d,\"type\":\"call_service\",\"domain\":\"%s\","
                 "\"service\":\"%s\",\"target\":{\"entity_id\":\"%s\"},"
                 "\"service_data\":{%s}}",
                 id, domain, service, entity_id ? entity_id : "", service_data);
    } else {
        snprintf(buf, sizeof(buf),
                 "{\"id\":%d,\"type\":\"call_service\",\"domain\":\"%s\","
                 "\"service\":\"%s\",\"target\":{\"entity_id\":\"%s\"}}",
                 id, domain, service, entity_id ? entity_id : "");
    }
    ESP_LOGI(TAG, "call_service id=%d %s.%s -> %s%s",
             id, domain, service, entity_id ? entity_id : "",
             (service_data && service_data[0]) ? " (+data)" : "");
    return ws_send(buf);
}

/* call_service against the active media_player (s_entity) -- every playback
 * command below targets it, so this stays the short call site they use. */
static bool call_service(const char *domain, const char *service,
                         const char *service_data)
{
    return call_service_entity(domain, service, s_entity, service_data);
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

bool ha_light_toggle(const char *entity_id)
{
    return call_service_entity("light", "toggle", entity_id, NULL);
}

bool ha_light_set_brightness(const char *entity_id, int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    char data[32];
    snprintf(data, sizeof(data), "\"brightness_pct\":%d", pct);
    return call_service_entity("light", "turn_on", entity_id, data);
}

bool ha_light_set_hs(const char *entity_id, int hue_deg, int sat_pct)
{
    if (hue_deg < 0) hue_deg = 0;
    hue_deg %= 360;
    if (sat_pct < 1) sat_pct = 1;
    if (sat_pct > 100) sat_pct = 100;
    char data[48];
    snprintf(data, sizeof(data), "\"hs_color\":[%d,%d]", hue_deg, sat_pct);
    return call_service_entity("light", "turn_on", entity_id, data);
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
        json_obj_get_str(attrs, "friendly_name",    s_track.device_name, sizeof(s_track.device_name));

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
                     "spotify:album:%.49s", content_id + 16);
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

/* Full entity_ids for the current device list. ui_device_t.id (64 B) is too
 * small for long HA entity_ids, so the UI rows carry the list index instead and
 * ha_set_active_entity() resolves it back here. Written on the WS task (build),
 * read on the ha task (switch); the list is always built before it can be tapped. */
static char s_dev_ids[MAX_DEVICES][96];
static bool s_dev_playable[MAX_DEVICES];
static int  s_dev_count = 0;

static bool media_player_is_unavailable(const char *obj)
{
    char state[24] = {0};
    json_obj_get_str(obj, "state", state, sizeof(state));
    return strcmp(state, "unavailable") == 0;
}

static bool media_player_is_renderer(const char *eid, const char *attrs)
{
    char fname[64] = {0};
    if (attrs) json_obj_get_str(attrs, "friendly_name", fname, sizeof(fname));
    return ascii_contains_ci(eid, "media_renderer") ||
           ascii_contains_ci(fname, "media renderer") ||
           ascii_contains_ci(fname, "sonos");
}

static void media_player_state(const char *obj, char *out, size_t out_len)
{
    if (!json_obj_get_str(obj, "state", out, out_len))
        snprintf(out, out_len, "unknown");
}

/* Build the device list from a get_states result array: every media_player
 * entity becomes one row (name = friendly_name, detail = entity_id tail so
 * duplicate friendly names stay distinguishable). Pushed straight to the UI. */
static void build_device_list(const char *arr)
{
    static ui_device_t devs[MAX_DEVICES];   /* static: only ever touched on the WS task */
    int n = 0;
    int skipped_unavailable = 0;
    if (arr && *arr == '[') {
        const char *p = arr + 1;
        while (*p && n < MAX_DEVICES) {
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',') p++;
            if (*p != '{') break;
            const char *obj = p;
            char eid[96];
            if (json_obj_get_str(obj, "entity_id", eid, sizeof(eid)) &&
                strncmp(eid, "media_player.", 13) == 0) {
                if (media_player_is_unavailable(obj)) {
                    ESP_LOGI(TAG, "device: %s state=unavailable (skipped)", eid);
                    skipped_unavailable++;
                    p = json_skip_value(p);
                    continue;
                }
                ui_device_t *d = &devs[n];
                memset(d, 0, sizeof(*d));

                snprintf(s_dev_ids[n], sizeof(s_dev_ids[n]), "%s", eid);
                snprintf(d->id, sizeof(d->id), "%d", n);   /* UI carries the list index */

                const char *attrs = json_obj_get(obj, "attributes");
                char fname[64] = {0};
                char state[24] = {0};
                if (attrs) json_obj_get_str(attrs, "friendly_name", fname, sizeof(fname));
                media_player_state(obj, state, sizeof(state));
                bool renderer = media_player_is_renderer(eid, attrs);
                snprintf(d->name, sizeof(d->name), "%.39s", fname[0] ? fname : eid + 13);
                snprintf(d->detail, sizeof(d->detail), "%s %.14s",
                         renderer ? "READY" : "OTHER", state);

                d->is_active = (strcmp(eid, s_entity) == 0);
                d->is_sonos  = false;   /* all HA entities switch via ui_request_transfer */
                s_dev_playable[n] = renderer;
                ESP_LOGI(TAG, "device[%d]: %s name='%s' state=%s kind=%s",
                         n, eid, d->name, state, renderer ? "renderer" : "other");
                n++;
            }
            p = json_skip_value(p);
        }
    }
    s_dev_count = n;
    ESP_LOGI(TAG, "devices: %d available media_player entities (%d unavailable skipped)",
             n, skipped_unavailable);
    ui_set_devices(devs, n);
}

static bool select_best_available_device(void)
{
    if (s_dev_count <= 0) return false;
    int pick = 0;
    for (int i = 0; i < s_dev_count; i++) {
        if (s_dev_playable[i]) {
            pick = i;
            break;
        }
    }
    if (!s_dev_ids[pick][0] || strcmp(s_dev_ids[pick], s_entity) == 0) return false;
    ESP_LOGW(TAG, "active entity missing; auto-selecting %s", s_dev_ids[pick]);
    ha_set_active_entity(s_dev_ids[pick]);
    return true;
}

/* Build the lights list from a get_states result array: every light entity
 * becomes one row. Unlike build_device_list(), ui_light_t.entity_id (96 B)
 * comfortably fits a full HA entity_id, so no index-indirection table is
 * needed -- ha_light_toggle()/ha_light_set_brightness() take it directly. */
static void build_light_list(const char *arr)
{
    static ui_light_t lights[MAX_LIGHTS];   /* static: only ever touched on the WS task */
    static int hues[MAX_LIGHTS];
    static int sats[MAX_LIGHTS];
    int n = 0;
    int skipped_unavailable = 0;
    if (arr && *arr == '[') {
        const char *p = arr + 1;
        while (*p && n < MAX_LIGHTS) {
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',') p++;
            if (*p != '{') break;
            const char *obj = p;
            char eid[96];
            if (json_obj_get_str(obj, "entity_id", eid, sizeof(eid)) &&
                strncmp(eid, "light.", 6) == 0) {
                ui_light_t *l = &lights[n];
                memset(l, 0, sizeof(*l));

                snprintf(l->entity_id, sizeof(l->entity_id), "%s", eid);

                char state[16] = {0};
                json_obj_get_str(obj, "state", state, sizeof(state));
                if (strcmp(state, "unavailable") == 0) {
                    ESP_LOGI(TAG, "light: %s state=unavailable (skipped)", eid);
                    skipped_unavailable++;
                    p = json_skip_value(p);
                    continue;
                }
                l->is_on = (strcmp(state, "on") == 0);

                const char *attrs = json_obj_get(obj, "attributes");
                char fname[64] = {0};
                if (attrs) json_obj_get_str(attrs, "friendly_name", fname, sizeof(fname));
                snprintf(l->name, sizeof(l->name), "%.39s", fname[0] ? fname : eid + 6);

                /* HA reports brightness 0-255; a light usually omits the
                 * attribute entirely while off. If HA says the light supports
                 * colour/brightness modes, keep controls visible anyway so a
                 * preset tap can turn it on at the chosen level. */
                l->brightness_pct = -1;
                double bri = 0.0;
                if (attrs && json_obj_get_double(attrs, "brightness", &bri))
                    l->brightness_pct = (int)(bri * 100.0 / 255.0 + 0.5);

                hues[n] = -1;
                sats[n] = 100;
                const char *modes = attrs ? json_obj_get(attrs, "supported_color_modes") : NULL;
                bool supports_level = modes && !ascii_contains_ci(modes, "onoff");
                bool supports_colour = modes &&
                    (ascii_contains_ci(modes, "hs") ||
                     ascii_contains_ci(modes, "xy") ||
                     ascii_contains_ci(modes, "rgb"));
                if (l->brightness_pct < 0 && supports_level)
                    l->brightness_pct = 100;
                const char *hs = attrs ? json_obj_get(attrs, "hs_color") : NULL;
                double hue = 0.0, sat = 0.0;
                if (json_array_get_two_doubles(hs, &hue, &sat)) {
                    if (hue < 0.0) hue = 0.0;
                    if (hue > 359.0) hue = 359.0;
                    if (sat < 1.0) sat = 1.0;
                    if (sat > 100.0) sat = 100.0;
                    hues[n] = (int)(hue + 0.5);
                    sats[n] = (int)(sat + 0.5);
                } else if (supports_colour) {
                    hues[n] = 28;
                    sats[n] = 66;
                }

                ESP_LOGI(TAG, "light: %s state=%s brightness=%d%% hue=%d sat=%d",
                         eid, state[0] ? state : "(missing)", l->brightness_pct,
                         hues[n], sats[n]);
                n++;
            }
            p = json_skip_value(p);
        }
    }
    ESP_LOGI(TAG, "lights: %d available light entities (%d unavailable skipped)",
             n, skipped_unavailable);
    ui_set_lights_ext(lights, hues, sats, n);
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
        /* Guard against a stale event from a not-yet-unsubscribed old
         * trigger (ha_set_active_entity's unsubscribe is fire-and-forget)
         * landing after the active entity has already switched. */
        char eid[96];
        if (to && json_obj_get_str(to, "entity_id", eid, sizeof(eid)) &&
            strcmp(eid, s_entity) == 0) {
            apply_state_object(to);
        }
    } else if (strcmp(type, "result") == 0) {
        int id = 0;
        const char *idp = json_obj_get(msg, "id");
        if (idp) id = atoi(idp);
        const char *success = json_obj_get(msg, "success");
        bool failed = (success && strncmp(success, "false", 5) == 0);
        char err_msg[128] = {0};
        if (failed) {
            const char *err = json_obj_get(msg, "error");
            if (!json_obj_get_str(err, "message", err_msg, sizeof(err_msg)))
                snprintf(err_msg, sizeof(err_msg), "unknown error");
            ESP_LOGW(TAG, "result id=%d FAILED: %s", id, err_msg);
        }
        if (id == s_states_req_id) {
            const char *arr = json_obj_get(msg, "result");
            const char *st  = find_entity_in_array(arr);
            if (st) apply_state_object(st);
            else {
                ESP_LOGW(TAG, "entity %s not found in get_states; open Devices and pick a valid media_player",
                         s_entity ? s_entity : "");
                build_device_list(arr);
                if (!select_best_available_device())
                    ui_show_toast("HA media player not found - open Devices", 3500);
            }
        } else if (s_devices_req_id && id == s_devices_req_id) {
            build_device_list(json_obj_get(msg, "result"));
            s_devices_req_id = 0;
        } else if (s_lights_req_id && id == s_lights_req_id) {
            build_light_list(json_obj_get(msg, "result"));
            s_lights_req_id = 0;
        } else if (s_album_entities_req_id && id == s_album_entities_req_id) {
            s_album_entities_req_id = 0;
            s_album_pending_since_us = 0;
            if (failed) {
                ui_set_album_candidates(NULL, 0,
                    "Home Assistant could not list media players");
            } else {
                collect_album_browse_entities(json_obj_get(msg, "result"));
                if (s_album_browse_entity_count == 0) {
                    ui_set_album_candidates(NULL, 0,
                        "No HA media_player entities found");
                } else {
                    start_next_album_browse_entity();
                }
            }
        } else if (s_album_browse_req_id && id == s_album_browse_req_id) {
            if (failed) {
                ESP_LOGW(TAG, "album browse source failed, trying next source: %s", err_msg);
                start_next_album_browse_entity();
            } else {
                handle_album_browse_result(json_obj_get(msg, "result"));
            }
        } else if (!failed && id) {
            ESP_LOGI(TAG, "result id=%d OK", id);
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
            s_album_entities_req_id = 0;
            s_album_pending_since_us = 0;
            if (s_album_browse_req_id) {
                s_album_browse_req_id = 0;
                ui_set_album_candidates(NULL, 0, "Home Assistant disconnected");
            }
            break;
        case WEBSOCKET_EVENT_DATA: {
            /* op_code 1 = text, 0 = continuation. Ignore ping/pong/binary/close. */
            if (d->op_code != 1 && d->op_code != 0) break;
            if (d->payload_len <= 0) break;
            if (d->payload_len > RX_MAX_CAP) {
                if (d->payload_offset == 0) {
                    s_rx_dropping_oversize = true;
                    s_rx_drop_was_album = (s_album_browse_req_id != 0);
                    ESP_LOGW(TAG, "ws frame too large (%d B > %d B), dropping",
                             d->payload_len, RX_MAX_CAP);
                }
                if (d->payload_offset + d->data_len >= d->payload_len) {
                    bool was_album = s_rx_drop_was_album;
                    s_rx_dropping_oversize = false;
                    s_rx_drop_was_album = false;
                    if (was_album && s_album_browse_req_id) {
                        ESP_LOGW(TAG, "album browse response too large, trying next source");
                        start_next_album_browse_entity();
                    }
                }
                break;
            }
            if (d->payload_offset == 0) {
                s_rx_dropping_oversize = false;
                s_rx_drop_was_album = false;
            } else if (s_rx_dropping_oversize) {
                break;
            }

            if ((size_t)d->payload_len + 1 > s_rx_cap) {
                char *grown = realloc(s_rx, d->payload_len + 1);
                if (!grown) {
                    ESP_LOGW(TAG, "ws rx realloc failed for %d B", d->payload_len + 1);
                    if (s_album_browse_req_id) start_next_album_browse_entity();
                    break;
                }
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
    /* Copy into a writable buffer so the active entity can be switched at runtime
     * (s_entity already points at s_entity_buf). */
    snprintf(s_entity_buf, sizeof(s_entity_buf), "%s", entity ? entity : "");
}

void ha_spotify_init(const char *client_id, const char *client_secret,
                     const char *refresh_token)
{
    s_sp_client_id = client_id;
    s_sp_client_secret = client_secret;
    s_sp_refresh_token = refresh_token;
    s_sp_access_token[0] = '\0';
    s_sp_token_expiry_us = 0;
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

void ha_client_tick(void)
{
    if (!s_album_pending_since_us) return;
    int64_t now = esp_timer_get_time();
    if (now - s_album_pending_since_us < HA_ALBUM_REQ_TIMEOUT_US) return;

    if (s_album_entities_req_id) {
        ESP_LOGW(TAG, "album source discovery timed out");
        s_album_entities_req_id = 0;
        s_album_pending_since_us = 0;
        ui_set_album_candidates(NULL, 0,
            "Home Assistant did not return media players");
    } else if (s_album_browse_req_id) {
        ESP_LOGW(TAG, "album browse timed out, trying next source");
        start_next_album_browse_entity();
    }
}

void ha_request_devices(void)
{
    /* Re-pull every entity's state; the result handler filters media_player.* */
    char buf[64];
    s_devices_req_id = s_msg_id++;
    snprintf(buf, sizeof(buf), "{\"id\":%d,\"type\":\"get_states\"}", s_devices_req_id);
    if (!ws_send(buf)) s_devices_req_id = 0;
}

void ha_request_album_candidates(const char *query)
{
    if (query && query[0]) {
        static ha_album_candidate_t cands[HA_ALBUM_CANDIDATE_MAX];
        char err[160] = {0};
        int n = 0;
        bool ok = spotify_search_albums(query, cands, HA_ALBUM_CANDIDATE_MAX,
                                        &n, err, sizeof(err));
        ui_set_album_candidates(ok ? cands : NULL, ok ? n : 0,
                                ok ? (n ? NULL : "No Spotify albums matched")
                                   : (err[0] ? err : "Spotify album search failed"));
        return;
    }

    s_album_browse_req_id = 0;
    s_album_browse_depth = 0;
    s_album_browse_entity_count = 0;
    s_album_browse_entity_next = 0;

    char buf[64];
    s_album_entities_req_id = s_msg_id++;
    s_album_pending_since_us = esp_timer_get_time();
    snprintf(buf, sizeof(buf), "{\"id\":%d,\"type\":\"get_states\"}", s_album_entities_req_id);
    if (!ws_send(buf)) {
        s_album_entities_req_id = 0;
        s_album_pending_since_us = 0;
        ui_set_album_candidates(NULL, 0, "Home Assistant is not connected");
    }
}

void ha_request_lights(void)
{
    /* Re-pull every entity's state; the result handler filters light.* */
    char buf[64];
    s_lights_req_id = s_msg_id++;
    snprintf(buf, sizeof(buf), "{\"id\":%d,\"type\":\"get_states\"}", s_lights_req_id);
    if (!ws_send(buf)) s_lights_req_id = 0;
}

void ha_set_active_entity(const char *sel)
{
    if (!sel || !sel[0]) return;

    /* The UI rows carry the device-list index (entity_ids are too long for
     * ui_device_t.id); resolve it back to the full entity_id. */
    const char *entity = sel;
    char *end = NULL;
    long idx = strtol(sel, &end, 10);
    if (end && *end == '\0' && idx >= 0 && idx < s_dev_count && s_dev_ids[idx][0])
        entity = s_dev_ids[idx];

    if (strcmp(entity, s_entity) == 0) return;   /* already the active entity */

    /* Stop the previous entity's trigger so its events stop overwriting state
     * (the entity_id check in handle_message's "event" branch is the backstop
     * if this send fails or a stale event is already in flight -- but only
     * forget s_sub_id once HA has actually been asked to drop it, so a failed
     * send can still be retried by a later switch instead of leaking the
     * subscription for the rest of the session). */
    if (s_sub_id) {
        char buf[96];
        snprintf(buf, sizeof(buf),
                 "{\"id\":%d,\"type\":\"unsubscribe_events\",\"subscription\":%d}",
                 s_msg_id++, s_sub_id);
        if (ws_send(buf)) {
            s_sub_id = 0;
        } else {
            ESP_LOGW(TAG, "unsubscribe send failed, subscription %d left active", s_sub_id);
        }
    }

    snprintf(s_entity_buf, sizeof(s_entity_buf), "%s", entity);
    s_art_loaded[0]          = '\0';   /* force the new entity's art to reload */
    s_track.album_art_url[0] = '\0';
    ESP_LOGI(TAG, "active entity -> %s", s_entity);

    send_subscribe();   /* fresh get_states (refreshes now-playing) + new trigger */
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
    if (!rel) { if (out_len) out[0] = '\0'; return; }
    /* Music Assistant returns an already-absolute entity_picture
     * (e.g. http://<ma-host>:8095/imageproxy?...). The native HA media proxy
     * instead gives an absolute PATH (/api/media_player_proxy/...). Use a full
     * URL as-is; only prefix the HA host:port when it's a bare path. */
    if (strncmp(rel, "http://", 7) == 0 || strncmp(rel, "https://", 8) == 0)
        snprintf(out, out_len, "%s", rel);
    else
        snprintf(out, out_len, "http://%s:%d%s", s_host, s_port, rel);
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
