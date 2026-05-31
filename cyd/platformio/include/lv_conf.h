/*
 * LVGL configuration for the CYD Arduino build.
 *
 * Only overrides that differ from LVGL's built-in defaults are set here;
 * lv_conf_internal.h supplies defaults for everything else. Found via the
 * project include/ dir + the -D LV_CONF_INCLUDE_SIMPLE build flag.
 */

#ifndef LV_CONF_H
#define LV_CONF_H

/* No #includes here: lv_conf.h is also pulled in while preprocessing LVGL's
 * ARM SIMD .S assembly files, where C headers (stdint.h) would be parsed as
 * assembly and fail. */

/* 16-bit RGB565 to match the ILI9341 panel. */
#define LV_COLOR_DEPTH 16

/* Use the C library heap (shared with the rest of the app) rather than a
 * fixed LVGL pool, so LVGL competes for the same heap as WiFi/TLS instead of
 * permanently reserving a block. */
#define LV_USE_STDLIB_MALLOC    LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING    LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_CLIB

/* Tick is provided by lv_tick_set_cb(millis) in the display glue. */

/* Software renderer (default), no GPU. */
#define LV_USE_DRAW_SW 1

/* Logging off in normal builds. */
#define LV_USE_LOG 0

/* Fonts used by ui.cpp. The FontAwesome symbol glyphs (LV_SYMBOL_UP/DOWN) are
 * baked into these Montserrat fonts. */
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* Widgets used: label, image, bar (all default-on, listed for clarity). */
#define LV_USE_LABEL 1
#define LV_USE_IMAGE 1
#define LV_USE_BAR   1

/* Screen-load slide animations (lv_screen_load_anim) need the animation
 * subsystem, which is on by default. */

#endif /* LV_CONF_H */
