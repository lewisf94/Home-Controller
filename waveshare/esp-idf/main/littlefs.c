/*
 * LittleFS mount for the CYD -- internal flash storage.
 *
 * The CYD's SD socket is wired to SPI3_HOST which is also used by the
 * XPT2046 touch controller, and the ESP32 only has two hardware SPI DMA
 * channels (both consumed by the LCD and LVGL framebuffer). Rather than
 * juggle three SPI peripherals across two buses, we store nowplaying.jpg
 * in a 256 KB LittleFS partition carved from the 4 MB internal flash.
 * No external hardware needed, no DMA conflicts, POSIX fopen/fread works
 * identically to SD so JPEGDEC's openFile callbacks need no changes.
 */

#include "littlefs.h"

#include "esp_littlefs.h"
#include "esp_log.h"

static const char *TAG = "littlefs";

#define MOUNT_POINT      "/littlefs"
#define PARTITION_LABEL  "storage"

static bool s_mounted = false;

bool littlefs_mount(void)
{
    if (s_mounted) return true;

    esp_vfs_littlefs_conf_t conf = {
        .base_path             = MOUNT_POINT,
        .partition_label       = PARTITION_LABEL,
        .format_if_mount_failed = true,
        .dont_mount            = false,
    };

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_vfs_littlefs_register failed: %s", esp_err_to_name(err));
        return false;
    }

    s_mounted = true;

    size_t total = 0, used = 0;
    esp_err_t info_err = esp_littlefs_info(PARTITION_LABEL, &total, &used);
    if (info_err == ESP_OK) {
        ESP_LOGI(TAG, "mounted at %s (%u KB total, %u KB used)",
                 MOUNT_POINT, (unsigned)(total / 1024), (unsigned)(used / 1024));
    } else {
        /* Distinguish "info call failed" from a real empty filesystem (which
         * would also print 0/0) so storage problems don't masquerade as a
         * freshly-mounted partition. */
        ESP_LOGW(TAG, "mounted at %s, but esp_littlefs_info failed: %s",
                 MOUNT_POINT, esp_err_to_name(info_err));
    }
    return true;
}

bool littlefs_is_mounted(void)
{
    return s_mounted;
}
