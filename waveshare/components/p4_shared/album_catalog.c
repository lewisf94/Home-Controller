#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>   /* strcasecmp / strncasecmp */

#include "albums.h"
#include "album_thumbs.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "album_catalog";

#define NVS_NS_ALBUMS       "albumcat"
#define NVS_KEY_RUNTIME     "runtime"
#define STORE_BYTES         4096
#define R_TITLE_BYTES       80
#define R_ARTIST_BYTES      56
#define R_URI_BYTES         64
#define ALBUM_CATALOG_MAX_RUNTIME 16

typedef struct {
    char title[R_TITLE_BYTES];
    char artist[R_ARTIST_BYTES];
    char uri[R_URI_BYTES];
} runtime_album_t;

static runtime_album_t s_runtime[ALBUM_CATALOG_MAX_RUNTIME];
static album_entry_t   s_runtime_entries[ALBUM_CATALOG_MAX_RUNTIME];
static size_t          s_runtime_count = 0;
static bool            s_loaded = false;

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

static bool persist_runtime(void)
{
    /* Static, not stack: 4 KB would be a large bite out of the LVGL task's
     * stack (this runs from the ADD button's event handler). Single-writer --
     * only the LVGL task adds and init runs before it starts. */
    static char buf[STORE_BYTES];
    size_t off = 0;
    for (size_t i = 0; i < s_runtime_count; i++) {
        int n = snprintf(buf + off, sizeof(buf) - off, "%s\t%s\t%s\n",
                         s_runtime[i].uri, s_runtime[i].title, s_runtime[i].artist);
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

        char *t1 = strchr(line, '\t');
        if (!t1) continue;
        *t1++ = '\0';
        char *t2 = strchr(t1, '\t');
        if (!t2) continue;
        *t2++ = '\0';
        if (strncmp(line, "spotify:album:", 14) != 0 || !t1[0]) continue;

        runtime_album_t *r = &s_runtime[s_runtime_count];
        copy_clean(r->uri, sizeof(r->uri), line);
        copy_clean(r->title, sizeof(r->title), t1);
        copy_clean(r->artist, sizeof(r->artist), t2[0] ? t2 : "Unknown artist");
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
 * fallback. Baked slots return the embedded blob; runtime albums have no
 * embedded art yet (fetched covers land in a later change), so return NULL. */
const uint16_t *album_catalog_thumb(size_t index)
{
    album_catalog_init();
    if (index >= s_order_count) return NULL;
    album_ref_t r = s_order[index];
    if (r.runtime) return NULL;
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

bool album_catalog_add(const char *title, const char *artist, const char *uri)
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
