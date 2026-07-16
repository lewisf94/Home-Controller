#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>   /* strcasecmp / strncasecmp */

#include "albums.h"
#include "album_thumbs.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "album_catalog";

#define NVS_NS_ALBUMS       "albumcat"
#define NVS_KEY_RUNTIME     "runtime"
#define STORE_BYTES         4096
#define R_TITLE_BYTES       80
#define R_ARTIST_BYTES      56
#define R_URI_BYTES         64
#define R_URL_BYTES         100
#define ALBUM_CATALOG_MAX_RUNTIME 16
#define RT_THUMB_BYTES      ((size_t)ALBUM_THUMB_W * ALBUM_THUMB_H * sizeof(uint16_t))
#define RT_THUMB_CACHE_DIR  "/littlefs"

typedef struct {
    char title[R_TITLE_BYTES];
    char artist[R_ARTIST_BYTES];
    char uri[R_URI_BYTES];
    char image_url[R_URL_BYTES];   /* Spotify cover URL; art fetched at runtime */
} runtime_album_t;

static runtime_album_t s_runtime[ALBUM_CATALOG_MAX_RUNTIME];
static album_entry_t   s_runtime_entries[ALBUM_CATALOG_MAX_RUNTIME];
static size_t          s_runtime_count = 0;
static bool            s_loaded = false;

/* Runtime cover thumbnails are one PSRAM allocation per populated album, not
 * one MAX-sized pool. Each decoded 220x220 RGB565 thumb is also cached in the
 * existing LittleFS data partition under its stable Spotify album id, so boot
 * can restore art without another HTTPS/JPEG pass. A slot is published only
 * after its complete file/copy succeeds. */
static uint16_t *s_rt_thumbs[ALBUM_CATALOG_MAX_RUNTIME] = { NULL };
static bool      s_rt_filled[ALBUM_CATALOG_MAX_RUNTIME] = { false };

/* Display order: the baked list stays in its (gen_albums-sorted) order and each
 * runtime album is inserted at its alphabetical slot, so an added album no
 * longer lands at the end. Display position -> a source ref (baked blob index
 * or runtime index); album_catalog_get() and album_catalog_thumb() both index
 * through this, which keeps thumbnails aligned to the sorted order. */
#define ALBUM_CATALOG_ORDER_MAX 320
typedef struct { bool runtime; uint16_t idx; } album_ref_t;
static album_ref_t s_order[ALBUM_CATALOG_ORDER_MAX];
static size_t      s_order_count = 0;

static const char *skip_article(const char *s)
{
    if (!s) return "";
    while (*s == ' ') s++;
    if (strncasecmp(s, "the ", 4) == 0) return s + 4;
    if (strncasecmp(s, "an ", 3) == 0)  return s + 3;
    if (strncasecmp(s, "a ", 2) == 0)   return s + 2;
    return s;
}

/* Sort key: artist (leading article ignored) then title -- matches
 * scripts/gen_albums.py so runtime albums merge into the baked ordering. */
static int cat_cmp_entry(const album_entry_t *a, const album_entry_t *b)
{
    int c = strcasecmp(skip_article(a->artist), skip_article(b->artist));
    if (c) return c;
    return strcasecmp(skip_article(a->title), skip_article(b->title));
}

static const album_entry_t *entry_of(album_ref_t r)
{
    return r.runtime ? &s_runtime_entries[r.idx] : albums_get(r.idx);
}

static void rebuild_order(void)
{
    s_order_count = 0;
    size_t baked = albums_count();
    for (size_t i = 0; i < baked && s_order_count < ALBUM_CATALOG_ORDER_MAX; i++)
        s_order[s_order_count++] = (album_ref_t){ .runtime = false, .idx = (uint16_t)i };

    for (size_t r = 0; r < s_runtime_count && s_order_count < ALBUM_CATALOG_ORDER_MAX; r++) {
        const album_entry_t *re = &s_runtime_entries[r];
        size_t pos = s_order_count;
        for (size_t j = 0; j < s_order_count; j++) {
            if (cat_cmp_entry(re, entry_of(s_order[j])) < 0) { pos = j; break; }
        }
        for (size_t k = s_order_count; k > pos; k--) s_order[k] = s_order[k - 1];
        s_order[pos] = (album_ref_t){ .runtime = true, .idx = (uint16_t)r };
        s_order_count++;
    }
}

static void bind_runtime_entry(size_t i)
{
    s_runtime_entries[i].title  = s_runtime[i].title;
    s_runtime_entries[i].artist = s_runtime[i].artist;
    s_runtime_entries[i].uri    = s_runtime[i].uri;
}

static void copy_clean(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) return;
    size_t i = 0;
    if (src) {
        while (*src && i + 1 < dst_len) {
            char c = *src++;
            dst[i++] = (c == '\t' || c == '\r' || c == '\n') ? ' ' : c;
        }
    }
    dst[i] = '\0';
}

static bool runtime_thumb_path(const char *uri, char *out, size_t out_len)
{
    static const char prefix[] = "spotify:album:";
    if (!uri || strncmp(uri, prefix, sizeof(prefix) - 1) != 0 || !out || out_len == 0)
        return false;

    const char *id = uri + sizeof(prefix) - 1;
    size_t id_len = 0;
    while (id[id_len]) {
        char c = id[id_len];
        bool alnum = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                     (c >= 'a' && c <= 'z');
        if (!alnum || id_len >= 48) return false;
        id_len++;
    }
    if (id_len == 0) return false;

    int n = snprintf(out, out_len, RT_THUMB_CACHE_DIR "/album_%s.rgb565", id);
    return n > 0 && (size_t)n < out_len;
}

static uint16_t *runtime_thumb_alloc(size_t rt_idx)
{
    if (rt_idx >= ALBUM_CATALOG_MAX_RUNTIME) return NULL;
    if (!s_rt_thumbs[rt_idx]) {
        s_rt_thumbs[rt_idx] = heap_caps_malloc(
            RT_THUMB_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_rt_thumbs[rt_idx])
            ESP_LOGW(TAG, "runtime thumb alloc failed (album %u, %u B)",
                     (unsigned)rt_idx, (unsigned)RT_THUMB_BYTES);
    }
    return s_rt_thumbs[rt_idx];
}

static void load_runtime_thumb_cache(void)
{
    size_t loaded = 0;
    for (size_t i = 0; i < s_runtime_count; i++) {
        char path[96];
        if (!runtime_thumb_path(s_runtime[i].uri, path, sizeof(path))) continue;

        FILE *f = fopen(path, "rb");
        if (!f) continue;
        uint16_t *slot = runtime_thumb_alloc(i);
        if (!slot) {
            fclose(f);
            break;
        }

        size_t got = fread(slot, 1, RT_THUMB_BYTES, f);
        int extra = fgetc(f);
        fclose(f);
        if (got == RT_THUMB_BYTES && extra == EOF) {
            s_rt_filled[i] = true;
            loaded++;
        } else {
            ESP_LOGW(TAG, "runtime thumb cache invalid (album %u, %u/%u B)",
                     (unsigned)i, (unsigned)got, (unsigned)RT_THUMB_BYTES);
            heap_caps_free(slot);
            s_rt_thumbs[i] = NULL;
        }
    }
    if (loaded) ESP_LOGI(TAG, "restored %u runtime album covers from cache", (unsigned)loaded);
}

static bool persist_runtime_thumb(size_t rt_idx, const uint16_t *rgb)
{
    if (rt_idx >= s_runtime_count || !rgb) return false;

    char path[96];
    char tmp[104];
    if (!runtime_thumb_path(s_runtime[rt_idx].uri, path, sizeof(path))) return false;
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (n <= 0 || (size_t)n >= sizeof(tmp)) return false;

    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    size_t wrote = fwrite(rgb, 1, RT_THUMB_BYTES, f);
    bool ok = (fclose(f) == 0 && wrote == RT_THUMB_BYTES);
    if (!ok) {
        remove(tmp);
        return false;
    }

    if (rename(tmp, path) != 0) {
        remove(path);
        if (rename(tmp, path) != 0) {
            remove(tmp);
            return false;
        }
    }
    return true;
}

static bool persist_runtime(void)
{
    /* Static, not stack: 4 KB would be a large bite out of the LVGL task's
     * stack (this runs from the ADD button's event handler). Single-writer --
     * only the LVGL task adds and init runs before it starts. */
    static char buf[STORE_BYTES];
    size_t off = 0;
    for (size_t i = 0; i < s_runtime_count; i++) {
        int n = snprintf(buf + off, sizeof(buf) - off, "%s\t%s\t%s\t%s\n",
                         s_runtime[i].uri, s_runtime[i].title, s_runtime[i].artist,
                         s_runtime[i].image_url);
        if (n < 0 || (size_t)n >= sizeof(buf) - off) {
            ESP_LOGW(TAG, "runtime catalogue too large to persist");
            return false;
        }
        off += (size_t)n;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_ALBUMS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return false;
    }
    err = nvs_set_str(h, NVS_KEY_RUNTIME, buf);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "persist failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static void load_runtime(void)
{
    s_runtime_count = 0;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_ALBUMS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        s_loaded = true;
        return;
    }

    static char buf[STORE_BYTES];   /* static: see persist_runtime */
    size_t len = sizeof(buf);
    err = nvs_get_str(h, NVS_KEY_RUNTIME, buf, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        s_loaded = true;
        return;
    }

    char *p = buf;
    while (*p && s_runtime_count < ALBUM_CATALOG_MAX_RUNTIME) {
        char *line = p;
        char *nl = strchr(line, '\n');
        if (nl) {
            *nl = '\0';
            p = nl + 1;
        } else {
            p = line + strlen(line);
        }

        char *t1 = strchr(line, '\t');       /* after uri */
        if (!t1) continue;
        *t1++ = '\0';
        char *t2 = strchr(t1, '\t');         /* after title */
        if (!t2) continue;
        *t2++ = '\0';
        char *t3 = strchr(t2, '\t');         /* after artist (image_url; may be absent in old 3-field rows) */
        if (t3) *t3++ = '\0';
        if (strncmp(line, "spotify:album:", 14) != 0 || !t1[0]) continue;

        runtime_album_t *r = &s_runtime[s_runtime_count];
        copy_clean(r->uri, sizeof(r->uri), line);
        copy_clean(r->title, sizeof(r->title), t1);
        copy_clean(r->artist, sizeof(r->artist), t2[0] ? t2 : "Unknown artist");
        copy_clean(r->image_url, sizeof(r->image_url), t3 ? t3 : "");
        bind_runtime_entry(s_runtime_count);
        s_runtime_count++;
    }
    s_loaded = true;
    ESP_LOGI(TAG, "loaded %u runtime albums", (unsigned)s_runtime_count);
}

void album_catalog_init(void)
{
    if (!s_loaded) {
        load_runtime();
        load_runtime_thumb_cache();
        rebuild_order();
    }
}

size_t album_catalog_baked_count(void)
{
    return albums_count();
}

size_t album_catalog_count(void)
{
    album_catalog_init();
    return s_order_count;
}

const album_entry_t *album_catalog_get(size_t index)
{
    album_catalog_init();
    if (index >= s_order_count) return NULL;
    return entry_of(s_order[index]);
}

/* RGB565 thumbnail for display position `index`, or NULL for the letter-card
 * fallback. Baked slots return the embedded blob; runtime albums return their
 * fetched cover once album_catalog_set_thumb() has filled the slot (NULL ->
 * letter card until then). */
const uint16_t *album_catalog_thumb(size_t index)
{
    album_catalog_init();
    if (index >= s_order_count) return NULL;
    album_ref_t r = s_order[index];
    if (r.runtime) {
        if (r.idx < ALBUM_CATALOG_MAX_RUNTIME && s_rt_filled[r.idx])
            return s_rt_thumbs[r.idx];
        return NULL;
    }
    return album_thumb_data(r.idx);
}

bool album_catalog_contains_uri(const char *uri)
{
    if (!uri || !uri[0]) return false;
    album_catalog_init();
    size_t baked = albums_count();
    for (size_t i = 0; i < baked; i++) {
        const album_entry_t *a = albums_get(i);
        if (a && a->uri && strcmp(a->uri, uri) == 0) return true;
    }
    for (size_t i = 0; i < s_runtime_count; i++) {
        if (strcmp(s_runtime[i].uri, uri) == 0) return true;
    }
    return false;
}

bool album_catalog_add(const char *title, const char *artist, const char *uri,
                       const char *image_url)
{
    if (!uri || strncmp(uri, "spotify:album:", 14) != 0 || !title || !title[0])
        return false;
    album_catalog_init();
    if (album_catalog_contains_uri(uri)) return false;
    if (s_runtime_count >= ALBUM_CATALOG_MAX_RUNTIME) {
        ESP_LOGW(TAG, "runtime album limit reached (%d)", ALBUM_CATALOG_MAX_RUNTIME);
        return false;
    }

    runtime_album_t *r = &s_runtime[s_runtime_count];
    copy_clean(r->uri, sizeof(r->uri), uri);
    copy_clean(r->title, sizeof(r->title), title);
    copy_clean(r->artist, sizeof(r->artist), (artist && artist[0]) ? artist : "Unknown artist");
    copy_clean(r->image_url, sizeof(r->image_url), image_url ? image_url : "");
    s_rt_filled[s_runtime_count] = false;   /* cover fetched fresh for this slot */
    bind_runtime_entry(s_runtime_count);
    s_runtime_count++;

    if (!persist_runtime()) {
        s_runtime_count--;
        return false;
    }
    rebuild_order();
    ESP_LOGI(TAG, "added runtime album: %s -- %s", r->artist, r->title);
    return true;
}

size_t album_catalog_runtime_count(void)
{
    album_catalog_init();
    return s_runtime_count;
}

/* For the backend cover fetcher: if runtime album `rt_idx` still needs art (has
 * an image_url and no thumb yet), copy its URL into url_out and return true. */
bool album_catalog_runtime_art_todo(size_t rt_idx, char *url_out, size_t url_len)
{
    album_catalog_init();
    if (rt_idx >= s_runtime_count) return false;
    if (rt_idx < ALBUM_CATALOG_MAX_RUNTIME && s_rt_filled[rt_idx]) return false;
    if (!s_runtime[rt_idx].image_url[0]) return false;
    if (url_out && url_len) snprintf(url_out, url_len, "%s", s_runtime[rt_idx].image_url);
    return true;
}

/* Broader cover-fetch query used by backends that can repair older runtime rows
 * which were saved before image_url was available. Returns true for any runtime
 * album without a thumb yet, copying both the current image_url and Spotify URI. */
bool album_catalog_runtime_needs_art(size_t rt_idx, char *url_out, size_t url_len,
                                     char *uri_out, size_t uri_len)
{
    album_catalog_init();
    if (url_out && url_len) url_out[0] = '\0';
    if (uri_out && uri_len) uri_out[0] = '\0';
    if (rt_idx >= s_runtime_count) return false;
    if (rt_idx < ALBUM_CATALOG_MAX_RUNTIME && s_rt_filled[rt_idx]) return false;
    if (url_out && url_len) snprintf(url_out, url_len, "%s", s_runtime[rt_idx].image_url);
    if (uri_out && uri_len) snprintf(uri_out, uri_len, "%s", s_runtime[rt_idx].uri);
    return true;
}

bool album_catalog_set_image_url(size_t rt_idx, const char *image_url)
{
    album_catalog_init();
    if (rt_idx >= s_runtime_count || !image_url || !image_url[0]) return false;

    char old[R_URL_BYTES];
    snprintf(old, sizeof(old), "%s", s_runtime[rt_idx].image_url);
    copy_clean(s_runtime[rt_idx].image_url, sizeof(s_runtime[rt_idx].image_url), image_url);
    if (!persist_runtime()) {
        snprintf(s_runtime[rt_idx].image_url, sizeof(s_runtime[rt_idx].image_url), "%s", old);
        return false;
    }
    ESP_LOGI(TAG, "runtime album art URL repaired (album %u)", (unsigned)rt_idx);
    return true;
}

/* Store a decoded ALBUM_THUMB_W x H RGB565 cover for runtime album `rt_idx`
 * (called from the backend task). album_catalog_thumb() returns it on the next
 * browser rebuild. The filled flag is set last, after the copy. */
bool album_catalog_set_thumb(size_t rt_idx, const uint16_t *rgb)
{
    album_catalog_init();
    if (rt_idx >= s_runtime_count || rt_idx >= ALBUM_CATALOG_MAX_RUNTIME || !rgb)
        return false;
    uint16_t *slot = runtime_thumb_alloc(rt_idx);
    if (!slot) return false;
    memcpy(slot, rgb, RT_THUMB_BYTES);
    s_rt_filled[rt_idx] = true;
    if (!persist_runtime_thumb(rt_idx, slot))
        ESP_LOGW(TAG, "runtime thumb cache write failed (album %u)", (unsigned)rt_idx);
    return true;
}
