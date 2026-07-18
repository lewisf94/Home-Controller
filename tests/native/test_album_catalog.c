#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "album_thumbs.h"
#include "albums.h"
#include "esp_heap_caps.h"
#include "nvs.h"

static char g_nvs_value[4096];
static bool g_nvs_present;
static bool g_commit_fails;
static uint16_t g_baked_thumbs[2][ALBUM_THUMB_W * ALBUM_THUMB_H];

static const album_entry_t g_baked[] = {
    {"Alpha Album", "A Artist", "spotify:album:BAKED1"},
    {"Charlie Album", "The C Artist", "spotify:album:BAKED2"},
};

const album_entry_t *albums_get(size_t index)
{
    return index < sizeof(g_baked) / sizeof(g_baked[0]) ? &g_baked[index] : NULL;
}
size_t albums_count(void) { return sizeof(g_baked) / sizeof(g_baked[0]); }
const uint16_t *album_thumb_data(size_t index)
{
    return index < 2 ? g_baked_thumbs[index] : NULL;
}

void *heap_caps_malloc(size_t size, uint32_t caps) { (void)caps; return malloc(size); }
void heap_caps_free(void *ptr) { free(ptr); }
size_t heap_caps_get_free_size(uint32_t caps) { (void)caps; return 0; }
size_t heap_caps_get_minimum_free_size(uint32_t caps) { (void)caps; return 0; }
size_t heap_caps_get_largest_free_block(uint32_t caps) { (void)caps; return 0; }
esp_err_t heap_caps_register_failed_alloc_callback(
    void (*callback)(size_t, uint32_t, const char *))
{ (void)callback; return ESP_OK; }

esp_err_t nvs_open(const char *namespace_name, int mode, nvs_handle_t *handle)
{
    (void)namespace_name; (void)mode; *handle = 1; return ESP_OK;
}
esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *out, size_t *length)
{
    (void)handle; (void)key;
    if (!g_nvs_present) return ESP_ERR_NVS_NOT_FOUND;
    size_t needed = strlen(g_nvs_value) + 1;
    if (*length < needed) return ESP_FAIL;
    memcpy(out, g_nvs_value, needed);
    *length = needed;
    return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value)
{
    (void)handle; (void)key;
    snprintf(g_nvs_value, sizeof(g_nvs_value), "%s", value);
    g_nvs_present = true;
    return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{ (void)handle; (void)key; g_nvs_present = false; return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t handle) { (void)handle; return g_commit_fails ? ESP_FAIL : ESP_OK; }
void nvs_close(nvs_handle_t handle) { (void)handle; }

#include "../../../waveshare/components/p4_shared/album_catalog.c"

static void scenario_helpers(void)
{
    assert(strcmp(skip_article("  The Artist"), "Artist") == 0);
    assert(strcmp(skip_article("An Artist"), "Artist") == 0);
    assert(strcmp(skip_article("A Artist"), "Artist") == 0);
    assert(strcmp(skip_article("Artist"), "Artist") == 0);

    char clean[8];
    copy_clean(clean, sizeof(clean), "A\tB\nC");
    assert(strcmp(clean, "A B C") == 0);
    copy_clean(clean, sizeof(clean), "123456789");
    assert(strcmp(clean, "1234567") == 0);

    char path[96];
    assert(runtime_thumb_path("spotify:album:AbC123", path, sizeof(path)));
    assert(strcmp(path, "/littlefs/album_AbC123.rgb565") == 0);
    assert(!runtime_thumb_path("spotify:track:AbC123", path, sizeof(path)));
    assert(!runtime_thumb_path("spotify:album:bad/id", path, sizeof(path)));
    assert(!runtime_thumb_path("spotify:album:", path, sizeof(path)));
}

static void scenario_load_sort(void)
{
    snprintf(g_nvs_value, sizeof(g_nvs_value),
             "spotify:album:RUNTIME1\tBeta Album\tB Artist\thttps://img/1\n"
             "malformed row\n"
             "spotify:album:RUNTIME2\tDelta Album\tD Artist\n");
    g_nvs_present = true;
    album_catalog_init();
    assert(album_catalog_count() == 4);
    assert(strcmp(album_catalog_get(0)->uri, "spotify:album:BAKED1") == 0);
    assert(strcmp(album_catalog_get(1)->uri, "spotify:album:RUNTIME1") == 0);
    assert(strcmp(album_catalog_get(2)->uri, "spotify:album:BAKED2") == 0);
    assert(strcmp(album_catalog_get(3)->uri, "spotify:album:RUNTIME2") == 0);
    assert(album_catalog_contains_uri("spotify:album:RUNTIME1"));
    assert(!album_catalog_contains_uri("spotify:album:MISSING"));
    assert(album_catalog_thumb(0) == g_baked_thumbs[0]);
    assert(album_catalog_thumb(99) == NULL);
}

static void scenario_add(void)
{
    album_catalog_init();
    assert(album_catalog_add("New\nAlbum", NULL, "spotify:album:NEW1", "https://img/new"));
    assert(album_catalog_runtime_count() == 1);
    assert(strstr(g_nvs_value, "New Album\tUnknown artist\thttps://img/new") != NULL);
    assert(!album_catalog_add("Duplicate", "Artist", "spotify:album:NEW1", NULL));
    assert(!album_catalog_add("", "Artist", "spotify:album:EMPTY", NULL));
    assert(!album_catalog_add("Bad", "Artist", "spotify:track:BAD", NULL));

    g_commit_fails = true;
    assert(!album_catalog_add("Rollback", "Artist", "spotify:album:ROLLBACK", NULL));
    assert(album_catalog_runtime_count() == 1);
}

static void scenario_art(void)
{
    snprintf(g_nvs_value, sizeof(g_nvs_value),
             "spotify:album:RUNTIME1\tBeta Album\tB Artist\t\n");
    g_nvs_present = true;
    album_catalog_init();

    char url[100];
    char uri[64];
    assert(album_catalog_runtime_needs_art(0, url, sizeof(url), uri, sizeof(uri)));
    assert(url[0] == '\0');
    assert(strcmp(uri, "spotify:album:RUNTIME1") == 0);
    assert(!album_catalog_runtime_art_todo(0, url, sizeof(url)));
    assert(album_catalog_set_image_url(0, "https://img/repaired"));
    assert(album_catalog_runtime_art_todo(0, url, sizeof(url)));
    assert(strcmp(url, "https://img/repaired") == 0);

    uint16_t pixels[ALBUM_THUMB_W * ALBUM_THUMB_H] = {0};
    assert(album_catalog_set_thumb(0, pixels));
    assert(!album_catalog_runtime_needs_art(0, url, sizeof(url), uri, sizeof(uri)));
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (strcmp(argv[1], "helpers") == 0) scenario_helpers();
    else if (strcmp(argv[1], "load-sort") == 0) scenario_load_sort();
    else if (strcmp(argv[1], "add") == 0) scenario_add();
    else if (strcmp(argv[1], "art") == 0) scenario_art();
    else return 2;
    return 0;
}
