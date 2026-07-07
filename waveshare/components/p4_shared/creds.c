#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "creds";

#define NVS_NS "creds"
#define CREDS_VAL_MAX 300

bool creds_get_stored(const char *key, char *out, size_t out_len)
{
    if (out && out_len) out[0] = '\0';
    if (!key || !out || out_len == 0) return false;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = out_len;
    esp_err_t err = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    if (err != ESP_OK || !out[0]) {
        out[0] = '\0';
        return false;
    }
    return true;
}

bool creds_get(const char *key, char *out, size_t out_len, const char *fallback)
{
    if (creds_get_stored(key, out, out_len)) return true;
    if (out && out_len) {
        if (fallback) {
            strlcpy(out, fallback, out_len);
        } else {
            out[0] = '\0';
        }
    }
    return false;
}

bool creds_is_set(const char *key)
{
    char tmp[CREDS_VAL_MAX];
    return creds_get_stored(key, tmp, sizeof(tmp));
}

bool creds_set(const char *key, const char *value)
{
    if (!key || !key[0]) return false;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return false;
    }
    if (!value || !value[0]) {
        err = nvs_erase_key(h, key);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;   /* already clear */
    } else {
        err = nvs_set_str(h, key, value);
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "store '%s' failed: %s", key, esp_err_to_name(err));
        return false;
    }
    /* Log the KEY and set/cleared state only -- never the value. */
    ESP_LOGI(TAG, "'%s' %s", key, (value && value[0]) ? "set" : "cleared");
    return true;
}
