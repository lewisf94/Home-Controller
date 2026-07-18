#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "nvs.h"

static char g_value[512];
static bool g_present;
static bool g_open_fails;
static bool g_commit_fails;
static int g_commits;

size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t length = strlen(src);
    if (size) {
        size_t copy = length < size - 1 ? length : size - 1;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return length;
}

esp_err_t nvs_open(const char *namespace_name, int mode, nvs_handle_t *handle)
{
    (void)namespace_name; (void)mode;
    if (g_open_fails) return ESP_FAIL;
    *handle = 1;
    return ESP_OK;
}
esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *out, size_t *length)
{
    (void)handle; (void)key;
    if (!g_present) return ESP_ERR_NVS_NOT_FOUND;
    size_t needed = strlen(g_value) + 1;
    if (*length < needed) return ESP_FAIL;
    memcpy(out, g_value, needed);
    *length = needed;
    return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value)
{
    (void)handle; (void)key;
    snprintf(g_value, sizeof(g_value), "%s", value);
    g_present = true;
    return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    (void)handle; (void)key;
    if (!g_present) return ESP_ERR_NVS_NOT_FOUND;
    g_present = false;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    g_commits++;
    return g_commit_fails ? ESP_FAIL : ESP_OK;
}
void nvs_close(nvs_handle_t handle) { (void)handle; }

#include "../../../waveshare/components/p4_shared/creds.c"

int main(void)
{
    char out[16];
    assert(!creds_get_stored("wifi", out, sizeof(out)));
    assert(out[0] == '\0');

    assert(!creds_get("wifi", out, sizeof(out), "fallback"));
    assert(strcmp(out, "fallback") == 0);
    assert(!creds_get("wifi", out, 5, "fallback"));
    assert(strcmp(out, "fall") == 0);

    assert(creds_set("wifi", "stored"));
    assert(g_commits == 1);
    assert(creds_get_stored("wifi", out, sizeof(out)));
    assert(strcmp(out, "stored") == 0);
    assert(creds_get("wifi", out, sizeof(out), "fallback"));
    assert(strcmp(out, "stored") == 0);
    assert(creds_is_set("wifi"));

    assert(creds_set("wifi", ""));
    assert(!g_present);
    assert(creds_set("wifi", NULL));
    assert(!creds_set(NULL, "value"));
    assert(!creds_set("", "value"));

    g_open_fails = true;
    assert(!creds_set("wifi", "value"));
    assert(!creds_get_stored("wifi", out, sizeof(out)));
    g_open_fails = false;

    g_commit_fails = true;
    assert(!creds_set("wifi", "value"));

    return 0;
}
