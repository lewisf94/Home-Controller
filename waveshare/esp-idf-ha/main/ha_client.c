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
#include "esp_heap_caps.h"
#include "esp_websocket_client.h"
#include "app_core_reliability.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "mbedtls/base64.h"

#include "player.h"    /* spotify_track_t -- backend-neutral contract from p4_shared */
#include "ui.h"        /* ui_set_track_info */

bool p4_json_copy_string(const char *p, char *out, size_t out_len);

static const char *TAG = "ha";

/* ── Config ──────────────────────────────────────────────────────────────── */
static const char *s_host   = NULL;
static int         s_port   = 8123;
static const char *s_token  = NULL;
static char        s_entity_buf[96] = {0};
static const char *s_entity = s_entity_buf;   /* points at the buffer; runtime-switchable */
static bool        s_active_is_ma = false;

static const char *s_sp_client_id     = NULL;
static const char *s_sp_client_secret = NULL;
static const char *s_sp_refresh_token = NULL; /* retained for future saved-library fallback */
static char        s_sp_access_token[256] = {0};
static int64_t     s_sp_token_expiry_us = 0;

static esp_websocket_client_handle_t s_ws = NULL;
static volatile bool s_authenticated = false;
static volatile bool s_initial_state_received = false;
static int s_msg_id = 1;                 /* incrementing WS command id */
static int s_states_req_id = 0;          /* id of our now-playing get_states request */
static int s_inventory_req_id = 0;       /* shared devices/lights inventory snapshot */
static int s_queue_req_id = 0;           /* Music Assistant get_queue response */
static int64_t s_inventory_pending_since_us = 0;
static int64_t s_inventory_last_us = 0;
static int64_t s_lights_settle_due_us = 0;   /* coalesced post-light-command refresh deadline */
static int64_t s_forced_inventory_last_us = 0; /* last cooldown-bypassing snapshot */
static int s_album_browse_req_id = 0;    /* id of a media_player/browse_media request */
static int s_album_browse_depth = 0;     /* follow at most a few folder layers */
static int s_sub_id = 0;                 /* id of the active subscribe_trigger (for unsubscribe) */
static volatile bool s_subscribe_pending = false;
static volatile bool s_ws_restart_pending = false; /* server closed cleanly; tick must restart the client */
static volatile bool s_auth_rejected = false;      /* last auth attempt answered auth_invalid */
static int64_t s_ws_restart_due_us = 0;            /* written by the ha task only */
static int64_t s_last_rx_us = 0;                   /* last inbound WS frame (heartbeat) */
static int64_t s_ping_await_us = 0;                /* heartbeat ping sent-at; 0 = not awaiting a pong */

/* Small, fixed diagnostics tables keep command timing observable without
 * allocating on the HA path. A late result can overwrite its modulo slot; the
 * ID check then suppresses a misleading measurement. */
#define HA_DIAG_REQUEST_SLOTS 8
typedef struct {
    int id;
    int64_t sent_us;
} ha_diag_request_t;
static ha_diag_request_t s_diag_requests[HA_DIAG_REQUEST_SLOTS];
static uint32_t s_diag_track_seq = 0;

static spotify_track_t s_track = {0};
static char s_media_content_id[160] = {0};
static char s_media_content_type[32] = {0};

typedef enum {
    TRANSFER_TARGET_NONE = 0,
    TRANSFER_TARGET_SPOTIFY,
    TRANSFER_TARGET_MA,
    TRANSFER_TARGET_HA,
} transfer_target_t;
static transfer_target_t s_transfer_target = TRANSFER_TARGET_NONE;
static char     s_transfer_entity[96] = {0};
static char     s_transfer_media_id[160] = {0};
static char     s_transfer_media_type[32] = {0};
static char     s_transfer_title[80] = {0};
static char     s_transfer_artist[64] = {0};
static uint32_t s_transfer_position_ms = 0;
static int64_t  s_transfer_due_us = 0;
static bool     s_transfer_seek_pending = false;

/* HA core Spotify integration support. That integration creates one account
 * media_player whose `source_list` holds the Connect devices known to Spotify.
 * HA permits renaming its entity_id, so detection cannot rely only on the
 * historical media_player.spotify_* prefix. Each source becomes a row; tapping
 * it calls select_source and follows the account entity. Multiple accounts:
 * the last entity seen wins (rare enough not to engineer for). */
#define SPOTIFY_SRC_PREFIX "spotify-src:"
#define MAX_SPOTIFY_SOURCES 8
static char s_spotify_entity[96] = "";
static char s_spotify_sources[MAX_SPOTIFY_SOURCES][40];
static int  s_spotify_source_count = 0;
static char s_spotify_source_now[40] = "";

/* Pending album-art relative URL, set by the WS task, consumed by the ha task. */
static portMUX_TYPE s_art_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_pending_art[256] = {0};
static bool s_art_pending = false;
static char s_art_loaded[256] = {0};     /* last URL we already fetched */
static uint32_t s_pending_art_seq = 0;
static int64_t s_pending_art_event_us = 0;

/* Inbound frame reassembly (WS frames can arrive in chunks). */
static char  *s_rx     = NULL;
static size_t s_rx_cap = 0;
static bool   s_rx_dropping_oversize = false;
static bool   s_rx_drop_was_album = false;
static bool   s_rx_drop_was_devices = false;
/* HA's get_states result contains every entity. A moderately busy installation
 * can exceed 256 KB, so keep the reassembly buffer in PSRAM and allow a
 * realistic full snapshot without taking memory from the display/codec. */
#define RX_MAX_CAP (768 * 1024)

/* Must match ui.c's private ui_album_candidate_t layout. Kept local so the
 * shared public include/ folder does not need a private-folder include sync. */
#define HA_ALBUM_CANDIDATE_MAX 16
#define HA_QUEUE_ITEM_MAX 12
#define HA_ALBUM_BROWSE_MAX_DEPTH 3
#define HA_ALBUM_BROWSE_ENTITY_MAX 16
#define HA_LIGHT_VALUE_UNKNOWN_SUPPORTED (-2)
typedef struct {
    char title[80];
    char artist[56];
    char uri[64];
    char image_url[100];   /* cover art URL (Spotify search only); "" otherwise */
} ha_album_candidate_t;

/* Must match ui.c's private ui_queue_item_t layout. Kept local for the same
 * reason as ha_album_candidate_t: these are backend/UI seam data, not a public
 * component contract yet. */
typedef struct {
    char title[80];
    char artist[56];
    char uri[96];
    bool is_current;
} ha_queue_item_t;

static char s_album_browse_entities[HA_ALBUM_BROWSE_ENTITY_MAX][96];
static int  s_album_browse_entity_count = 0;
static int  s_album_browse_entity_next = 0;
static char s_album_browse_item_id[160] = {0};
static char s_album_browse_item_title[80] = {0};
static char s_album_browse_item_artist[56] = {0};
static int64_t s_album_pending_since_us = 0;
#define HA_ALBUM_REQ_TIMEOUT_US (10LL * 1000LL * 1000LL)
#define HA_INVENTORY_COOLDOWN_US (15LL * 1000LL * 1000LL)

/* Post-light-command settle refresh (see ha_request_lights_fresh /
 * ha_client_tick). A full get_states is the heaviest SDIO burst we can ask
 * for, so as a mere settle-confirmation it runs under stricter rules than an
 * ordinary inventory fetch: it waits out the Matter round-trip, coalesces any
 * commands issued meanwhile into ONE snapshot, keeps a hard floor between
 * cooldown-bypassing snapshots, demands the larger cover-fetch-class memory
 * reserve, and never runs at all while music is streaming (the optimistic row
 * state stands until the next natural refresh). */
#define LIGHT_SETTLE_DELAY_US        (700LL * 1000LL)
#define LIGHT_SETTLE_RETRY_US        (1500LL * 1000LL)
#define LIGHT_SETTLE_STREAM_RETRY_US (3000LL * 1000LL)
#define FORCED_INVENTORY_MIN_US      (5LL * 1000LL * 1000LL)
#define LIGHT_SETTLE_MIN_FREE        (64U * 1024U)
#define LIGHT_SETTLE_MIN_LARGEST     (32U * 1024U)

/* Delay before restarting the WebSocket after a clean server-side close.
 * Long enough not to hammer a Home Assistant that is still booting, short
 * enough that recovery feels automatic. */
#define WS_RESTART_DELAY_US          (5LL * 1000LL * 1000LL)
/* Connected-but-unauthenticated watchdog: a booting HA has been seen taking
 * 10 s just to send auth_required and then dropping the socket without any
 * event (hardware log 2026-07-12). If the handshake hasn't completed in this
 * long, force a fresh connect rather than trusting the half-open socket. */
#define WS_AUTH_STALL_US             (30LL * 1000LL * 1000LL)
/* Idle heartbeat. With playback stopped there are no state events, so a
 * half-open socket (router NAT drop, HA restart without a clean FIN) is
 * indistinguishable from a quiet-but-healthy link. After HA_PING_IDLE_US with
 * no inbound frame, send an application-level {"type":"ping"}; if nothing (the
 * pong, or any other frame) arrives within HA_PONG_TIMEOUT_US, treat the link
 * as dead and reconnect. This is what recovers a controller that was left idle
 * while the network blipped, without waiting for the user to poke it. */
#define HA_PING_IDLE_US              (30LL * 1000LL * 1000LL)
#define HA_PONG_TIMEOUT_US           (10LL * 1000LL * 1000LL)

/* From p4_shared/audio_stream_bridge.h (root-private to p4_shared; declared
 * here to match main.c's cross-component externs). True while the ES8311 is
 * streaming music -- installation-wide snapshots must hold off then. */
bool audio_stream_is_active(void);

/* `err` NULL = success; else a short human reason shown on the add screen. */
void ui_set_album_candidates(const void *list, int count, const char *err);
void ui_set_queue(const void *list, int count, const char *err);
void ui_show_toast(const char *msg, uint32_t ms_dur);
void ui_set_devices_error(const char *message);
void ui_set_lights_ext(const ui_light_t *list, const int *hues, const int *sats,
                       const int *temps, int count);

static bool media_player_is_unavailable(const char *obj);
static bool media_player_is_renderer(const char *eid, const char *attrs);
static bool json_slice_contains(const char *start, const char *needle);

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
    return p4_json_copy_string(p, out, out_len);
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
static bool request_inventory_refresh(const char *reason, bool force_cooldown);

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

static void diag_request_sent(int id)
{
    ha_diag_request_t *slot = &s_diag_requests[(unsigned)id % HA_DIAG_REQUEST_SLOTS];
    slot->id = id;
    slot->sent_us = esp_timer_get_time();
}

static void diag_request_result(int id, bool failed)
{
    ha_diag_request_t *slot = &s_diag_requests[(unsigned)id % HA_DIAG_REQUEST_SLOTS];
    if (slot->id != id || slot->sent_us == 0) return;
    int64_t elapsed_us = esp_timer_get_time() - slot->sent_us;
    ESP_LOGI(TAG, "diag_ha_command id=%d result=%s rtt_ms=%lld",
             id, failed ? "failed" : "ok", elapsed_us / 1000);
    slot->id = 0;
    slot->sent_us = 0;
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
            const char *img = json_array_first_obj(json_obj_get(item, "images"));
            if (img && (v = json_obj_get(img, "url")))
                json_copy_string(v, a->image_url, sizeof(a->image_url));
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

static bool spotify_search_tracks(const char *query, ha_album_candidate_t *out,
                                  int max, int *count, char *err_out,
                                  size_t err_len)
{
    if (err_out && err_len) err_out[0] = '\0';
    if (count) *count = 0;
    if (!query || !query[0] || !out || max <= 0) return false;
    if (max > SPOTIFY_SEARCH_LIMIT_MAX) max = SPOTIFY_SEARCH_LIMIT_MAX;
    if (!spotify_client_token(err_out, err_len)) return false;

    char trimmed[80], q[260], url[384];
    if (!search_query_trim_copy(trimmed, sizeof(trimmed), query)) {
        if (err_out) snprintf(err_out, err_len, "Enter a song or artist");
        return false;
    }
    if (!url_encode(q, sizeof(q), trimmed)) {
        if (err_out) snprintf(err_out, err_len, "Search text is too long");
        return false;
    }
    snprintf(url, sizeof(url),
             "https://api.spotify.com/v1/search?q=%s&type=track&limit=%d", q, max);

    spotify_resp_buf_t resp = {0};
    esp_http_client_config_t cfg = {
        .url = url, .method = HTTP_METHOD_GET,
        .event_handler = spotify_http_event_handler, .user_data = &resp,
        .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 7000,
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
        const char *tracks = json_obj_get(resp.data, "tracks");
        const char *item = json_array_first_obj(json_obj_get(tracks, "items"));
        while (item && n < max) {
            ha_album_candidate_t *track = &out[n];
            memset(track, 0, sizeof(*track));
            const char *v;
            if ((v = json_obj_get(item, "name")))
                json_copy_string(v, track->title, sizeof(track->title));
            if ((v = json_obj_get(item, "uri")))
                json_copy_string(v, track->uri, sizeof(track->uri));
            const char *artist = json_array_first_obj(json_obj_get(item, "artists"));
            if (artist && (v = json_obj_get(artist, "name")))
                json_copy_string(v, track->artist, sizeof(track->artist));
            if (!track->artist[0]) snprintf(track->artist, sizeof(track->artist), "Unknown artist");
            if (track->title[0] && strncmp(track->uri, "spotify:track:", 14) == 0) n++;
            item = json_array_next_obj(item);
        }
        ok = true;
    } else if (status == 401) {
        s_sp_access_token[0] = '\0';
        s_sp_token_expiry_us = 0;
        if (err_out) snprintf(err_out, err_len, "Spotify rejected the search token");
    } else {
        char api_msg[96] = {0};
        spotify_response_error_message(resp.data, api_msg, sizeof(api_msg));
        if (err_out) snprintf(err_out, err_len, "%s",
                              api_msg[0] ? api_msg : "Spotify song search failed");
    }
    free(resp.data);
    if (count) *count = n;
    return ok;
}

static bool spotify_album_id_from_uri(const char *uri, char *out, size_t out_len)
{
    if (!out || out_len == 0) return false;
    out[0] = '\0';
    if (!uri) return false;

    const char *p = NULL;
    if (strncmp(uri, "spotify:album:", 14) == 0) {
        p = uri + 14;
    } else if (strncmp(uri, "spotify://album/", 16) == 0) {
        p = uri + 16;
    } else {
        const char *web = strstr(uri, "open.spotify.com/album/");
        if (web) p = web + strlen("open.spotify.com/album/");
    }
    if (!p || !*p) return false;

    size_t i = 0;
    while (p[i] && i + 1 < out_len) {
        char c = p[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9');
        if (!ok) break;
        out[i] = c;
        i++;
    }
    out[i] = '\0';
    return i > 0;
}

bool ha_spotify_album_image_url(const char *spotify_uri, char *out, size_t out_len)
{
    if (!out || out_len == 0) return false;
    out[0] = '\0';

    char id[64];
    if (!spotify_album_id_from_uri(spotify_uri, id, sizeof(id))) return false;

    char err_msg[96];
    if (!spotify_client_token(err_msg, sizeof(err_msg))) {
        ESP_LOGW(TAG, "spotify album art repair token failed: %s",
                 err_msg[0] ? err_msg : "-");
        return false;
    }

    char url[160];
    snprintf(url, sizeof(url), "https://api.spotify.com/v1/albums/%s", id);

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
    if (!client) return false;

    char bearer[320];
    snprintf(bearer, sizeof(bearer), "Bearer %s", s_sp_access_token);
    esp_http_client_set_header(client, "Authorization", bearer);

    esp_err_t herr = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    bool ok = false;
    if (herr == ESP_OK && status == 200 && resp.data) {
        const char *img = json_array_first_obj(json_obj_get(resp.data, "images"));
        if (img && json_obj_get_str(img, "url", out, out_len)) ok = true;
    } else if (status == 401) {
        ESP_LOGW(TAG, "spotify album art lookup got 401, invalidating search token");
        s_sp_access_token[0] = '\0';
        s_sp_token_expiry_us = 0;
    } else {
        char api_msg[96] = {0};
        spotify_response_error_message(resp.data, api_msg, sizeof(api_msg));
        ESP_LOGW(TAG, "spotify album art lookup failed (err=%d status=%d msg=%s)",
                 (int)herr, status, api_msg[0] ? api_msg : "-");
    }

    free(resp.data);
    if (ok) ESP_LOGI(TAG, "spotify album art URL found for %.32s", spotify_uri);
    return ok;
}

static void send_auth(void)
{
    char buf[320];
    snprintf(buf, sizeof(buf), "{\"type\":\"auth\",\"access_token\":\"%s\"}",
             s_token ? s_token : "");
    ws_send(buf);
}

/* The initial get_states snapshot can be large. Do not overlap it with a
 * trigger subscription, and especially do not start a second snapshot while
 * resolving a stale configured entity. ESP-Hosted has a small internal RX
 * pool, so the previous startup burst could assert in sdio_rx_get_buffer. */
static bool request_current_state(void)
{
    char buf[64];
    s_states_req_id = s_msg_id++;
    snprintf(buf, sizeof(buf), "{\"id\":%d,\"type\":\"get_states\"}", s_states_req_id);
    if (ws_send(buf)) return true;
    s_states_req_id = 0;
    return false;
}

static bool subscribe_active_entity(void)
{
    char buf[256];
    /* Push only future state changes after the snapshot has settled. */
    s_sub_id = s_msg_id++;
    snprintf(buf, sizeof(buf),
             "{\"id\":%d,\"type\":\"subscribe_trigger\","
             "\"trigger\":{\"platform\":\"state\",\"entity_id\":\"%s\"}}",
             s_sub_id, s_entity ? s_entity : "");
    if (ws_send(buf)) return true;
    s_sub_id = 0;
    return false;
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
    bool sent = ws_send(buf);
    if (sent) diag_request_sent(id);
    return sent;
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
    /* The HA Spotify integration's account entity is not a Music Assistant
     * player, so music_assistant.play_media would be rejected there. It does
     * accept a plain media_player.play_media with the raw spotify: URI as a
     * context id -- playback starts on whichever Connect device is active. */
    if (s_spotify_entity[0] && strcmp(s_entity, s_spotify_entity) == 0) {
        char data[200];
        snprintf(data, sizeof(data),
                 "\"media_content_id\":\"%s\",\"media_content_type\":\"album\"",
                 spotify_uri ? spotify_uri : "");
        return call_service("media_player", "play_media", data);
    }

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

static bool transfer_media_id(char *out, size_t out_len, transfer_target_t target)
{
    const char *id = s_transfer_media_id;
    if (!id[0]) return false;
    if (target == TRANSFER_TARGET_SPOTIFY) {
        if (strncmp(id, "spotify:track:", 14) == 0) {
            snprintf(out, out_len, "%s", id);
            return true;
        }
        if (strncmp(id, "spotify://track/", 16) == 0) {
            snprintf(out, out_len, "spotify:track:%s", id + 16);
            return true;
        }

        /* Music Assistant can expose a library:// or provider-specific media
         * ID even when the track came from Spotify. Resolve that ID from the
         * visible title and artist before the Spotify Connect handoff. */
        char query[80];
        snprintf(query, sizeof(query), "%.45s %.28s",
                 s_transfer_title, s_transfer_artist);
        ha_album_candidate_t *matches = heap_caps_calloc(
            3, sizeof(*matches), MALLOC_CAP_SPIRAM);
        if (!matches) return false;
        int count = 0;
        char err[128] = {0};
        bool ok = spotify_search_tracks(query, matches, 3, &count,
                                        err, sizeof(err));
        int best = -1;
        int best_score = -1;
        for (int i = 0; ok && i < count; i++) {
            int score = 0;
            if (ascii_contains_ci(matches[i].title, s_transfer_title) ||
                ascii_contains_ci(s_transfer_title, matches[i].title)) score += 2;
            if (ascii_contains_ci(matches[i].artist, s_transfer_artist) ||
                ascii_contains_ci(s_transfer_artist, matches[i].artist)) score += 1;
            if (score > best_score) {
                best = i;
                best_score = score;
            }
        }
        if (best >= 0 && best_score >= 2) {
            snprintf(out, out_len, "%s", matches[best].uri);
            ESP_LOGI(TAG, "output transfer resolved Spotify track: %s -- %s -> %s",
                     s_transfer_artist, s_transfer_title, out);
        } else {
            ESP_LOGW(TAG, "output transfer Spotify lookup failed: query='%s' matches=%d reason=%s",
                     query, count, err[0] ? err : "no matching title");
        }
        heap_caps_free(matches);
        return best >= 0 && best_score >= 2;
    }
    if (target == TRANSFER_TARGET_MA && strncmp(id, "spotify:track:", 14) == 0) {
        snprintf(out, out_len, "spotify://track/%s", id + 14);
        return true;
    }
    snprintf(out, out_len, "%s", id);
    return true;
}

static void clear_pending_transfer(void)
{
    s_transfer_target = TRANSFER_TARGET_NONE;
    s_transfer_due_us = 0;
    s_transfer_seek_pending = false;
    s_transfer_entity[0] = '\0';
    s_transfer_media_id[0] = '\0';
    s_transfer_media_type[0] = '\0';
    s_transfer_title[0] = '\0';
    s_transfer_artist[0] = '\0';
    s_transfer_position_ms = 0;
}

static void run_pending_transfer(void)
{
    if (s_transfer_target == TRANSFER_TARGET_NONE || !s_transfer_entity[0]) return;

    if (s_transfer_seek_pending) {
        char data[48];
        snprintf(data, sizeof(data), "\"seek_position\":%u",
                 (unsigned)(s_transfer_position_ms / 1000));
        call_service_entity("media_player", "media_seek", s_transfer_entity, data);
        ESP_LOGI(TAG, "output transfer seek -> %s at %u ms",
                 s_transfer_entity, (unsigned)s_transfer_position_ms);
        clear_pending_transfer();
        return;
    }

    char media_id[192];
    if (!transfer_media_id(media_id, sizeof(media_id), s_transfer_target)) {
        ui_show_toast("Output changed, but this track cannot transfer to Spotify", 3500);
        clear_pending_transfer();
        return;
    }

    char data[300];
    bool sent;
    if (s_transfer_target == TRANSFER_TARGET_MA) {
        snprintf(data, sizeof(data),
                 "\"media_id\":\"%s\",\"media_type\":\"track\"", media_id);
        sent = call_service_entity("music_assistant", "play_media",
                                   s_transfer_entity, data);
    } else {
        const char *type = s_transfer_media_type[0] ? s_transfer_media_type : "track";
        snprintf(data, sizeof(data),
                 "\"media_content_id\":\"%s\",\"media_content_type\":\"%s\"",
                 media_id, type);
        sent = call_service_entity("media_player", "play_media",
                                   s_transfer_entity, data);
    }
    if (!sent) {
        ui_show_toast("Output transfer could not start the track", 3500);
        clear_pending_transfer();
        return;
    }

    ESP_LOGI(TAG, "output transfer play -> %s", s_transfer_entity);
    if (s_transfer_position_ms >= 1500) {
        s_transfer_seek_pending = true;
        s_transfer_due_us = esp_timer_get_time() + 1000LL * 1000LL;
    } else {
        clear_pending_transfer();
    }
}

static bool queue_available(void)
{
    if (!s_authenticated) {
        ui_set_queue(NULL, 0, "Home Assistant is not connected");
        return false;
    }
    if (!s_active_is_ma) {
        ui_set_queue(NULL, 0, "Select a Music Assistant player in Output");
        return false;
    }
    return true;
}

void ha_request_queue(void)
{
    if (!queue_available()) return;
    char buf[320];
    s_queue_req_id = s_msg_id++;
    snprintf(buf, sizeof(buf),
             "{\"id\":%d,\"type\":\"call_service\",\"domain\":\"music_assistant\","
             "\"service\":\"get_queue\",\"target\":{\"entity_id\":\"%s\"},"
             "\"return_response\":true}", s_queue_req_id, s_entity ? s_entity : "");
    if (!ws_send(buf)) {
        s_queue_req_id = 0;
        ui_set_queue(NULL, 0, "Home Assistant is not connected");
    }
}

bool ha_queue_add(const char *spotify_uri, bool play_next)
{
    if (!queue_available() || !spotify_uri || !spotify_uri[0]) return false;
    const char *c1 = strchr(spotify_uri, ':');
    const char *c2 = c1 ? strchr(c1 + 1, ':') : NULL;
    char media_id[160];
    char media_type[20] = "track";
    if (c1 && c2) {
        snprintf(media_type, sizeof(media_type), "%.*s", (int)(c2 - c1 - 1), c1 + 1);
        snprintf(media_id, sizeof(media_id), "spotify://%s/%s", media_type, c2 + 1);
    } else {
        snprintf(media_id, sizeof(media_id), "%s", spotify_uri);
    }
    char data[280];
    snprintf(data, sizeof(data),
             "\"media_id\":\"%s\",\"media_type\":\"%s\",\"enqueue\":\"%s\"",
             media_id, media_type, play_next ? "next" : "add");
    bool ok = call_service("music_assistant", "play_media", data);
    if (ok) ui_show_toast(play_next ? "Added next" : "Added to queue", 1600);
    return ok;
}

bool ha_queue_clear(void)
{
    if (!queue_available()) return false;
    bool ok = call_service("media_player", "clear_playlist", NULL);
    if (ok) ui_show_toast("Queue cleared", 1600);
    return ok;
}

void ha_search_queue_tracks(const char *query)
{
    static ha_album_candidate_t tracks[HA_ALBUM_CANDIDATE_MAX];
    char err[160] = {0};
    int n = 0;
    if (!queue_available()) {
        ui_set_album_candidates(NULL, 0, "Select a Music Assistant player in Output");
        return;
    }
    bool ok = spotify_search_tracks(query, tracks, HA_ALBUM_CANDIDATE_MAX,
                                    &n, err, sizeof(err));
    ui_set_album_candidates(ok ? tracks : NULL, ok ? n : 0,
                            ok ? (n ? NULL : "No Spotify songs matched")
                               : (err[0] ? err : "Spotify song search failed"));
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
    if (sat_pct < 0) {
        int kelvin = hue_deg;
        if (kelvin < 1500) kelvin = 1500;
        if (kelvin > 9000) kelvin = 9000;
        char temp_data[40];
        snprintf(temp_data, sizeof(temp_data), "\"color_temp_kelvin\":%d", kelvin);
        return call_service_entity("light", "turn_on", entity_id, temp_data);
    }

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
    /* Monotonic playback-position reference -- see the media_position comment
     * below. Function-static: apply_state_object only ever runs for the active
     * entity, and a transfer changes the title (track_changed -> re-base). */
    static uint32_t s_media_pos_raw = 0;
    static int64_t  s_media_pos_ref_us = 0;
    if (!st || *st != '{') return;
    int64_t event_us = esp_timer_get_time();
    char state_updated[40] = {0};
    json_obj_get_str(st, "last_updated", state_updated, sizeof(state_updated));

    char state[24] = {0};
    json_obj_get_str(st, "state", state, sizeof(state));
    s_track.is_playing = (strcmp(state, "playing") == 0);
    bool track_changed = false;
    bool art_changed = false;

    const char *attrs = json_obj_get(st, "attributes");
    if (attrs && *attrs == '{') {
        s_active_is_ma = json_slice_contains(attrs, "\"mass_player_id\"");
        char prev_title[sizeof s_track.title];
        snprintf(prev_title, sizeof prev_title, "%s", s_track.title);
        json_obj_get_str(attrs, "media_title",      s_track.title,  sizeof(s_track.title));
        json_obj_get_str(attrs, "media_artist",     s_track.artist, sizeof(s_track.artist));
        json_obj_get_str(attrs, "media_album_name", s_track.album,  sizeof(s_track.album));
        json_obj_get_str(attrs, "friendly_name",    s_track.device_name, sizeof(s_track.device_name));
        track_changed = (strcmp(prev_title, s_track.title) != 0);
        if (track_changed) s_diag_track_seq++;

        /* HA's media_position is a snapshot sampled at media_position_updated_at,
         * NOT a live counter. Re-basing progress to it on every poll snaps the
         * bar backwards ~one poll interval each cycle (very visible on Music
         * Assistant / local Sendspin playback, where the bar looked frozen).
         * Keep a monotonic reference and only re-base when the position really
         * moves (seek / track change / periodic refresh); the value pushed to
         * the UI below is reference + elapsed, so the bar advances smoothly like
         * the direct-Spotify build's live progress. */
        double pos = 0, dur = 0;
        if (json_obj_get_double(attrs, "media_duration", &dur))
            s_track.duration_ms = (uint32_t)(dur * 1000.0);
        if (json_obj_get_double(attrs, "media_position", &pos)) {
            uint32_t raw = (uint32_t)(pos * 1000.0);
            uint32_t drift = raw > s_media_pos_raw ? raw - s_media_pos_raw
                                                   : s_media_pos_raw - raw;
            if (track_changed || drift > 1500 || s_media_pos_ref_us == 0) {
                s_media_pos_raw    = raw;
                s_media_pos_ref_us = esp_timer_get_time();
            }
        }

        char art[256];
        if (json_obj_get_str(attrs, "entity_picture", art, sizeof(art)) &&
            strcmp(art, s_track.album_art_url) != 0) {
            strncpy(s_track.album_art_url, art, sizeof(s_track.album_art_url) - 1);
            s_track.album_art_url[sizeof(s_track.album_art_url) - 1] = '\0';
            taskENTER_CRITICAL(&s_art_mux);
            strncpy(s_pending_art, art, sizeof(s_pending_art) - 1);
            s_pending_art[sizeof(s_pending_art) - 1] = '\0';
            s_pending_art_seq = s_diag_track_seq;
            s_pending_art_event_us = event_us;
            s_art_pending = true;
            taskEXIT_CRITICAL(&s_art_mux);
            art_changed = true;
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
        char content_id[160] = {0};
        char content_type[32] = {0};
        json_obj_get_str(attrs, "media_content_id",   content_id,   sizeof(content_id));
        json_obj_get_str(attrs, "media_content_type", content_type, sizeof(content_type));
        snprintf(s_media_content_id, sizeof(s_media_content_id), "%s", content_id);
        snprintf(s_media_content_type, sizeof(s_media_content_type), "%s", content_type);
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

    /* Resolve the live position from the monotonic reference (see the
     * media_position comment). Playing: reference + elapsed. Paused: hold the
     * reference and reset the clock so a long pause never inflates the position
     * on resume. */
    if (s_track.is_playing && s_media_pos_ref_us) {
        int64_t elapsed_ms = (esp_timer_get_time() - s_media_pos_ref_us) / 1000;
        if (elapsed_ms < 0) elapsed_ms = 0;
        uint32_t live = s_media_pos_raw + (uint32_t)elapsed_ms;
        if (s_track.duration_ms && live > s_track.duration_ms)
            live = s_track.duration_ms;
        s_track.progress_ms = live;
    } else if (!s_track.is_playing) {
        s_track.progress_ms = s_media_pos_raw;
        s_media_pos_ref_us  = esp_timer_get_time();
    }

    ESP_LOGI(TAG, "state: %s -- %s [%s]", s_track.artist, s_track.title, state);
    int64_t ui_started_us = esp_timer_get_time();
    ui_set_track_info(&s_track);
    int64_t ui_finished_us = esp_timer_get_time();
    if (track_changed) {
        ESP_LOGI(TAG,
                 "diag_meta seq=%u updated=%s parse_ms=%lld ui_call_ms=%lld art_changed=%d",
                 (unsigned)s_diag_track_seq,
                 state_updated[0] ? state_updated : "unknown",
                 (ui_started_us - event_us) / 1000,
                 (ui_finished_us - ui_started_us) / 1000,
                 art_changed ? 1 : 0);
    }
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
static bool s_dev_is_ma[MAX_DEVICES];
static int  s_dev_count = 0;
static ui_device_t s_devices[MAX_DEVICES];
static bool s_devices_cache_valid = false;

static ui_light_t s_lights[MAX_LIGHTS];
static int s_light_hues[MAX_LIGHTS];
static int s_light_sats[MAX_LIGHTS];
static int s_light_temps[MAX_LIGHTS];
static int s_light_count = 0;
static bool s_lights_cache_valid = false;

/* Bounded substring search inside ONE JSON value. json_obj_get returns a
 * pointer into the whole websocket message, so a plain strstr from there could
 * match text belonging to a LATER entity in the array. */
static bool json_slice_contains(const char *start, const char *needle)
{
    if (!start) return false;
    const char *end = json_skip_value(start);
    size_t nlen = strlen(needle);
    for (const char *p = start; p + nlen <= end; p++)
        if (memcmp(p, needle, nlen) == 0) return true;
    return false;
}

/* Copy up to `max` string elements out of a JSON array of strings. */
static int json_array_get_strings(const char *arr, char out[][40], int max)
{
    int n = 0;
    if (!arr || *arr != '[') return 0;
    const char *p = arr + 1;
    while (*p && *p != ']' && n < max) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',') p++;
        if (*p == ']' || *p == '\0') break;
        if (*p == '"' && json_copy_string(p, out[n], 40)) n++;
        p = json_skip_value(p);
    }
    return n;
}

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

static bool media_player_is_spotify_account(const char *eid, const char *attrs)
{
    if (eid && (strncmp(eid, "media_player.spotify", 20) == 0 ||
                ascii_contains_ci(eid + 13, "spotify"))) return true;
    if (!attrs || json_slice_contains(attrs, "\"mass_player_id\"")) return false;

    /* The HA Spotify account player exposes only SELECT_SOURCE (2048) and a
     * source_list. This capability signature survives user entity renames;
     * ordinary MA/Sonos/TV players expose a broader feature mask. */
    const char *sources = json_obj_get(attrs, "source_list");
    double supported = 0.0;
    return sources && *sources == '[' &&
           json_obj_get_double(attrs, "supported_features", &supported) &&
           (int)supported == 2048;
}

static void media_player_state(const char *obj, char *out, size_t out_len)
{
    if (!json_obj_get_str(obj, "state", out, out_len))
        snprintf(out, out_len, "unknown");
}

/* Collapse HA integration aliases onto the Music Assistant player that users
 * actually control. For example, "Living Room - Sonos Play:5 Media Renderer"
 * and "Living Room Speaker" both normalise to "living room". */
static void normalise_device_name(const char *src, char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    size_t n = 0;
    bool previous_space = true;
    for (; src && *src && n + 1 < out_len; src++) {
        char c = *src;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c == ' ' || c == '\t' || c == '_' || c == '-') {
            if (!previous_space) out[n++] = ' ';
            previous_space = true;
        } else {
            out[n++] = c;
            previous_space = false;
        }
    }
    while (n > 0 && out[n - 1] == ' ') n--;
    out[n] = '\0';

    char *model = strstr(out, " sonos ");
    if (model) *model = '\0';
    char *renderer = strstr(out, " media renderer");
    if (renderer) *renderer = '\0';
    size_t len = strlen(out);
    if (len > 8 && strcmp(out + len - 8, " speaker") == 0)
        out[len - 8] = '\0';
    while (len > 0 && out[len - 1] == ' ') out[--len] = '\0';
}

static int device_duplicate_index(const ui_device_t *devs, int count, const char *name)
{
    char wanted[48];
    normalise_device_name(name, wanted, sizeof(wanted));
    if (!wanted[0]) return -1;
    for (int i = 0; i < count; i++) {
        /* The HA Spotify account entity often shares its friendly name with a
         * real speaker (both can be "Home Controller"). An account is not an
         * output alias -- if it were allowed to absorb one, the on-device
         * Music Assistant speaker vanished from the picker (hardware log
         * 2026-07-11). The UI renders Spotify rows in their own section, so
         * cross-section dedup is never wanted. */
        if (strncmp(devs[i].detail, "SPOTIFY", 7) == 0) continue;
        char existing[48];
        normalise_device_name(devs[i].name, existing, sizeof(existing));
        if (strcmp(wanted, existing) == 0) return i;
    }
    return -1;
}

/* Build the device list from a get_states result array: every media_player
 * entity becomes one row (name = friendly_name, detail = entity_id tail so
 * duplicate friendly names stay distinguishable). Pushed straight to the UI. */
static void build_device_list(const char *arr)
{
    ui_device_t *devs = s_devices;          /* cache owned by the WS task */
    int n = 0;
    int skipped_unavailable = 0;
    int skipped_renderer = 0;
    int skipped_duplicate = 0;
    memset(s_dev_is_ma, 0, sizeof(s_dev_is_ma));
    s_spotify_source_count = 0;   /* re-discovered below on every build */
    s_spotify_entity[0] = '\0';
    s_spotify_source_now[0] = '\0';
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
                const char *attrs = json_obj_get(obj, "attributes");
                char fname[64] = {0};
                char state[24] = {0};
                if (attrs) json_obj_get_str(attrs, "friendly_name", fname, sizeof(fname));
                media_player_state(obj, state, sizeof(state));
                bool renderer = media_player_is_renderer(eid, attrs);
                /* Provenance tags so the list makes clear WHAT each row is:
                 * a Music Assistant player, the Spotify account entity, or a
                 * plain HA media_player. Label-only -- routing decisions key
                 * on the entity, not these. */
                bool is_ma = attrs && json_slice_contains(attrs, "\"mass_player_id\"");
                bool is_spotify_acct = media_player_is_spotify_account(eid, attrs);
                if (is_spotify_acct && attrs) {
                    snprintf(s_spotify_entity, sizeof(s_spotify_entity), "%s", eid);
                    s_spotify_source_count = json_array_get_strings(
                        json_obj_get(attrs, "source_list"),
                        s_spotify_sources, MAX_SPOTIFY_SOURCES);
                    if (!json_obj_get_str(attrs, "source", s_spotify_source_now,
                                          sizeof(s_spotify_source_now)))
                        s_spotify_source_now[0] = '\0';
                    ESP_LOGI(TAG, "Spotify account: %s sources=%d active='%s'",
                             eid, s_spotify_source_count,
                             s_spotify_source_now[0] ? s_spotify_source_now : "-");
                }
                /* The Spotify account entity is a source SELECTOR, not an output:
                 * it rejects play_media / media_play_pause (supported_features=
                 * 2048, hardware log 2026-07-12), duplicates the "Home Controller"
                 * name, and could show red alongside an active Connect source.
                 * Do not render it -- its Connect devices are appended as their
                 * own rows below and it is followed internally via
                 * s_spotify_entity for select_source + state. */
                if (is_spotify_acct) {
                    p = json_skip_value(p);
                    continue;
                }
                /* Sonos/DLNA renderer entities are implementation detail rows.
                 * MA's player is the useful control surface, so do not expose
                 * a second alias with a model-heavy name. */
                if (renderer && !is_ma) {
                    ESP_LOGI(TAG, "device: %s renderer alias skipped", eid);
                    skipped_renderer++;
                    p = json_skip_value(p);
                    continue;
                }

                const char *name = fname[0] ? fname : eid + 13;
                int slot = device_duplicate_index(devs, n, name);
                if (slot >= 0 && (!is_ma || s_dev_is_ma[slot])) {
                    ESP_LOGI(TAG, "device: %s duplicate of %s skipped", eid, s_dev_ids[slot]);
                    skipped_duplicate++;
                    p = json_skip_value(p);
                    continue;
                }
                if (slot < 0) slot = n++;

                ui_device_t *d = &devs[slot];
                memset(d, 0, sizeof(*d));
                snprintf(s_dev_ids[slot], sizeof(s_dev_ids[slot]), "%s", eid);
                snprintf(d->id, sizeof(d->id), "%d", slot);   /* UI carries the list index */
                snprintf(d->name, sizeof(d->name), "%.39s", fname[0] ? fname : eid + 13);
                snprintf(d->detail, sizeof(d->detail), "%s %.7s",
                         is_ma ? "MUSIC ASSISTANT" : "HOME ASSISTANT", state);

                d->is_active = (strcmp(eid, s_entity) == 0);
                d->is_sonos  = false;   /* all HA entities switch via ui_request_transfer */
                s_dev_playable[slot] = is_ma || renderer;
                s_dev_is_ma[slot] = is_ma;
                ESP_LOGI(TAG, "device[%d]: %s name='%s' state=%s kind=%s",
                         slot, eid, d->name, state,
                         is_ma ? "ma" : renderer ? "renderer" : "other");
            }
            p = json_skip_value(p);
        }
    }

    /* Append the Spotify account's Connect devices (source_list) as their own
     * rows, so e.g. the phone is directly selectable. Never auto-picked. */
    for (int i = 0; i < s_spotify_source_count && n < MAX_DEVICES; i++) {
        /* This integration-created target accepts select_source but does not
         * identify a useful physical output in this installation. Keep it out
         * of the picker so it cannot be confused with the controller speaker. */
        if (strcmp(s_spotify_sources[i], "Home Assistant") == 0) {
            ESP_LOGI(TAG, "spotify source 'Home Assistant' hidden");
            continue;
        }
        /* Skip a Connect source that is really an output already listed as a
         * speaker -- e.g. this device's own "Home Controller" MA player. It is
         * controllable via that speaker row; the self-referential Connect row
         * was the confusing "two Home Controllers" duplicate. External targets
         * (phone, Echo) don't match any speaker, so they stay. */
        if (device_duplicate_index(devs, n, s_spotify_sources[i]) >= 0) {
            ESP_LOGI(TAG, "spotify source '%s' duplicates a speaker row -- skipped",
                     s_spotify_sources[i]);
            continue;
        }
        ui_device_t *d = &devs[n];
        memset(d, 0, sizeof(*d));
        snprintf(s_dev_ids[n], sizeof(s_dev_ids[n]), SPOTIFY_SRC_PREFIX "%.80s",
                 s_spotify_sources[i]);
        snprintf(d->id, sizeof(d->id), "%d", n);
        snprintf(d->name, sizeof(d->name), "%.39s", s_spotify_sources[i]);
        snprintf(d->detail, sizeof(d->detail), "SPOTIFY CONNECT");
        d->is_active = s_spotify_entity[0] &&
                       strcmp(s_entity, s_spotify_entity) == 0 &&
                       strcmp(s_spotify_sources[i], s_spotify_source_now) == 0;
        d->is_sonos = false;
        s_dev_playable[n] = false;
        s_dev_is_ma[n] = false;
        ESP_LOGI(TAG, "device[%d]: spotify connect source '%s'%s", n,
                 s_spotify_sources[i], d->is_active ? " (active)" : "");
        n++;
    }

    s_dev_count = n;
    s_devices_cache_valid = true;
    ESP_LOGI(TAG, "devices: %d available (%d unavailable, %d renderer, %d duplicate skipped)",
             n, skipped_unavailable, skipped_renderer, skipped_duplicate);
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
    ui_light_t *lights = s_lights;          /* cache owned by the WS task */
    int *hues = s_light_hues;
    int *sats = s_light_sats;
    int *temps = s_light_temps;
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
                temps[n] = -1;
                const char *modes = attrs ? json_obj_get(attrs, "supported_color_modes") : NULL;
                bool supports_level = modes && !ascii_contains_ci(modes, "onoff");
                bool supports_colour = modes &&
                    (ascii_contains_ci(modes, "hs") ||
                     ascii_contains_ci(modes, "xy") ||
                     ascii_contains_ci(modes, "rgb"));
                bool supports_temp = modes && ascii_contains_ci(modes, "color_temp");
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
                    hues[n] = HA_LIGHT_VALUE_UNKNOWN_SUPPORTED;
                    sats[n] = 66;
                }

                double kelvin = 0.0;
                if (attrs && json_obj_get_double(attrs, "color_temp_kelvin", &kelvin) &&
                    kelvin > 0.0) {
                    temps[n] = (int)(kelvin + 0.5);
                } else {
                    double mired = 0.0;
                    if (attrs && json_obj_get_double(attrs, "color_temp", &mired) &&
                        mired > 0.0) {
                        temps[n] = (int)(1000000.0 / mired + 0.5);
                    } else if (supports_temp) {
                        temps[n] = HA_LIGHT_VALUE_UNKNOWN_SUPPORTED;
                    }
                }

                ESP_LOGI(TAG, "light: %s state=%s brightness=%d%% hue=%d sat=%d temp=%d",
                         eid, state[0] ? state : "(missing)", l->brightness_pct,
                          hues[n], sats[n], temps[n]);
                n++;
            }
            p = json_skip_value(p);
        }
    }
    ESP_LOGI(TAG, "lights: %d available light entities (%d unavailable skipped)",
             n, skipped_unavailable);
    s_light_count = n;
    s_lights_cache_valid = true;
    ui_set_lights_ext(lights, hues, sats, temps, n);
}

static const char *json_first_object_value(const char *obj)
{
    if (!obj || *obj != '{') return NULL;
    const char *p = obj + 1;
    while (*p && *p != '}') {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',') p++;
        if (*p != '"') return NULL;
        p = json_skip_string(p);
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (*p != ':') return NULL;
        p++;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        return (*p == '{') ? p : NULL;
    }
    return NULL;
}

static void handle_queue_result(const char *result)
{
    static ha_queue_item_t items[HA_QUEUE_ITEM_MAX];
    const char *response = result ? json_obj_get(result, "response") : NULL;
    const char *queue = response ? json_obj_get(response, s_entity ? s_entity : "") : NULL;
    if (!queue) queue = json_first_object_value(response);
    if (!queue) {
        ui_set_queue(NULL, 0, "Music Assistant returned no queue");
        return;
    }

    int current_index = -1;
    const char *cur = json_obj_get(queue, "current_index");
    if (cur) current_index = atoi(cur);
    const char *entry = json_array_first_obj(json_obj_get(queue, "items"));
    int n = 0, index = 0;
    while (entry && n < HA_QUEUE_ITEM_MAX) {
        ha_queue_item_t *out = &items[n];
        memset(out, 0, sizeof(*out));
        const char *media = json_obj_get(entry, "media_item");
        const char *source = (media && *media == '{') ? media : entry;
        json_obj_get_str(source, "name", out->title, sizeof(out->title));
        if (!out->title[0]) json_obj_get_str(entry, "name", out->title, sizeof(out->title));
        json_obj_get_str(source, "uri", out->uri, sizeof(out->uri));
        if (!out->uri[0]) json_obj_get_str(entry, "uri", out->uri, sizeof(out->uri));
        const char *artists = json_obj_get(source, "artists");
        const char *artist = json_array_first_obj(artists);
        if (artist) json_obj_get_str(artist, "name", out->artist, sizeof(out->artist));
        if (!out->artist[0]) json_obj_get_str(source, "artist", out->artist, sizeof(out->artist));
        if (!out->artist[0]) snprintf(out->artist, sizeof(out->artist), "Music Assistant");
        if (!out->title[0]) snprintf(out->title, sizeof(out->title), "Untitled item");
        out->is_current = (index == current_index);
        n++;
        index++;
        entry = json_array_next_obj(entry);
    }
    ui_set_queue(items, n, NULL);
}

static void handle_message(const char *msg)
{
    char type[24] = {0};
    if (!json_obj_get_str(msg, "type", type, sizeof(type))) return;

    if (strcmp(type, "auth_required") == 0) {
        ESP_LOGI(TAG, "authentication requested");
        send_auth();
    } else if (strcmp(type, "auth_ok") == 0) {
        s_authenticated = true;
        s_auth_rejected = false;
        s_initial_state_received = false;
        s_sub_id = 0;
        ESP_LOGI(TAG, "authenticated");
        if (!request_current_state())
            ESP_LOGW(TAG, "initial state request could not be sent");
    } else if (strcmp(type, "auth_invalid") == 0) {
        s_authenticated = false;
        s_initial_state_received = false;
        s_auth_rejected = true;
        /* Never fatal: HA rejects even a valid token while its auth
         * subsystem is still starting (seen booting controller + HA
         * together), then closes the socket. The CLOSED handler arms the
         * reconnect; arm it here too in case the server leaves us open. A
         * genuinely bad token just keeps cycling, and the Devices screen
         * points at Settings > SETUP. */
        ESP_LOGE(TAG, "auth_invalid -- retrying; check HA_TOKEN / Settings > SETUP if persistent");
        s_ws_restart_pending = true;
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
            char err_code[48] = {0};
            if (err) {
                json_obj_get_str(err, "code", err_code, sizeof err_code);
                json_obj_get_str(err, "message", err_msg, sizeof err_msg);
            }
            if (!err_msg[0])
                snprintf(err_msg, sizeof err_msg, "%s",
                         err_code[0] ? err_code : "unknown error");
            /* Log the code AND a bounded raw slice of the error object: some
             * Music Assistant failures (seek/volume on the local player) carry
             * the real reason in a field our scanner doesn't surface, and a bare
             * "unknown error" is not diagnosable. */
            ESP_LOGW(TAG, "result id=%d FAILED: code=%s msg=%s raw=%.140s",
                     id, err_code[0] ? err_code : "?", err_msg,
                     err ? err : "(none)");
        }
        if (id) diag_request_result(id, failed);
        if (id == s_states_req_id) {
            /* Clear before auto-selecting. ha_set_active_entity() can start a
             * new snapshot; clearing afterwards would otherwise lose its id. */
            s_states_req_id = 0;
            const char *arr = json_obj_get(msg, "result");
            if (!failed) {
                /* The one mandatory installation-wide snapshot seeds every
                 * inventory cache. Pages can render immediately from these
                 * typed caches instead of each issuing its own get_states. */
                build_device_list(arr);
                build_light_list(arr);
                collect_album_browse_entities(arr);
                s_inventory_last_us = esp_timer_get_time();
            }
            const char *st  = find_entity_in_array(arr);
            bool selected_other = false;
            if (st) apply_state_object(st);
            else {
                ESP_LOGW(TAG, "entity %s not found in get_states; open Devices and pick a valid media_player",
                         s_entity ? s_entity : "");
                selected_other = select_best_available_device();
                if (!selected_other)
                    ui_show_toast("HA media player not found - open Devices", 3500);
                else {
                    /* The same snapshot already contains the new active
                     * player. Use it instead of starting another full
                     * get_states transfer while the first is still settling. */
                    const char *selected = find_entity_in_array(arr);
                    if (selected) apply_state_object(selected);
                }
            }
            if (!failed) {
                s_initial_state_received = true;
                ESP_LOGI(TAG, "initial state received");
                s_subscribe_pending = true;
            }
        } else if (s_queue_req_id && id == s_queue_req_id) {
            s_queue_req_id = 0;
            if (failed) ui_set_queue(NULL, 0, err_msg[0] ? err_msg : "Music Assistant could not read the queue");
            else handle_queue_result(json_obj_get(msg, "result"));
        } else if (s_inventory_req_id && id == s_inventory_req_id) {
            s_inventory_req_id = 0;
            s_inventory_pending_since_us = 0;
            if (failed) {
                ui_set_devices_error(err_msg[0] ? err_msg : "Home Assistant could not refresh devices");
            } else {
                const char *arr = json_obj_get(msg, "result");
                build_device_list(arr);
                build_light_list(arr);
                if (!s_album_browse_req_id) collect_album_browse_entities(arr);
                s_inventory_last_us = esp_timer_get_time();
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
        } else if (failed && id) {
            /* A transport / seek / volume command HA rejected (not one of the
             * inventory/queue/browse requests, which surface their own errors).
             * Toast it so a control that silently did nothing is explained --
             * e.g. Music Assistant refusing a seek while the local player is
             * idle/stopped. */
            if (ascii_contains_ci(err_msg, "cannot control device volume")) {
                ui_show_toast("This output does not support remote volume", 3500);
            } else {
                char toast[160];
                snprintf(toast, sizeof toast, "Player rejected command: %s", err_msg);
                ui_show_toast(toast, 3000);
            }
        }
    }
}

/* ── WebSocket event handler (reassembly + dispatch) ─────────────────────── */

/* Session teardown shared by both disconnect flavours (abnormal drop and
 * clean server-side close). Runs on the WS task. */
static void ws_session_reset(void)
{
    s_authenticated = false;
    s_initial_state_received = false;
    s_states_req_id = 0;
    s_sub_id = 0;
    s_subscribe_pending = false;
    if (s_queue_req_id) {
        s_queue_req_id = 0;
        ui_set_queue(NULL, 0, "Home Assistant disconnected");
    }
    if (s_inventory_req_id) {
        s_inventory_req_id = 0;
        s_inventory_pending_since_us = 0;
        ui_set_devices_error("Home Assistant disconnected");
    }
    s_album_pending_since_us = 0;
    if (s_album_browse_req_id) {
        s_album_browse_req_id = 0;
        ui_set_album_candidates(NULL, 0, "Home Assistant disconnected");
    }
}

static void ws_event_handler(void *arg, esp_event_base_t base,
                             int32_t event_id, void *event_data)
{
    (void)arg; (void)base;
    esp_websocket_event_data_t *d = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "ws connected");
            s_last_rx_us = esp_timer_get_time();
            s_ping_await_us = 0;
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "ws disconnected");
            ws_session_reset();
            break;
        case WEBSOCKET_EVENT_CLOSED:
            /* A clean server-side close. HA sends one after auth_invalid --
             * including when a VALID token is rejected because HA's auth
             * subsystem is still starting up (hardware log 2026-07-12:
             * controller booted alongside HA, one auth_invalid, then the
             * connection sat dead until a power cycle). Unlike DISCONNECTED,
             * the client does NOT auto-reconnect from a clean close, so arm a
             * deferred restart; ha_client_tick() performs it off this task
             * (stopping the client from its own event handler deadlocks). */
            ESP_LOGW(TAG, "ws closed by server -- reconnect scheduled");
            ws_session_reset();
            s_ws_restart_pending = true;
            break;
        case WEBSOCKET_EVENT_DATA: {
            /* Any inbound frame (data, pong, keepalive) proves the link is live
             * -- feed the heartbeat before the op_code filter. */
            s_last_rx_us = esp_timer_get_time();
            s_ping_await_us = 0;
            /* op_code 1 = text, 0 = continuation. Ignore ping/pong/binary/close. */
            if (d->op_code != 1 && d->op_code != 0) break;
            if (d->payload_len <= 0) break;
            if (d->payload_len > RX_MAX_CAP) {
                if (d->payload_offset == 0) {
                    s_rx_dropping_oversize = true;
                    s_rx_drop_was_album = (s_album_browse_req_id != 0);
                    s_rx_drop_was_devices = (s_inventory_req_id != 0);
                    ESP_LOGW(TAG, "ws frame too large (%d B > %d B), dropping",
                             d->payload_len, RX_MAX_CAP);
                }
                if (d->payload_offset + d->data_len >= d->payload_len) {
                    bool was_album = s_rx_drop_was_album;
                    bool was_devices = s_rx_drop_was_devices;
                    s_rx_dropping_oversize = false;
                    s_rx_drop_was_album = false;
                    s_rx_drop_was_devices = false;
                    if (was_album && s_album_browse_req_id) {
                        ESP_LOGW(TAG, "album browse response too large, trying next source");
                        start_next_album_browse_entity();
                    }
                    if (was_devices && s_inventory_req_id) {
                        ESP_LOGW(TAG, "inventory response too large");
                        s_inventory_req_id = 0;
                        s_inventory_pending_since_us = 0;
                        ui_set_devices_error("Home Assistant inventory is too large");
                    }
                }
                break;
            }
            if (d->payload_offset == 0) {
                s_rx_dropping_oversize = false;
                s_rx_drop_was_album = false;
                s_rx_drop_was_devices = false;
            } else if (s_rx_dropping_oversize) {
                break;
            }

            if ((size_t)d->payload_len + 1 > s_rx_cap) {
                /* Keep small auth/control frames in normal RAM, matching the
                 * previously hardware-working path. With the HA build's
                 * SPIRAM malloc policy, large get_states responses still move
                 * to PSRAM automatically once they exceed 4 KB. */
                char *grown = realloc(s_rx, d->payload_len + 1);
                if (!grown) {
                    ESP_LOGW(TAG, "ws rx realloc failed for %d B", d->payload_len + 1);
                    if (s_album_browse_req_id) start_next_album_browse_entity();
                    if (s_inventory_req_id) {
                        s_inventory_req_id = 0;
                        s_inventory_pending_since_us = 0;
                        ui_set_devices_error("Not enough memory to refresh HA inventory");
                    }
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
    s_authenticated = false;
    s_initial_state_received = false;
    s_subscribe_pending = false;
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
    s_authenticated = false;
    s_initial_state_received = false;
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

bool ha_client_is_authenticated(void)
{
    return s_authenticated;
}

bool ha_client_is_ready(void)
{
    return s_authenticated && s_initial_state_received;
}

void ha_client_tick(void)
{
    /* Deferred WebSocket restart after a clean server-side close (see the
     * WEBSOCKET_EVENT_CLOSED / auth_invalid handlers): the client never
     * auto-reconnects from a clean close, and stop() cannot run on the WS
     * task itself. */
    if (s_ws_restart_pending && s_ws) {
        int64_t now = esp_timer_get_time();
        if (!s_ws_restart_due_us) {
            s_ws_restart_due_us = now + WS_RESTART_DELAY_US;
        } else if (now >= s_ws_restart_due_us) {
            s_ws_restart_pending = false;
            s_ws_restart_due_us = 0;
            ESP_LOGI(TAG, "restarting WebSocket (server closed / auth retry)");
            esp_websocket_client_stop(s_ws);
            esp_websocket_client_start(s_ws);
        }
    }

    /* Auth-handshake watchdog (see WS_AUTH_STALL_US). */
    static int64_t s_unauth_conn_since_us;
    if (s_ws && !s_authenticated && !s_ws_restart_pending &&
        esp_websocket_client_is_connected(s_ws)) {
        int64_t now = esp_timer_get_time();
        if (!s_unauth_conn_since_us) {
            s_unauth_conn_since_us = now;
        } else if (now - s_unauth_conn_since_us >= WS_AUTH_STALL_US) {
            s_unauth_conn_since_us = 0;
            ESP_LOGW(TAG, "auth handshake stalled -- restarting WebSocket");
            s_ws_restart_pending = true;
        }
    } else {
        s_unauth_conn_since_us = 0;
    }

    /* Idle heartbeat (see HA_PING_IDLE_US). Only while fully connected and
     * authenticated -- the restart/auth watchdogs above own the other states. */
    if (s_ws && s_authenticated && !s_ws_restart_pending &&
        esp_websocket_client_is_connected(s_ws)) {
        int64_t now = esp_timer_get_time();
        if (!s_last_rx_us) s_last_rx_us = now;
        if (s_ping_await_us) {
            if (now - s_ping_await_us >= HA_PONG_TIMEOUT_US) {
                ESP_LOGW(TAG, "no heartbeat pong in %llds -- link dead, reconnecting",
                         HA_PONG_TIMEOUT_US / 1000000LL);
                s_ping_await_us = 0;
                s_ws_restart_pending = true;
            }
        } else if (now - s_last_rx_us >= HA_PING_IDLE_US) {
            char ping[48];
            snprintf(ping, sizeof ping, "{\"id\":%d,\"type\":\"ping\"}", s_msg_id++);
            if (ws_send(ping)) s_ping_await_us = now;
        }
    } else {
        s_ping_await_us = 0;
    }

    /* Only the HA task sends the delayed trigger. The WebSocket callback first
     * finishes parsing the big startup response, so SDIO never has a new
     * outbound command competing with that receive burst. */
    if (s_subscribe_pending && s_authenticated && !s_states_req_id && !s_sub_id) {
        if (subscribe_active_entity()) s_subscribe_pending = false;
    }
    if (s_transfer_due_us && esp_timer_get_time() >= s_transfer_due_us) {
        s_transfer_due_us = 0;
        run_pending_transfer();
    }
    if (s_inventory_pending_since_us &&
        esp_timer_get_time() - s_inventory_pending_since_us >= HA_ALBUM_REQ_TIMEOUT_US) {
        ESP_LOGW(TAG, "inventory refresh timed out");
        s_inventory_req_id = 0;
        s_inventory_pending_since_us = 0;
        ui_set_devices_error("Home Assistant inventory refresh timed out");
    }
    if (s_lights_settle_due_us &&
        esp_timer_get_time() >= s_lights_settle_due_us) {
        int64_t now = esp_timer_get_time();
        if (audio_stream_is_active()) {
            /* Never compete with the audio feed for SDIO/internal SRAM just to
             * confirm a toggle the row already shows optimistically. Re-check
             * after playback; the deadline survives until a window opens. */
            s_lights_settle_due_us = now + LIGHT_SETTLE_STREAM_RETRY_US;
        } else if (s_forced_inventory_last_us &&
                   now - s_forced_inventory_last_us < FORCED_INVENTORY_MIN_US) {
            s_lights_settle_due_us = s_forced_inventory_last_us + FORCED_INVENTORY_MIN_US;
        } else if (!app_core_reliability_network_budget_ok(
                       "light settle refresh",
                       LIGHT_SETTLE_MIN_FREE, LIGHT_SETTLE_MIN_LARGEST)) {
            s_lights_settle_due_us = now + LIGHT_SETTLE_RETRY_US;
        } else if (request_inventory_refresh("light command settle", true)) {
            s_lights_settle_due_us = 0;
            s_forced_inventory_last_us = now;
        } else {
            /* Auth lost or a snapshot already in flight. An in-flight snapshot
             * may predate the light command, so keep the deadline armed. */
            s_lights_settle_due_us = now + LIGHT_SETTLE_RETRY_US;
        }
    }
    if (!s_album_pending_since_us) return;
    int64_t now = esp_timer_get_time();
    if (now - s_album_pending_since_us < HA_ALBUM_REQ_TIMEOUT_US) return;

    if (s_album_browse_req_id) {
        ESP_LOGW(TAG, "album browse timed out, trying next source");
        start_next_album_browse_entity();
    }
}

/* `force_cooldown` skips only the rate limit -- the transport-protecting
 * guards (auth, single-flight, memory budget) always apply. Used by the
 * post-command settle refresh, where a service call just changed real state
 * and waiting out the cooldown would leave the UI showing the old state. */
static bool request_inventory_refresh(const char *reason, bool force_cooldown)
{
    int64_t now = esp_timer_get_time();
    if (!s_authenticated || !s_initial_state_received || s_states_req_id ||
        s_inventory_req_id) return false;
    if (!force_cooldown &&
        s_inventory_last_us && now - s_inventory_last_us < HA_INVENTORY_COOLDOWN_US)
        return false;
    if (!app_core_reliability_network_budget_ok(
            reason ? reason : "HA inventory",
            APP_CORE_INTERNAL_NETWORK_MIN_FREE,
            APP_CORE_INTERNAL_NETWORK_MIN_LARGEST)) return false;

    char buf[64];
    s_inventory_req_id = s_msg_id++;
    s_inventory_pending_since_us = now;
    snprintf(buf, sizeof(buf), "{\"id\":%d,\"type\":\"get_states\"}", s_inventory_req_id);
    if (!ws_send(buf)) {
        s_inventory_req_id = 0;
        s_inventory_pending_since_us = 0;
        return false;
    }
    ESP_LOGI(TAG, "inventory refresh requested (%s)", reason ? reason : "periodic");
    return true;
}

void ha_request_devices(void)
{
    if (s_devices_cache_valid)
        ui_set_devices(s_devices, s_dev_count);
    if (!request_inventory_refresh("device inventory", false) && !s_devices_cache_valid) {
        /* Say WHY the screen is empty. "Not connected" was previously reported
         * for every refusal, which misled when the real cause was memory
         * pressure or a snapshot already being in flight. */
        if (!s_authenticated || !s_initial_state_received)
            ui_set_devices_error(s_auth_rejected
                ? "Home Assistant rejected the access token -- retrying (check Settings > SETUP)"
                : "Home Assistant is not connected");
        else if (s_states_req_id || s_inventory_req_id)
            ;   /* a snapshot is in flight -- its response renders this screen */
        else
            ui_set_devices_error("Home Assistant is busy -- try again shortly");
    }
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
    s_album_browse_entity_next = 0;
    if (s_album_browse_entity_count > 0)
        start_next_album_browse_entity();
    else
        ui_set_album_candidates(NULL, 0, "No cached HA media libraries found");
}

void ha_request_lights(void)
{
    if (s_lights_cache_valid)
        ui_set_lights_ext(s_lights, s_light_hues, s_light_sats,
                          s_light_temps, s_light_count);
    request_inventory_refresh("light inventory", false);
}

void ha_request_lights_fresh(void)
{
    /* Post-command settle: a light service call just changed real state. Do
     * NOT re-push the cache (it still holds the PRE-command state) and do NOT
     * snapshot inline -- arm a coalesced deadline that ha_client_tick() honours
     * once the Matter round-trip has settled. Each further command pushes the
     * deadline out, so a brightness drag or a run of toggles costs at most ONE
     * installation-wide get_states, never one per command. */
    s_lights_settle_due_us = esp_timer_get_time() + LIGHT_SETTLE_DELAY_US;
}

static const char *resolve_selected_entity(const char *sel, bool *is_ma,
                                           bool *is_spotify_source)
{
    if (is_ma) *is_ma = false;
    if (is_spotify_source) *is_spotify_source = false;
    if (!sel || !sel[0]) return NULL;

    const char *entity = sel;
    char *end = NULL;
    long idx = strtol(sel, &end, 10);
    if (end && *end == '\0' && idx >= 0 && idx < s_dev_count && s_dev_ids[idx][0]) {
        entity = s_dev_ids[idx];
        if (is_ma) *is_ma = s_dev_is_ma[idx];
    } else if (is_ma) {
        for (int i = 0; i < s_dev_count; i++) {
            if (strcmp(entity, s_dev_ids[i]) == 0) {
                *is_ma = s_dev_is_ma[i];
                break;
            }
        }
    }
    if (is_spotify_source)
        *is_spotify_source =
            strncmp(entity, SPOTIFY_SRC_PREFIX, strlen(SPOTIFY_SRC_PREFIX)) == 0;
    return entity;
}

void ha_switch_active_entity(const char *sel, bool transfer_playback)
{
    bool target_is_ma = false;
    bool target_is_spotify_source = false;
    const char *selected = resolve_selected_entity(sel, &target_is_ma,
                                                   &target_is_spotify_source);
    if (!selected) return;

    const char *target_entity = selected;
    if (target_is_spotify_source) {
        if (!s_spotify_entity[0]) return;
        target_entity = s_spotify_entity;
    }

    /* A switch between two Spotify Connect sources is already a native
     * Spotify transfer. Do not pause or restart the account session. */
    bool changes_entity = strcmp(target_entity, s_entity) != 0;
    if (!transfer_playback || !changes_entity || !s_track.is_playing) {
        ha_set_active_entity(sel);
        return;
    }

    char old_entity[sizeof(s_entity_buf)];
    snprintf(old_entity, sizeof(old_entity), "%s", s_entity);
    call_service_entity("media_player", "media_pause", old_entity, NULL);

    clear_pending_transfer();
    snprintf(s_transfer_entity, sizeof(s_transfer_entity), "%s", target_entity);
    snprintf(s_transfer_media_id, sizeof(s_transfer_media_id), "%s",
             s_media_content_id);
    snprintf(s_transfer_media_type, sizeof(s_transfer_media_type), "%s",
             s_media_content_type);
    snprintf(s_transfer_title, sizeof(s_transfer_title), "%s", s_track.title);
    snprintf(s_transfer_artist, sizeof(s_transfer_artist), "%s", s_track.artist);
    s_transfer_position_ms = s_track.progress_ms;
    s_transfer_target = target_is_spotify_source ? TRANSFER_TARGET_SPOTIFY
                      : target_is_ma             ? TRANSFER_TARGET_MA
                                                 : TRANSFER_TARGET_HA;

    ha_set_active_entity(sel);
    if (s_transfer_media_id[0]) {
        /* Give HA time to select a Spotify Connect source before play_media.
         * The seek follows one second after playback starts. */
        s_transfer_due_us = esp_timer_get_time()
                          + (target_is_spotify_source ? 800LL : 250LL) * 1000LL;
        ui_show_toast("Transferring playback...", 1800);
        ESP_LOGI(TAG, "output transfer: %s -> %s media_id='%s' track='%s -- %s'",
                 old_entity, target_entity, s_transfer_media_id,
                 s_transfer_artist, s_transfer_title);
    } else {
        ui_show_toast("Old output paused; current track has no transferable ID", 3500);
        clear_pending_transfer();
    }
}

void ha_set_active_entity(const char *sel)
{
    bool unused_is_ma = false;
    bool is_spotify_source = false;
    const char *entity = resolve_selected_entity(sel, &unused_is_ma,
                                                 &is_spotify_source);
    if (!entity) return;

    /* A Spotify Connect source row: ask the Spotify account entity to transfer
     * playback to that device, then follow the account entity for state. */
    if (is_spotify_source) {
        if (!s_spotify_entity[0]) return;
        const char *src = entity + strlen(SPOTIFY_SRC_PREFIX);
        char data[80];
        snprintf(data, sizeof(data), "\"source\":\"%s\"", src);
        call_service_entity("media_player", "select_source", s_spotify_entity, data);
        snprintf(s_spotify_source_now, sizeof(s_spotify_source_now), "%s", src);
        entity = s_spotify_entity;
    }

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
    s_active_is_ma = false;  /* refreshed from the newly selected state */
    s_art_loaded[0]          = '\0';   /* force the new entity's art to reload */
    s_track.album_art_url[0] = '\0';
    ESP_LOGI(TAG, "active entity -> %s", s_entity);

    /* Do not issue another installation-wide get_states request here. The
     * startup snapshot is intentionally the only large HA frame; switching an
     * output reuses its cached initial state and waits for this entity's small
     * trigger updates instead. This keeps Sendspin and HA within ESP-Hosted's
     * finite SDIO receive pool. */
    s_subscribe_pending = true;
}

bool ha_take_pending_art(char *rel_out, size_t out_len,
                         uint32_t *seq_out, int64_t *event_us_out)
{
    bool got = false;
    taskENTER_CRITICAL(&s_art_mux);
    if (s_art_pending) {
        strncpy(rel_out, s_pending_art, out_len - 1);
        rel_out[out_len - 1] = '\0';
        if (seq_out) *seq_out = s_pending_art_seq;
        if (event_us_out) *event_us_out = s_pending_art_event_us;
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
typedef struct {
    FILE *file;
    size_t bytes_written;
    bool write_failed;
} art_file_ctx_t;

static esp_err_t art_file_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data) {
        art_file_ctx_t *ctx = (art_file_ctx_t *)evt->user_data;
        size_t written = fwrite(evt->data, 1, evt->data_len, ctx->file);
        ctx->bytes_written += written;
        if (written != (size_t)evt->data_len) ctx->write_failed = true;
    }
    return ESP_OK;
}

static bool art_url_is_ha_host(const char *url)
{
    if (!url || !s_host) return false;
    char prefix[160];
    snprintf(prefix, sizeof(prefix), "http://%s:%d/", s_host, s_port);
    if (strncmp(url, prefix, strlen(prefix)) == 0) return true;
    snprintf(prefix, sizeof(prefix), "https://%s:%d/", s_host, s_port);
    return strncmp(url, prefix, strlen(prefix)) == 0;
}

bool ha_download_to_file(const char *url, const char *path, size_t *out_len,
                         bool send_ha_auth)
{
    FILE *f = fopen(path, "wb");
    if (!f) { ESP_LOGE(TAG, "open %s failed", path); return false; }
    art_file_ctx_t file_ctx = {
        .file = f,
    };

    esp_http_client_config_t cfg = {
        .url               = url,
        .event_handler     = art_file_event,
        .user_data         = &file_ctx,
        .crt_bundle_attach = esp_crt_bundle_attach,  /* enable https (Spotify covers) */
        .timeout_ms        = 8000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { fclose(f); return false; }

    /* HA's /api/media_player_proxy/ endpoints require authentication even on
     * the local network. Without the Bearer token they return 401. Only send it
     * to HA-host URLs -- never to an external CDN (would leak the HA token). */
    if (send_ha_auth && art_url_is_ha_host(url) && s_token && s_token[0]) {
        char bearer[320];
        snprintf(bearer, sizeof(bearer), "Bearer %s", s_token);
        esp_http_client_set_header(c, "Authorization", bearer);
    }

    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    int len    = esp_http_client_get_content_length(c);
    esp_http_client_cleanup(c);
    fclose(f);

    if (out_len) *out_len = file_ctx.bytes_written;
    bool length_ok = len < 0 || (size_t)len == file_ctx.bytes_written;
    bool ok = err == ESP_OK && status == 200 && file_ctx.bytes_written > 0 &&
              !file_ctx.write_failed && length_ok;
    if (!ok) {
        ESP_LOGW(TAG,
                 "art GET failed (err=%d status=%d expected=%d written=%u write_failed=%d)",
                 (int)err, status, len, (unsigned)file_ctx.bytes_written,
                 file_ctx.write_failed ? 1 : 0);
    }
    return ok;
}
