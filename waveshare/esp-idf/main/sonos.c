/* See sonos.h. UPnP/SOAP control of a Sonos speaker on the local network. */

#include "sonos.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include "esp_log.h"
#include "esp_http_client.h"

static const char *TAG = "sonos";

#define AVT_SVC   "urn:schemas-upnp-org:service:AVTransport:1"
#define RC_SVC    "urn:schemas-upnp-org:service:RenderingControl:1"
#define CD_SVC    "urn:schemas-upnp-org:service:ContentDirectory:1"
#define AVT_PATH  "/MediaRenderer/AVTransport/Control"
#define RC_PATH   "/MediaRenderer/RenderingControl/Control"
#define CD_PATH   "/MediaServer/ContentDirectory/Control"

/* Spotify-on-Sonos service identity, used to build the cdudn account tag in album
 * metadata. Derived from this household's live queue dump (Browse Q:0):
 *   track URI: x-sonos-spotify:...?sid=9&flags=8232&sn=3
 *   serviceType = sid*256 + 7 = 9*256+7 = 2311 (Spotify "global")
 * cdudn format: SA_RINCON<type>_X_#Svc<type>-<sn>-Token
 * If album-start returns 500, verify sid/sn from a fresh Browse dump. */
#define SONOS_SP_STYPE "2311"
#define SONOS_SP_SN    "3"

/* Build + send one SOAP action. `inner` is the action-specific XML inside the
 * <u:Action> element, after the always-present <InstanceID>0</InstanceID>. */
static bool soap_post(const char *host, const char *path, const char *svc,
                      const char *action, const char *inner)
{
    if (!host || !host[0]) return false;

    char url[96];
    snprintf(url, sizeof url, "http://%s:1400%s", host, path);

    char soapaction[160];
    snprintf(soapaction, sizeof soapaction, "\"%s#%s\"", svc, action);

    /* Large enough for SetAVTransportURI's escaped DIDL-Lite album metadata. */
    char body[2048];
    int n = snprintf(body, sizeof body,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body><u:%s xmlns:u=\"%s\"><InstanceID>0</InstanceID>%s</u:%s></s:Body>"
        "</s:Envelope>",
        action, svc, inner ? inner : "", action);
    if (n <= 0 || n >= (int)sizeof body) return false;

    esp_http_client_config_t cfg = {
        .url        = url,
        .method     = HTTP_METHOD_POST,
        .timeout_ms = 4000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return false;

    esp_http_client_set_header(c, "Content-Type", "text/xml; charset=\"utf-8\"");
    esp_http_client_set_header(c, "SOAPAction", soapaction);
    esp_http_client_set_post_field(c, body, n);

    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);

    bool ok = (err == ESP_OK && status == 200);
    if (!ok) ESP_LOGW(TAG, "%s -> err=%d status=%d", action, (int)err, status);
    return ok;
}

bool sonos_play(const char *host)     { return soap_post(host, AVT_PATH, AVT_SVC, "Play", "<Speed>1</Speed>"); }
bool sonos_pause(const char *host)    { return soap_post(host, AVT_PATH, AVT_SVC, "Pause", ""); }
bool sonos_next(const char *host)     { return soap_post(host, AVT_PATH, AVT_SVC, "Next", ""); }
bool sonos_previous(const char *host) { return soap_post(host, AVT_PATH, AVT_SVC, "Previous", ""); }

bool sonos_seek_ms(const char *host, uint32_t position_ms)
{
    uint32_t s = position_ms / 1000;
    char inner[96];
    snprintf(inner, sizeof inner,
             "<Unit>REL_TIME</Unit><Target>%lu:%02lu:%02lu</Target>",
             (unsigned long)(s / 3600), (unsigned long)((s % 3600) / 60),
             (unsigned long)(s % 60));
    return soap_post(host, AVT_PATH, AVT_SVC, "Seek", inner);
}

bool sonos_set_volume(const char *host, int pct)
{
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    char inner[96];
    snprintf(inner, sizeof inner,
             "<Channel>Master</Channel><DesiredVolume>%d</DesiredVolume>", pct);
    return soap_post(host, RC_PATH, RC_SVC, "SetVolume", inner);
}

/* Copy `src` into `dst`, replacing every ':' with "%3a" -- URL-encoding the
 * colons of a Spotify URI for use inside a Sonos cpcontainer URI / item id.
 * Returns false if it wouldn't fit. */
static bool encode_uri_colons(char *dst, size_t dstsz, const char *src)
{
    size_t j = 0;
    for (size_t i = 0; src[i]; i++) {
        if (src[i] == ':') {
            if (j + 3 >= dstsz) return false;
            dst[j++] = '%'; dst[j++] = '3'; dst[j++] = 'a';
        } else {
            if (j + 1 >= dstsz) return false;
            dst[j++] = src[i];
        }
    }
    if (j >= dstsz) return false;
    dst[j] = '\0';
    return true;
}

/* Defined in the Diagnostics section below; used here so a SOAP fault on the
 * album enqueue logs the speaker's error body instead of a bare HTTP 500. */
static char *soap_query(const char *host, const char *path, const char *svc,
                        const char *action, const char *inner, size_t *out_len);
/* Speaker UUID (e.g. "RINCON_347E5C123456789A01400"), fetched once from
 * /xml/device_description.xml so we can build the x-rincon-queue transport URI. */
static bool sonos_fetch_uuid(const char *host);
static char s_sonos_uuid[64];

bool sonos_play_spotify_album(const char *host, const char *album_uri)
{
    if (!host || !host[0] || !album_uri || !album_uri[0]) return false;

    char enc[96];
    if (!encode_uri_colons(enc, sizeof enc, album_uri)) return false;

    /* Shared DIDL-Lite block used in both the cpcontainer and queue paths below.
     * Magic prefixes per soco: container "1004206c", metadata item-id "00040000".
     * The cdudn service descriptor must match sn=SONOS_SP_SN from the queue's
     * track URIs (verified via Browse Q:0 dump). */
#define _DIDL_OPEN \
        "&lt;DIDL-Lite xmlns:dc=&quot;http://purl.org/dc/elements/1.1/&quot; " \
        "xmlns:upnp=&quot;urn:schemas-upnp-org:metadata-1-0/upnp/&quot; " \
        "xmlns:r=&quot;urn:schemas-rinconnetworks-com:metadata-1-0/&quot; " \
        "xmlns=&quot;urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/&quot;&gt;" \
        "&lt;item id=&quot;00040000%s&quot; parentID=&quot;-1&quot; restricted=&quot;true&quot;&gt;" \
        "&lt;dc:title&gt;&lt;/dc:title&gt;" \
        "&lt;upnp:class&gt;object.container.album.musicAlbum&lt;/upnp:class&gt;" \
        "&lt;desc id=&quot;cdudn&quot; nameSpace=&quot;urn:schemas-rinconnetworks-com:metadata-1-0/&quot;&gt;" \
        "SA_RINCON" SONOS_SP_STYPE "_X_#Svc" SONOS_SP_STYPE "-" SONOS_SP_SN "-Token" \
        "&lt;/desc&gt;&lt;/item&gt;&lt;/DIDL-Lite&gt;"

    char inner[1500];
    int n;

    /* Release from any external transport controller (e.g. a Spotify Connect
     * session that ended without cleaning up). Without this, SetAVTransportURI
     * returns error 714 (Illegal MIME-type) for x-rincon-queue and
     * x-rincon-cpcontainer URIs even when nothing is actively streaming.
     * Equivalent to soco's unjoin() before reusing the transport. */
    soap_post(host, AVT_PATH, AVT_SVC, "BecomeCoordinatorOfStandaloneGroup", "");

    /* First attempt: SetAVTransportURI directly with the cpcontainer URI.
     * Previously failed when the cdudn sn was wrong (0 instead of SONOS_SP_SN);
     * retried now with the correct sn from a live queue dump. If the firmware
     * accepts cpcontainer URIs this is simpler than the AddURIToQueue path. */
    n = snprintf(inner, sizeof inner,
        "<InstanceID>0</InstanceID>"
        "<CurrentURI>x-rincon-cpcontainer:1004206c%s</CurrentURI>"
        "<CurrentURIMetaData>" _DIDL_OPEN "</CurrentURIMetaData>",
        enc, enc);
    if (n > 0 && n < (int)sizeof inner) {
        char *r = soap_query(host, AVT_PATH, AVT_SVC, "SetAVTransportURI", inner, NULL);
        if (r) { free(r); return sonos_play(host); }
        /* soap_query already logged the fault -- fall through. */
    }

    /* Fallback: populate the local queue, then point the transport at it.
     * Required when the firmware rejects cpcontainer URIs in SetAVTransportURI. */
    soap_post(host, AVT_PATH, AVT_SVC, "RemoveAllTracksFromQueue", "");

    n = snprintf(inner, sizeof inner,
        "<InstanceID>0</InstanceID>"
        "<EnqueuedURI>x-rincon-cpcontainer:1004206c%s</EnqueuedURI>"
        "<EnqueuedURIMetaData>" _DIDL_OPEN "</EnqueuedURIMetaData>"
        "<DesiredFirstTrackNumberEnqueued>0</DesiredFirstTrackNumberEnqueued>"
        "<EnqueueAsNext>0</EnqueueAsNext>",
        enc, enc);
    if (n <= 0 || n >= (int)sizeof inner) return false;

    char *r = soap_query(host, AVT_PATH, AVT_SVC, "AddURIToQueue", inner, NULL);
    if (!r) { ESP_LOGW(TAG, "AddURIToQueue(%s) failed", album_uri); return false; }
    free(r);

    if (sonos_fetch_uuid(host)) {
        char set_inner[200];
        n = snprintf(set_inner, sizeof set_inner,
                     "<InstanceID>0</InstanceID>"
                     "<CurrentURI>x-rincon-queue:%s#0</CurrentURI>"
                     "<CurrentURIMetaData></CurrentURIMetaData>",
                     s_sonos_uuid);
        if (n > 0 && n < (int)sizeof set_inner) {
            /* soap_query logs the fault body if this returns 500. */
            char *sr = soap_query(host, AVT_PATH, AVT_SVC, "SetAVTransportURI", set_inner, NULL);
            if (sr) free(sr);
        }
    }
#undef _DIDL_OPEN
    return sonos_play(host);
}

/* ── Diagnostics ─────────────────────────────────────────────────────── */

typedef struct { char *data; size_t len; size_t cap; } rbuf_t;

static esp_err_t rbuf_evt(esp_http_client_event_t *e)
{
    if (e->event_id != HTTP_EVENT_ON_DATA || !e->user_data) return ESP_OK;
    rbuf_t *b = (rbuf_t *)e->user_data;
    size_t need = b->len + e->data_len + 1;
    if (need > b->cap) {
        size_t nc = b->cap ? b->cap : 512;
        while (nc < need) nc *= 2;
        if (nc > 8192) return ESP_OK;            /* enough for the DIDL we need */
        char *g = realloc(b->data, nc);
        if (!g) return ESP_FAIL;
        b->data = g; b->cap = nc;
    }
    memcpy(b->data + b->len, e->data, e->data_len);
    b->len += e->data_len;
    b->data[b->len] = '\0';
    return ESP_OK;
}

/* Fetch the speaker's UDN from /xml/device_description.xml and store it in
 * s_sonos_uuid (e.g. "RINCON_347E5C123456789A01400"). Cached after first call. */
static bool sonos_fetch_uuid(const char *host)
{
    if (s_sonos_uuid[0]) return true;

    char url[96];
    snprintf(url, sizeof url, "http://%s:1400/xml/device_description.xml", host);

    rbuf_t rb = {0};
    esp_http_client_config_t cfg = {
        .url           = url,
        .method        = HTTP_METHOD_GET,
        .event_handler = rbuf_evt,
        .user_data     = &rb,
        .timeout_ms    = 4000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return false;

    esp_err_t err  = esp_http_client_perform(c);
    int       st   = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);

    bool ok = false;
    if (err == ESP_OK && st == 200 && rb.data) {
        const char *p = strstr(rb.data, "<UDN>uuid:");
        if (p) {
            p += strlen("<UDN>uuid:");
            const char *e = strstr(p, "</UDN>");
            if (e) {
                size_t n = (size_t)(e - p);
                if (n < sizeof s_sonos_uuid) {
                    memcpy(s_sonos_uuid, p, n);
                    s_sonos_uuid[n] = '\0';
                    ESP_LOGI(TAG, "speaker uuid: %s", s_sonos_uuid);
                    ok = true;
                }
            }
        }
    }
    if (!ok)
        ESP_LOGW(TAG, "fetch_uuid: err=%d status=%d", (int)err, st);
    free(rb.data);
    return ok;
}

/* Persistent keep-alive client for SOAP queries. The now-playing poll fires
 * three queries back-to-back (GetPositionInfo + GetTransportInfo + GetVolume)
 * every few seconds, all to the same host:1400 -- reusing one TCP connection
 * across them (and across poll cycles) avoids a connect/teardown per query.
 * Single-threaded: only spotify_task calls soap_query. Dropped on transport
 * error so the next call reconnects instead of reusing a dead socket. */
static esp_http_client_handle_t s_query_client = NULL;

static void query_client_close(void)
{
    if (s_query_client) {
        esp_http_client_cleanup(s_query_client);
        s_query_client = NULL;
    }
}

/* POST a query action with caller-supplied inner args and return the response
 * body in a freshly-malloc'd, NUL-terminated buffer (caller frees). *out_len,
 * if non-NULL, receives the body length. Returns NULL on transport/HTTP error. */
static char *soap_query(const char *host, const char *path, const char *svc,
                        const char *action, const char *inner, size_t *out_len)
{
    if (!host || !host[0]) return NULL;

    char url[96];
    snprintf(url, sizeof url, "http://%s:1400%s", host, path);
    char soapaction[200];
    snprintf(soapaction, sizeof soapaction, "\"%s#%s\"", svc, action);
    /* Large enough for AddURIToQueue's escaped DIDL-Lite album metadata. */
    char body[2048];
    int n = snprintf(body, sizeof body,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body><u:%s xmlns:u=\"%s\">%s</u:%s></s:Body>"
        "</s:Envelope>", action, svc, inner ? inner : "", action);
    if (n <= 0 || n >= (int)sizeof body) return NULL;

    /* rb lives on this stack frame; perform() runs synchronously and fully
     * before we return, so pointing the persistent client's user_data at it
     * for the duration of this call is safe. */
    rbuf_t rb = {0};
    if (!s_query_client) {
        esp_http_client_config_t cfg = {
            .url               = url,
            .method            = HTTP_METHOD_POST,
            .event_handler     = rbuf_evt,
            .user_data         = &rb,
            .timeout_ms        = 2000,   /* fast-fail: a powered-off speaker must
                                          * not stall the inline poll for long */
            .keep_alive_enable = true,
        };
        s_query_client = esp_http_client_init(&cfg);
        if (!s_query_client) return NULL;
    } else {
        /* Same host:port across all Sonos actions, only the path/SOAPAction
         * differ, so the kept-alive TCP connection is reused. */
        esp_http_client_set_url(s_query_client, url);
        esp_http_client_set_method(s_query_client, HTTP_METHOD_POST);
        esp_http_client_set_user_data(s_query_client, &rb);
    }
    esp_http_client_handle_t c = s_query_client;

    esp_http_client_set_header(c, "Content-Type", "text/xml; charset=\"utf-8\"");
    esp_http_client_set_header(c, "SOAPAction", soapaction);
    esp_http_client_set_post_field(c, body, n);

    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);

    /* Transport error breaks the connection -- drop the handle so the next
     * query opens a fresh one rather than reusing a dead socket. */
    if (err != ESP_OK) query_client_close();

    if (err == ESP_OK && status == 200 && rb.data) {
        if (out_len) *out_len = rb.len;
        return rb.data;
    }
    ESP_LOGW(TAG, "%s failed err=%d status=%d", action, (int)err, status);
    if (rb.data) ESP_LOGW(TAG, "%s fault: %.400s", action, rb.data);
    free(rb.data);
    return NULL;
}

/* Run a query action and log the full response in 180-char chunks so nothing
 * is truncated by the logger. */
static bool soap_action_log(const char *host, const char *path, const char *svc,
                            const char *action, const char *inner)
{
    size_t len = 0;
    char *data = soap_query(host, path, svc, action, inner, &len);
    if (!data) return false;
    ESP_LOGI(TAG, "===== %s (paste everything between the ===== lines) =====", action);
    for (size_t i = 0; i < len; i += 180)
        ESP_LOGI(TAG, "%.180s", data + i);
    ESP_LOGI(TAG, "===== end %s =====", action);
    free(data);
    return true;
}

bool sonos_is_playing(const char *host)
{
    char *r = soap_query(host, AVT_PATH, AVT_SVC, "GetTransportInfo",
                         "<InstanceID>0</InstanceID>", NULL);
    bool playing = r && strstr(r, ">PLAYING<") != NULL;
    free(r);
    return playing;
}

/* Copy the text between the first `open` and the following `close` into dst. */
static bool xml_between(const char *src, const char *open, const char *close,
                        char *dst, size_t dstsz)
{
    const char *a = strstr(src, open);
    if (!a) return false;
    a += strlen(open);
    const char *b = strstr(a, close);
    if (!b) return false;
    size_t n = (size_t)(b - a);
    if (n >= dstsz) n = dstsz - 1;
    memcpy(dst, a, n);
    dst[n] = '\0';
    return true;
}

/* Encode one Unicode code point as UTF-8. Returns bytes written (1-4).
 * Caller guarantees cp <= 0x10FFFF. */
static size_t utf8_encode(char *dst, uint32_t cp)
{
    if (cp < 0x80) {
        dst[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        dst[0] = (char)(0xC0 | (cp >> 6));
        dst[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        dst[0] = (char)(0xE0 | (cp >> 12));
        dst[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    dst[0] = (char)(0xF0 | (cp >> 18));
    dst[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    dst[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    dst[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* In-place single-pass un-escape of XML entities: the five named ones plus
 * numeric character references (&#39; / &#x27;), which Sonos DIDL uses for
 * apostrophes and other punctuation in track metadata. In-place is safe:
 * an entity's text is always at least as long as its UTF-8 encoding (the
 * shortest producing 2/3/4 bytes are &#128; / &#2048; / &#65536;), so the
 * write cursor never overtakes the read cursor. */
static void xml_unescape(char *s)
{
    char *w = s;
    for (char *r = s; *r; ) {
        if (r[0] == '&') {
            if (!strncmp(r, "&lt;", 4))   { *w++ = '<';  r += 4; continue; }
            if (!strncmp(r, "&gt;", 4))   { *w++ = '>';  r += 4; continue; }
            if (!strncmp(r, "&quot;", 6)) { *w++ = '"';  r += 6; continue; }
            if (!strncmp(r, "&apos;", 6)) { *w++ = '\''; r += 6; continue; }
            if (!strncmp(r, "&amp;", 5))  { *w++ = '&';  r += 5; continue; }
            if (r[1] == '#') {
                /* Require a leading digit (strtoul alone would accept
                 * whitespace/sign), a terminating ';', and a valid scalar
                 * code point; anything else copies through literally. */
                bool hex = (r[2] == 'x' || r[2] == 'X');
                const char *digits = r + (hex ? 3 : 2);
                if (hex ? isxdigit((unsigned char)*digits)
                        : isdigit((unsigned char)*digits)) {
                    char *end;
                    unsigned long cp = strtoul(digits, &end, hex ? 16 : 10);
                    if (*end == ';' && cp >= 1 && cp <= 0x10FFFF &&
                        !(cp >= 0xD800 && cp <= 0xDFFF)) {
                        w += utf8_encode(w, (uint32_t)cp);
                        r = end + 1;
                        continue;
                    }
                }
            }
        }
        *w++ = *r++;
    }
    *w = '\0';
}

/* The DIDL-extracted fields are entity-escaped twice: once at DIDL level
 * (a title apostrophe is &#39; inside the DIDL XML) and once more when the
 * whole DIDL blob is embedded in the SOAP response (&amp;#39; on the wire).
 * Decode the SOAP layer, then the DIDL layer. */
static void didl_unescape(char *s)
{
    xml_unescape(s);
    xml_unescape(s);
}

/* "H:MM:SS" (or "NOT_IMPLEMENTED") -> milliseconds. */
static uint32_t hms_to_ms(const char *s)
{
    int h = 0, m = 0, sec = 0;
    if (sscanf(s, "%d:%d:%d", &h, &m, &sec) != 3) return 0;
    /* Reject negative components ("NOT_IMPLEMENTED" already fails the sscanf): a
     * negative would wrap to a huge uint32 and skew the progress bar. */
    if (h < 0 || m < 0 || sec < 0) return 0;
    return (uint32_t)(((h * 60 + m) * 60 + sec) * 1000);
}

bool sonos_fetch_now_playing(const char *host, sonos_np_t *out)
{
    if (!host || !host[0] || !out) return false;
    char *body = soap_query(host, AVT_PATH, AVT_SVC, "GetPositionInfo",
                            "<InstanceID>0</InstanceID>", NULL);
    if (!body) return false;

    memset(out, 0, sizeof *out);
    char tmp[16];
    if (xml_between(body, "<TrackDuration>", "</TrackDuration>", tmp, sizeof tmp))
        out->duration_ms = hms_to_ms(tmp);
    if (xml_between(body, "<RelTime>", "</RelTime>", tmp, sizeof tmp))
        out->progress_ms = hms_to_ms(tmp);

    /* TrackMetaData is an escaped DIDL-Lite blob, so the inner tags read as
     * &lt;dc:title&gt; etc.; the extracted text is still double-escaped
     * (SOAP layer + DIDL layer) and is decoded by didl_unescape below. */
    bool got = xml_between(body, "&lt;dc:title&gt;", "&lt;/dc:title&gt;",
                           out->title, sizeof out->title);
    xml_between(body, "&lt;dc:creator&gt;", "&lt;/dc:creator&gt;",
                out->artist, sizeof out->artist);
    xml_between(body, "&lt;upnp:album&gt;", "&lt;/upnp:album&gt;",
                out->album, sizeof out->album);
    free(body);

    if (!got || out->title[0] == '\0') return false;   /* stopped / no track */
    didl_unescape(out->title);
    didl_unescape(out->artist);
    didl_unescape(out->album);
    out->is_playing = sonos_is_playing(host);           /* PLAYING vs PAUSED */
    out->volume     = sonos_get_volume(host);
    return true;
}

int sonos_get_volume(const char *host)
{
    char *r = soap_query(host, RC_PATH, RC_SVC, "GetVolume",
                         "<InstanceID>0</InstanceID><Channel>Master</Channel>", NULL);
    if (!r) return -1;
    char tmp[8];
    int vol = -1;
    if (xml_between(r, "<CurrentVolume>", "</CurrentVolume>", tmp, sizeof tmp)) {
        int v = atoi(tmp);
        if (v >= 0 && v <= 100) vol = v;   /* clamp to a sane Sonos range */
    }
    free(r);
    return vol;
}

bool sonos_log_diag(const char *host)
{
    bool a = soap_action_log(host, AVT_PATH, AVT_SVC, "GetMediaInfo",
                             "<InstanceID>0</InstanceID>");
    bool b = soap_action_log(host, AVT_PATH, AVT_SVC, "GetPositionInfo",
                             "<InstanceID>0</InstanceID>");
    /* The cdudn service descriptor lives on the queue container, not the
     * position info -- browse the queue to capture it. */
    bool c = soap_action_log(host, CD_PATH, CD_SVC, "Browse",
        "<ObjectID>Q:0</ObjectID><BrowseFlag>BrowseDirectChildren</BrowseFlag>"
        "<Filter>*</Filter><StartingIndex>0</StartingIndex>"
        "<RequestedCount>2</RequestedCount><SortCriteria></SortCriteria>");
    return a || b || c;
}
