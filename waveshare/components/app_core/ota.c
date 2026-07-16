/* app_core/ota.c -- shared over-the-air update runner for both waveshare P4
 * builds. The firmware URL and the "update now" trigger come from each build's
 * SETUP tab (a creds value + ui_request_ota()); this file does the transfer,
 * verifies against the embedded cert bundle, writes the inactive OTA slot, and
 * reboots. Status text is surfaced on-screen through ui_set_ota_status(). */

#include "app_core_ota.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

#include "ui.h"   /* ui_set_ota_status() -- p4_shared is an app_core dependency */

static const char *TAG = "ota";

static volatile bool s_running = false;
static char          s_url[300];

bool app_core_ota_in_progress(void) { return s_running; }

static void ota_status(const char *msg)
{
    ESP_LOGI(TAG, "%s", msg);
    ui_set_ota_status(msg);
}

static void ota_task(void *arg)
{
    (void)arg;
    char msg[80];

    esp_http_client_config_t http = {
        .url               = s_url,
        .crt_bundle_attach = esp_crt_bundle_attach,   /* verify https */
        .timeout_ms        = 15000,
        .keep_alive_enable = true,
        .buffer_size       = 4096,
        .buffer_size_tx    = 2048,
    };
    esp_https_ota_config_t cfg = { .http_config = &http };

    ota_status("Update: connecting...");
    esp_https_ota_handle_t h = NULL;
    esp_err_t err = esp_https_ota_begin(&cfg, &h);
    if (err != ESP_OK || !h) {
        snprintf(msg, sizeof msg, "Update failed: %s", esp_err_to_name(err));
        ota_status(msg);
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    int total = esp_https_ota_get_image_size(h);
    int last_pct = -1;
    while (1) {
        err = esp_https_ota_perform(h);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;
        int got = esp_https_ota_get_image_len_read(h);
        int pct = (total > 0) ? (int)((int64_t)got * 100 / total) : 0;
        if (pct != last_pct && (pct % 5) == 0) {
            last_pct = pct;
            snprintf(msg, sizeof msg, "Updating... %d%%", pct);
            ota_status(msg);
        }
    }

    if (err == ESP_OK && esp_https_ota_is_complete_data_received(h)) {
        err = esp_https_ota_finish(h);
        if (err == ESP_OK) {
            ota_status("Update complete -- restarting");
            vTaskDelay(pdMS_TO_TICKS(1500));
            esp_restart();               /* boots the freshly written slot */
        }
        snprintf(msg, sizeof msg, "Update finalize failed: %s", esp_err_to_name(err));
        ota_status(msg);
    } else {
        esp_https_ota_abort(h);
        snprintf(msg, sizeof msg, "Update failed: %s", esp_err_to_name(err));
        ota_status(msg);
    }

    s_running = false;
    vTaskDelete(NULL);
}

bool app_core_ota_start(const char *url)
{
    if (s_running) return false;
    if (!url || !url[0]) {
        ui_set_ota_status("No firmware URL set (Settings > SETUP)");
        return false;
    }
    strlcpy(s_url, url, sizeof s_url);
    s_running = true;
    /* 8 KB is the documented OTA-over-TLS stack; the transient allocation is
     * fine on the rare, user-triggered update (the 64 KB SDIO DMA reserve is
     * separate and untouched). */
    if (xTaskCreate(ota_task, "ota", 8192, NULL, 4, NULL) != pdPASS) {
        s_running = false;
        ui_set_ota_status("Update failed: out of memory");
        return false;
    }
    return true;
}
