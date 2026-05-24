/*
 * Music Controller -- ESP32-P4 (Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3),
 * DIRECT SPOTIFY backend (no Home Assistant).
 *
 * STATUS: checkpoint-1 skeleton -- display bring-up only. NOT hardware-tested,
 * NOT compile-checked here. See README.md for the checkpoint roadmap and the
 * required one-time BSP vendoring step.
 *
 * The board BSP (vendored at ../components/esp32_p4_wifi6_touch_lcd_4_3) brings
 * up the ST7701 MIPI-DSI panel + GT911 capacitive touch + LVGL (via
 * esp_lvgl_adapter). We render landscape 800x480 (native panel is 480x800
 * portrait) using the adapter's rotation flag, and triple partial buffers in
 * PSRAM for tear-free scrolling.
 *
 * The Spotify backend (spotify.c), album list (albums.c), embedded thumbnails
 * (album_thumbs.c), JPEG art decode (album_art.cpp) and LittleFS scratch
 * (littlefs.c) are copied unchanged from cyd/esp-idf/ -- they are
 * board-agnostic. The UI (ui.c) and input are added in later checkpoints
 * (ui.c needs lvgl_port_lock -> bsp_display_lock and an 800x480 re-layout).
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "Music Controller P4 (direct Spotify) -- checkpoint 1 skeleton");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* ST7701 DSI + GT911 + LVGL. ROTATE_90 -> landscape 800x480 (flip to
     * ROTATE_270 if it comes up upside-down; if touch is then mis-aligned,
     * adjust touch_flags swap_xy/mirror_x/mirror_y to match). */
    bsp_display_cfg_t cfg = {
        .lv_adapter_cfg  = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation        = ESP_LV_ADAPTER_ROTATE_90,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL,
        .touch_flags     = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
    };
    bsp_display_start_with_config(&cfg);
    bsp_display_backlight_on();

    /* All LVGL access goes under the BSP lock (esp_lvgl_adapter's mutex). When
     * ui.c is ported in checkpoint 4, map its lvgl_port_lock(0)/unlock() to
     * bsp_display_lock(0)/bsp_display_unlock(). */
    bsp_display_lock(-1);
    lv_obj_t *label = lv_label_create(lv_screen_active());
    lv_label_set_text(label, "Music Controller P4\ncheckpoint 1: display OK");
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    bsp_display_unlock();

    /* Next checkpoints (see README.md):
     *  2  WiFi  : esp_wifi_remote (slave esp32c6) + port wifi_init_sta from cyd/esp-idf/main.c
     *  3  Spotify: spotify_init/spotify_fetch_player on a task + scmd_t command queue (port from cyd main.c)
     *  4  UI    : port cyd ui.c (lvgl_port_lock -> bsp_display_lock); re-lay-out constants for 800x480
     *  5  Assets: regen album_thumbs.bin larger via scripts/embed_albums_idf.py; bigger now-playing art + fonts
     *  6  Touch controls: on-screen prev/play-pause/next + volume slider -> ui_request_*()
     *  7  Parity: WiFi-strength indicator, volume HUD, progress bar, view toggle; leave a seam for physical input
     */
}
