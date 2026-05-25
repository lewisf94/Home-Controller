/*
 * LVGL UI implementation -- see ui.h for the high-level contract.
 *
 * Ported from cyd/esp-idf/main/ui.c and re-laid-out for the Waveshare
 * 800x480 landscape panel. Two differences from the CYD build:
 *   - LVGL lock is the BSP adapter mutex: bsp_display_lock(-1) /
 *     bsp_display_unlock() (CYD used lvgl_port_lock(0)/unlock). -1
 *     (UINT32_MAX ms) blocks until free -- matches main.c's proven usage;
 *     0's meaning in esp_lv_adapter_lock is undocumented so we avoid it.
 *   - Fonts bumped for the bigger screen (montserrat 20/24/28; the config
 *     enables 16/20/24/28, not 12).
 *
 * Layout (800x480 landscape):
 *
 *   Browser screen
 *     y= 40..284  : horizontal scroll carousel (244 px tall)
 *                   each card 220x220 (matches the baked album_thumbs.bin size).
 *     y=298       : album title (montserrat 28, white)
 *     y=340       : artist  (montserrat 24, light grey)
 *     bottom      : "^ now playing" hint
 *
 *   Now-playing screen
 *     y=  6       : "v albums" hint (swipe down = back, left = next, right = prev)
 *     y= 48..368  : 320x320 album art (Spotify 640px JPEG decoded /2), centred
 *                   tap anywhere on screen = play/pause
 *     y=380       : track title (montserrat 28)
 *     y=418       : artist (montserrat 24, dimmer)
 *     y=452       : 520x12 progress bar (drag to seek)
 *
 * Local progress simulation: an LVGL timer ticks every 200 ms and adds
 * 200 ms to the cached progress_ms when is_playing is true, so the bar
 * advances smoothly between the 5-second Spotify polls. The poll
 * overwrites the cached value (re-syncs with the server) every cycle.
 */

#include "ui.h"

#include "albums.h"
#include "album_thumbs.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "bsp/esp-bsp.h"
#include "spotify.h"
#include "nvs.h"

#include <string.h>

static const char *TAG = "ui";

#define SCREEN_W 800
#define SCREEN_H 480

#define CARD_SIZE     220
#define CARD_GAP       28
#define SCROLLER_Y     40
#define SCROLLER_H    244

#define ART_W         320
#define ART_H         320
#define ART_X          ((SCREEN_W - ART_W) / 2)
#define ART_Y          48

#define PROG_W        520
#define PROG_H         12
#define PROG_X         ((SCREEN_W - PROG_W) / 2)
#define PROG_Y        452

/* Generous touch band around the thin progress bar: a drag that starts here is
 * a scrub, not a screen swipe. Wider than the bar so 0%/100% are easy to grab. */
#define SEEK_OV_W     (PROG_W + 80)
#define SEEK_OV_H      72
#define SEEK_OV_X     ((SCREEN_W - SEEK_OV_W) / 2)
#define SEEK_OV_Y     (PROG_Y + PROG_H / 2 - SEEK_OV_H / 2)

#define NP_TITLE_Y    380
#define NP_ARTIST_Y   418

#define BR_TITLE_Y    298
#define BR_ARTIST_Y   340


/* Runtime TTF fonts -- created once from embedded flash blobs, shared across
 * all screen builds (title/artist labels; hints keep the built-in bitmap font
 * because it carries the LVGL symbol glyphs). */
static lv_font_t *s_font_28 = NULL;   /* Montserrat 28 + DejaVu fallback */
static lv_font_t *s_font_24 = NULL;   /* Montserrat 24 + DejaVu fallback */
#define TTF_GLYPH_CACHE_CNT 128        /* matches LVGL's tiny_ttf default */

static lv_obj_t *s_screen_np      = NULL;
static lv_obj_t *s_screen_browser = NULL;

static lv_obj_t *s_np_art      = NULL;
static lv_obj_t *s_np_title    = NULL;
static lv_obj_t *s_np_artist   = NULL;
static lv_obj_t *s_np_progress = NULL;
static lv_obj_t *s_vol_hud     = NULL;

static lv_timer_t *s_vol_hud_timer = NULL;
static bool        s_seeking        = false;

static lv_obj_t *s_browser_scroller = NULL;
static lv_obj_t *s_browser_title    = NULL;
static lv_obj_t *s_browser_artist   = NULL;
static lv_obj_t *s_wifi_bars[4]     = {0};

#define MAX_CARDS 64
static lv_obj_t       *s_cards[MAX_CARDS]    = {0};
static lv_obj_t       *s_card_imgs[MAX_CARDS] = {0};  /* child lv_image per card */
static lv_image_dsc_t  s_card_dscs[MAX_CARDS] = {0};
static size_t          s_card_count          = 0;
static int             s_centered_card       = -1;
/* Logical target card for encoder scrolling. Tracked independently of the
 * live (possibly mid-animation) scroll position so fast spins don't lose
 * detents. Re-synced to the visually centered card on touch-driven scrolls. */
static int             s_target_card         = 0;

static lv_image_dsc_t *s_art_dsc = NULL;

/* Default to instant: the animated full-screen composite (lv_screen_load_anim)
 * is the heaviest draw the UI does and stalls under the DSI triple-partial
 * flush -- the freeze always struck on a swipe transition, never in steady
 * state. NONE skips the composite. The settings screen lets the user pick an
 * animated style (loaded from NVS at boot); NONE stays the safe default. */
static ui_transition_t s_transition = UI_TRANSITION_NONE;

/* lv_tick at which the in-flight transition animation finishes. While a screen
 * load animation is running, a second lv_screen_load_anim can stall the render
 * task, so load_screen ignores new animated requests until this passes. */
static uint32_t s_anim_block_until = 0;

/* Mode palette (neutrals only). Stored as 0xRRGGBB so the table is a constant
 * initializer (lv_color_hex() is not a constant expression). s_th points at the
 * active palette; build_*_screen reads it, so switching mode is a pointer swap +
 * rebuild. The accent (selection highlight + progress) is a separate setting --
 * see k_accents / accent_color(). */
typedef struct {
    uint32_t bg;       /* screen background */
    uint32_t surface;  /* cards / buttons / scroller fill */
    uint32_t text;     /* primary text (titles, progress indicator) */
    uint32_t text2;    /* secondary text (artist, section + button labels) */
    uint32_t dim;      /* hints */
    uint32_t track;    /* progress-bar track */
} theme_t;

/* Charcoal palette: dark grey (not pure black) + off-white text + neutral
 * greys, with a saturated orange primary accent and a red secondary accent.
 * Light theme is a warm off-white variant sharing the same accents.
 * { bg, surface, text, text2, dim, track }. */
static const theme_t THEME_DARK  = { 0x121212, 0x1E1E1E, 0xFAFAFA, 0x9A9A9A, 0x5E5E5E, 0x2C2C2C };
static const theme_t THEME_LIGHT = { 0xECEAE6, 0xDAD6CF, 0x1A1A1A, 0x57534C, 0x8C877E, 0xC6C1B8 };
static const theme_t *s_th = &THEME_DARK;

/* Light/Dark MODE (neutrals) is one setting; COLOUR THEME (accent) is a
 * separate setting that overlays a single accent on either mode. Accents are
 * mid-tone saturated hues chosen to read on both the charcoal and cream
 * backgrounds (no pale/yellow). One accent drives selection highlights AND the
 * live elements (progress bar). */
enum { THEME_DARK_IDX = 0, THEME_LIGHT_IDX = 1, THEME_COUNT = 2 };
static uint8_t s_theme = THEME_DARK_IDX;   /* light/dark mode */

enum { ACCENT_ORANGE = 0, ACCENT_RED, ACCENT_GREEN, ACCENT_PURPLE, ACCENT_COUNT };
static uint8_t s_accent = ACCENT_ORANGE;
static const uint32_t k_accents[ACCENT_COUNT] = { 0xFF5A00, 0xE0301E, 0x2FB344, 0x8B5CF6 };
static const char *const k_accent_names[ACCENT_COUNT] = { "ORANGE", "RED", "GREEN", "PURPLE" };
static uint32_t accent_color(void) { return k_accents[s_accent]; }

/* Browser styles (all use uniform scale + opacity -- see apply_card_transforms
 * for why skew/matrix transforms were reverted):
 *  - CAROUSEL: flat row, all cards same size (the original).
 *  - FOCUS:    centre card full size, side cards scale down + dim gently.
 *  - COVERFLOW: side covers shrink harder + dim more, so they recede strongly
 *               from the centre (depth via scale, not a true 3D tilt).
 * NVS persists the index; the old build's "Cover Flow" (index 1) is now FOCUS,
 * which is the correct migration -- index 1 was always the scale+dim mode. */
enum { BROWSER_CAROUSEL = 0, BROWSER_FOCUS = 1, BROWSER_COVERFLOW = 2,
       BROWSER_STYLE_COUNT = 3 };
static uint8_t s_browser_style = BROWSER_CAROUSEL;

/* True for any style that transforms cards per scroll position (Focus + CF). */
#define BROWSER_STYLE_TRANSFORMS(s) ((s) == BROWSER_FOCUS || (s) == BROWSER_COVERFLOW)

/* Settings screen + its option rows (transition + mode + colour + browser). */
static lv_obj_t *s_screen_settings = NULL;
static lv_obj_t *s_opt_btns[UI_TRANSITION_COUNT]          = {0};
static lv_obj_t *s_opt_labels[UI_TRANSITION_COUNT]        = {0};
static lv_obj_t *s_theme_btns[THEME_COUNT]                = {0};
static lv_obj_t *s_theme_labels[THEME_COUNT]              = {0};
static lv_obj_t *s_accent_btns[ACCENT_COUNT]             = {0};
static lv_obj_t *s_accent_labels[ACCENT_COUNT]           = {0};
static lv_obj_t *s_brstyle_btns[BROWSER_STYLE_COUNT]      = {0};
static lv_obj_t *s_brstyle_labels[BROWSER_STYLE_COUNT]    = {0};

#define NVS_SETTINGS_NS       "settings"
#define NVS_KEY_TRANSITION    "transition"
#define NVS_KEY_THEME         "theme"
#define NVS_KEY_ACCENT        "accent"
#define NVS_KEY_BROWSER_STYLE "browser_style"

static const char *const k_transition_names[UI_TRANSITION_COUNT] = {
    "OVER (SLIDE)", "MOVE (PUSH)", "FADE", "NONE (INSTANT)",
};
static const char *const k_theme_names[THEME_COUNT] = { "DARK", "LIGHT" };
static const char *const k_browser_style_names[BROWSER_STYLE_COUNT] = { "CAROUSEL", "FOCUS", "COVER FLOW" };

/* Cached track state. The LVGL progress timer reads progress_ms /
 * duration_ms / is_playing from here and ticks the bar between
 * Spotify polls so the bar moves at ~5 Hz instead of 0.2 Hz. */
static spotify_track_t s_track = {0};

static void on_gesture(lv_event_t *e);
static void on_card_clicked(lv_event_t *e);
static void on_browser_scroll(lv_event_t *e);
static void progress_timer_cb(lv_timer_t *t);
static void update_progress_bar(void);
static void on_open_settings(lv_event_t *e);
static void on_settings_back(lv_event_t *e);
static void on_transition_option(lv_event_t *e);
static void on_theme_option(lv_event_t *e);
static void on_accent_option(lv_event_t *e);
static void on_np_tap(lv_event_t *e);
static void on_seek_start(lv_event_t *e);
static void on_seek_pressing(lv_event_t *e);
static void on_seek_released(lv_event_t *e);
static void on_seek_click_absorb(lv_event_t *e);
static void wifi_timer_cb(lv_timer_t *t);
static void refresh_settings_selection(void);
static void refresh_theme_selection(void);
static void refresh_accent_selection(void);
static void refresh_browser_style_selection(void);
static void apply_theme_cb(void *unused);
static void rebuild_browser_cb(void *unused);
static void apply_card_transforms(void);
static void load_settings(void);
static void save_transition(ui_transition_t style);
static void save_theme(uint8_t idx);
static void save_accent(uint8_t idx);
static void save_browser_style(uint8_t idx);
static void on_browser_style_option(lv_event_t *e);

static lv_color_t card_color(size_t i)
{
    /* Deterministic palette per album so users can still tell cards
     * apart without real album art. Lightened so the white initial
     * stays legible. */
    static const lv_palette_t palettes[] = {
        LV_PALETTE_RED,        LV_PALETTE_PINK,       LV_PALETTE_PURPLE,
        LV_PALETTE_DEEP_PURPLE, LV_PALETTE_INDIGO,    LV_PALETTE_BLUE,
        LV_PALETTE_LIGHT_BLUE, LV_PALETTE_CYAN,       LV_PALETTE_TEAL,
        LV_PALETTE_GREEN,      LV_PALETTE_LIGHT_GREEN, LV_PALETTE_LIME,
        LV_PALETTE_YELLOW,     LV_PALETTE_AMBER,      LV_PALETTE_ORANGE,
        LV_PALETTE_DEEP_ORANGE, LV_PALETTE_BROWN,     LV_PALETTE_BLUE_GREY,
    };
    size_t n = sizeof(palettes) / sizeof(palettes[0]);
    return lv_palette_darken(palettes[i % n], 1);
}

/* All browser styles use the same card slot size + gap; Focus and Cover Flow
 * differ only by the per-card transform applied at scroll time, not the layout
 * footprint (so the 220px card always fits the 244px scroller -- no clipping). */
static int cs(void) { return CARD_SIZE; }
static int cg(void) { return CARD_GAP; }

static void style_label(lv_obj_t *label, const lv_font_t *font,
                        lv_color_t color, int16_t y)
{
    lv_label_set_text(label, "");
    lv_obj_set_width(label, SCREEN_W);
    /* Pin to a single line's height so LONG_DOT ellipsises overflow instead of
     * wrapping to a second line (which would grow down over the label below). */
    lv_obj_set_height(label, lv_font_get_line_height(font));
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, y);
}

static void build_browser_screen(void)
{
    s_screen_browser = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen_browser, lv_color_hex(s_th->bg), 0);
    lv_obj_set_style_bg_opa(s_screen_browser, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_screen_browser, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_screen_browser, on_gesture, LV_EVENT_GESTURE, NULL);

    s_browser_scroller = lv_obj_create(s_screen_browser);
    lv_obj_set_size(s_browser_scroller, SCREEN_W, SCROLLER_H);
    lv_obj_set_pos(s_browser_scroller, 0, SCROLLER_Y);
    lv_obj_set_style_bg_color(s_browser_scroller, lv_color_hex(s_th->bg), 0);
    lv_obj_set_style_bg_opa(s_browser_scroller, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_browser_scroller, 0, 0);
    lv_obj_set_style_pad_top(s_browser_scroller, 0, 0);
    lv_obj_set_style_pad_bottom(s_browser_scroller, 0, 0);
    /* Pad left/right so the first and last cards can fully snap to centre. */
    lv_obj_set_style_pad_left (s_browser_scroller, (SCREEN_W - cs()) / 2, 0);
    lv_obj_set_style_pad_right(s_browser_scroller, (SCREEN_W - cs()) / 2, 0);
    lv_obj_set_style_pad_column(s_browser_scroller, cg(), 0);
    lv_obj_set_flex_flow(s_browser_scroller, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_browser_scroller, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(s_browser_scroller, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(s_browser_scroller, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_snap_x(s_browser_scroller, LV_SCROLL_SNAP_CENTER);
    lv_obj_add_event_cb(s_browser_scroller, on_browser_scroll, LV_EVENT_SCROLL, NULL);

    s_card_count = albums_count();
    if (s_card_count > MAX_CARDS) s_card_count = MAX_CARDS;

    /* Debug: print thumbnail / album counts and a few thumb pointers to help
     * diagnose missing/corrupt embedded blobs when Cover Flow shows blank cards. */
    ESP_LOGI(TAG, "albums_count=%zu s_card_count=%zu album_thumb_count=%zu ALBUM_THUMB=%dx%d browser_style=%s",
             albums_count(), s_card_count, album_thumb_count(), (int)ALBUM_THUMB_W, (int)ALBUM_THUMB_H,
             k_browser_style_names[s_browser_style]);
    for (size_t __j = 0; __j < (s_card_count < 4 ? s_card_count : 4); __j++) {
        const uint16_t *__t = album_thumb_data(__j);
        ESP_LOGI(TAG, "thumb[%zu]=%p", __j, (const void *)__t);
    }

    for (size_t i = 0; i < s_card_count; i++) {
        const album_entry_t *a    = albums_get(i);
        const uint16_t      *thumb = album_thumb_data(i);

        lv_obj_t *card = lv_obj_create(s_browser_scroller);
        lv_obj_set_size(card, cs(), cs());
        lv_obj_set_style_radius(card, 0, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_set_style_bg_color(card, lv_color_hex(s_th->bg), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, on_card_clicked, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);

        if (thumb) {
            /* Artwork as a child lv_image. Focus/Cover Flow scale + dim the
             * IMAGE directly (lv_image_set_scale + image_recolor) rather than
             * transforming the card object. Per LVGL docs, image transforms
             * draw without an intermediate layer snapshot -- whereas setting
             * transform_scale/opa on an lv_obj forces a per-frame layer that
             * this board's DIRECT-mode rotated DSI flush mis-composited,
             * blacking out the off-centre cards. The card object carries no
             * transform, so no layer is ever created. */
            s_card_dscs[i].header.cf  = LV_COLOR_FORMAT_RGB565;
            s_card_dscs[i].header.w   = ALBUM_THUMB_W;
            s_card_dscs[i].header.h   = ALBUM_THUMB_H;
            s_card_dscs[i].data       = (const uint8_t *)thumb;
            s_card_dscs[i].data_size  = ALBUM_THUMB_BYTES;
            lv_obj_t *img = lv_image_create(card);
            lv_image_set_src(img, &s_card_dscs[i]);
            lv_obj_center(img);
            /* Scale about the image centre so side covers shrink inward. */
            lv_image_set_pivot(img, ALBUM_THUMB_W / 2, ALBUM_THUMB_H / 2);
            lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
            s_card_imgs[i] = img;
        } else {
            /* No embedded thumb (shouldn't happen with the generated
             * blob, but fall back gracefully): coloured square with
             * first-letter initial. */
            lv_obj_set_style_bg_color(card, card_color(i), 0);
            lv_obj_t *letter = lv_label_create(card);
            char ini[2] = { a->title[0] ? a->title[0] : '?', '\0' };
            lv_label_set_text(letter, ini);
            lv_obj_set_style_text_color(letter, lv_color_white(), 0);
            lv_obj_set_style_text_font(letter, &lv_font_montserrat_28, 0);
            lv_obj_center(letter);
        }

        s_cards[i] = card;
    }

    s_browser_title = lv_label_create(s_screen_browser);
    style_label(s_browser_title,
                s_font_28 ? s_font_28 : &lv_font_montserrat_28,
                lv_color_hex(s_th->text), BR_TITLE_Y);

    s_browser_artist = lv_label_create(s_screen_browser);
    style_label(s_browser_artist,
                s_font_24 ? s_font_24 : &lv_font_montserrat_24,
                lv_color_hex(s_th->text2), BR_ARTIST_Y);

    if (s_card_count > 0) {
        const album_entry_t *a = albums_get(0);
        lv_label_set_text(s_browser_title, a->title);
        lv_label_set_text(s_browser_artist, a->artist);
        s_centered_card = 0;
    }

    /* Apply initial cover flow scales (index-based, scroll_x=0 = card 0 centred). */
    apply_card_transforms();

    /* WiFi-strength indicator: four rising bars at top-left. */
    for (int i = 0; i < 4; i++) {
        int h = 6 + i * 4;   /* 6, 10, 14, 18 px tall */
        lv_obj_t *bar = lv_obj_create(s_screen_browser);
        lv_obj_set_size(bar, 5, h);
        lv_obj_set_pos(bar, 6 + i * 8, 22 - h);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_radius(bar, 0, 0);
        lv_obj_set_style_pad_all(bar, 0, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(s_th->track), 0);
        s_wifi_bars[i] = bar;
    }

    /* "^ now playing" hint at the bottom edge. */
    lv_obj_t *hint = lv_label_create(s_screen_browser);
    lv_label_set_text(hint, LV_SYMBOL_UP " now playing");
    lv_obj_set_style_text_color(hint, lv_color_hex(s_th->dim), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_20, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -6);

    /* Gear button (top-right) -> settings. Sits in the empty strip above the
     * carousel so it never overlaps a card. */
    lv_obj_t *gear = lv_button_create(s_screen_browser);
    lv_obj_set_size(gear, 44, 36);
    lv_obj_align(gear, LV_ALIGN_TOP_RIGHT, -6, 4);
    lv_obj_set_style_bg_color(gear, lv_color_hex(s_th->surface), 0);
    lv_obj_set_style_bg_opa(gear, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(gear, 6, 0);
    lv_obj_add_event_cb(gear, on_open_settings, LV_EVENT_CLICKED, NULL);
    lv_obj_t *gear_lbl = lv_label_create(gear);
    lv_label_set_text(gear_lbl, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_color(gear_lbl, lv_color_hex(s_th->text2), 0);
    lv_obj_set_style_text_font(gear_lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(gear_lbl);
}

static void build_np_screen(void)
{
    s_screen_np = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen_np, lv_color_hex(s_th->bg), 0);
    lv_obj_set_style_bg_opa(s_screen_np, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_screen_np, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_screen_np, on_gesture, LV_EVENT_GESTURE, NULL);

    /* "v albums" hint at the top so the user knows the swipe-down gesture. */
    lv_obj_t *hint = lv_label_create(s_screen_np);
    lv_label_set_text(hint, LV_SYMBOL_DOWN " albums");
    lv_obj_set_style_text_color(hint, lv_color_hex(s_th->dim), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_20, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 6);

    /* Tap anywhere on the screen to play/pause. The art widget would intercept
     * taps before they reach the screen, so mark it non-clickable. */
    lv_obj_add_event_cb(s_screen_np, on_np_tap, LV_EVENT_CLICKED, NULL);

    s_np_art = lv_image_create(s_screen_np);
    lv_obj_set_size(s_np_art, ART_W, ART_H);
    lv_obj_set_pos(s_np_art, ART_X, ART_Y);
    lv_obj_remove_flag(s_np_art, LV_OBJ_FLAG_CLICKABLE);
    if (s_art_dsc && s_art_dsc->data) {
        lv_image_set_src(s_np_art, s_art_dsc);
    }

    s_np_title = lv_label_create(s_screen_np);
    style_label(s_np_title,
                s_font_28 ? s_font_28 : &lv_font_montserrat_28,
                lv_color_hex(s_th->text), NP_TITLE_Y);

    s_np_artist = lv_label_create(s_screen_np);
    style_label(s_np_artist,
                s_font_24 ? s_font_24 : &lv_font_montserrat_24,
                lv_color_hex(s_th->text2), NP_ARTIST_Y);

    s_np_progress = lv_bar_create(s_screen_np);
    lv_obj_set_size(s_np_progress, PROG_W, PROG_H);
    lv_obj_set_pos(s_np_progress, PROG_X, PROG_Y);
    lv_bar_set_range(s_np_progress, 0, 1000);
    lv_bar_set_value(s_np_progress, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_np_progress, lv_color_hex(s_th->track), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_np_progress, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_np_progress, lv_color_hex(accent_color()), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_np_progress, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_np_progress, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(s_np_progress, 2, LV_PART_INDICATOR);
    lv_obj_remove_flag(s_np_progress, LV_OBJ_FLAG_CLICKABLE);

    /* Transparent touch overlay on top of the bar. Reads raw finger X to compute
     * seek position and drives lv_bar_set_value directly during drag. The bar
     * stays the visual source of truth and tracks song timing when not seeking. */
    lv_obj_t *seek_ov = lv_obj_create(s_screen_np);
    lv_obj_set_size(seek_ov, SEEK_OV_W, SEEK_OV_H);
    lv_obj_set_pos(seek_ov, SEEK_OV_X, SEEK_OV_Y);
    lv_obj_set_style_bg_opa(seek_ov, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(seek_ov, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(seek_ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(seek_ov, on_seek_start,        LV_EVENT_PRESSED,   NULL);
    lv_obj_add_event_cb(seek_ov, on_seek_pressing,     LV_EVENT_PRESSING,  NULL);
    /* Clear s_seeking + send the seek on BOTH release and press-lost, so a press
     * that gets reclassified mid-drag can never leave s_seeking stuck true. */
    lv_obj_add_event_cb(seek_ov, on_seek_released,     LV_EVENT_RELEASED,  NULL);
    lv_obj_add_event_cb(seek_ov, on_seek_released,     LV_EVENT_PRESS_LOST, NULL);
    /* Absorb CLICKED so it doesn't bubble to on_np_tap (play/pause). */
    lv_obj_add_event_cb(seek_ov, on_seek_click_absorb, LV_EVENT_CLICKED,   NULL);

    s_vol_hud = lv_label_create(s_screen_np);
    lv_label_set_text(s_vol_hud, "");
    lv_obj_set_style_text_color(s_vol_hud, lv_color_hex(0xFF4040), 0);
    lv_obj_set_style_text_font(s_vol_hud, &lv_font_montserrat_24, 0);
    lv_obj_align(s_vol_hud, LV_ALIGN_TOP_RIGHT, -8, 6);
    lv_obj_add_flag(s_vol_hud, LV_OBJ_FLAG_HIDDEN);
}

/* Highlight the active row by accent fill + black text (no checkmark -- the
 * fill is the indicator; cleaner, and keeps uppercase labels inside the
 * narrower buttons). Same pattern for transition / mode / browser rows. */
static void refresh_settings_selection(void)
{
    for (int i = 0; i < UI_TRANSITION_COUNT; i++) {
        if (!s_opt_btns[i] || !s_opt_labels[i]) continue;
        bool sel = (i == (int)s_transition);
        lv_obj_set_style_bg_color(s_opt_btns[i],
            sel ? lv_color_hex(accent_color()) : lv_color_hex(s_th->surface), 0);
        lv_obj_set_style_text_color(s_opt_labels[i],
            sel ? lv_color_black() : lv_color_hex(s_th->text2), 0);
        lv_label_set_text(s_opt_labels[i], k_transition_names[i]);
    }
}

static void refresh_theme_selection(void)
{
    for (int i = 0; i < THEME_COUNT; i++) {
        if (!s_theme_btns[i] || !s_theme_labels[i]) continue;
        bool sel = (i == (int)s_theme);
        lv_obj_set_style_bg_color(s_theme_btns[i],
            sel ? lv_color_hex(accent_color()) : lv_color_hex(s_th->surface), 0);
        lv_obj_set_style_text_color(s_theme_labels[i],
            sel ? lv_color_black() : lv_color_hex(s_th->text2), 0);
        lv_label_set_text(s_theme_labels[i], k_theme_names[i]);
    }
}

/* Colour theme row. Each button is a swatch filled with its own accent colour
 * (so you can see the choices); the selected one gets a white ring + check. */
static void refresh_accent_selection(void)
{
    for (int i = 0; i < ACCENT_COUNT; i++) {
        if (!s_accent_btns[i] || !s_accent_labels[i]) continue;
        bool sel = (i == (int)s_accent);
        lv_obj_set_style_bg_color(s_accent_btns[i], lv_color_hex(k_accents[i]), 0);
        lv_obj_set_style_border_width(s_accent_btns[i], sel ? 3 : 0, 0);
        lv_obj_set_style_border_color(s_accent_btns[i], lv_color_white(), 0);
        lv_obj_set_style_text_color(s_accent_labels[i], lv_color_white(), 0);
        if (sel) {
            char buf[24];
            snprintf(buf, sizeof(buf), LV_SYMBOL_OK "  %s", k_accent_names[i]);
            lv_label_set_text(s_accent_labels[i], buf);
        } else {
            lv_label_set_text(s_accent_labels[i], k_accent_names[i]);
        }
    }
}

static void build_settings_screen(void)
{
    s_screen_settings = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen_settings, lv_color_hex(s_th->bg), 0);
    lv_obj_set_style_bg_opa(s_screen_settings, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(s_screen_settings, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *back = lv_button_create(s_screen_settings);
    lv_obj_set_size(back, 120, 44);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_obj_set_style_bg_color(back, lv_color_hex(s_th->surface), 0);
    lv_obj_set_style_radius(back, 3, 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_add_event_cb(back, on_settings_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lbl = lv_label_create(back);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT "  BACK");
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(s_th->text), 0);
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(back_lbl);

    lv_obj_t *title = lv_label_create(s_screen_settings);
    lv_label_set_text(title, "SETTINGS");
    lv_obj_set_style_text_color(title, lv_color_hex(s_th->text), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    /* Letter-spacing gives the uppercase title a cleaner, more deliberate feel. */
    lv_obj_set_style_text_letter_space(title, 3, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    lv_obj_t *section = lv_label_create(s_screen_settings);
    lv_label_set_text(section, "MENU TRANSITION");
    lv_obj_set_style_text_color(section, lv_color_hex(s_th->text2), 0);
    lv_obj_set_style_text_font(section, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_letter_space(section, 2, 0);
    lv_obj_align(section, LV_ALIGN_TOP_LEFT, 24, 66);

    for (int i = 0; i < UI_TRANSITION_COUNT; i++) {
        lv_obj_t *btn = lv_button_create(s_screen_settings);
        lv_obj_set_size(btn, 520, 48);
        lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, 102 + i * 54);
        lv_obj_set_style_radius(btn, 3, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, on_transition_option, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
        lv_obj_center(lbl);

        s_opt_btns[i]   = btn;
        s_opt_labels[i] = lbl;
    }

    lv_obj_t *th_section = lv_label_create(s_screen_settings);
    lv_label_set_text(th_section, "MODE");
    lv_obj_set_style_text_color(th_section, lv_color_hex(s_th->text2), 0);
    lv_obj_set_style_text_font(th_section, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_letter_space(th_section, 2, 0);
    lv_obj_align(th_section, LV_ALIGN_TOP_LEFT, 24, 324);

    /* Dark | Light side by side (a binary choice reads as a segmented pair). */
    for (int i = 0; i < THEME_COUNT; i++) {
        lv_obj_t *btn = lv_button_create(s_screen_settings);
        lv_obj_set_size(btn, 250, 48);
        lv_obj_align(btn, LV_ALIGN_TOP_MID, (i == 0) ? -134 : 134, 360);
        lv_obj_set_style_radius(btn, 3, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, on_theme_option, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
        lv_obj_center(lbl);

        s_theme_btns[i]   = btn;
        s_theme_labels[i] = lbl;
    }

    /* Colour theme -- accent swatches, independent of light/dark mode. */
    lv_obj_t *col_section = lv_label_create(s_screen_settings);
    lv_label_set_text(col_section, "COLOUR");
    lv_obj_set_style_text_color(col_section, lv_color_hex(s_th->text2), 0);
    lv_obj_set_style_text_font(col_section, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_letter_space(col_section, 2, 0);
    lv_obj_align(col_section, LV_ALIGN_TOP_LEFT, 24, 420);

    for (int i = 0; i < ACCENT_COUNT; i++) {
        lv_obj_t *btn = lv_button_create(s_screen_settings);
        lv_obj_set_size(btn, 168, 48);
        /* Centres of four 168px swatches across the row. */
        lv_obj_align(btn, LV_ALIGN_TOP_MID, (i * 176) - 264, 456);
        lv_obj_set_style_radius(btn, 3, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, on_accent_option, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        lv_obj_center(lbl);

        s_accent_btns[i]   = btn;
        s_accent_labels[i] = lbl;
    }

    lv_obj_t *br_section = lv_label_create(s_screen_settings);
    lv_label_set_text(br_section, "BROWSER STYLE");
    lv_obj_set_style_text_color(br_section, lv_color_hex(s_th->text2), 0);
    lv_obj_set_style_text_font(br_section, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_letter_space(br_section, 2, 0);
    lv_obj_align(br_section, LV_ALIGN_TOP_LEFT, 24, 516);

    /* Carousel | Focus | Cover Flow -- three buttons in a row. */
    for (int i = 0; i < BROWSER_STYLE_COUNT; i++) {
        lv_obj_t *btn = lv_button_create(s_screen_settings);
        lv_obj_set_size(btn, 170, 48);
        /* Centres of three 170px buttons across the 520px content width. */
        lv_obj_align(btn, LV_ALIGN_TOP_MID, (i - 1) * 176, 552);
        lv_obj_set_style_radius(btn, 3, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, on_browser_style_option, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);

        lv_obj_t *lbl = lv_label_create(btn);
        /* 20px (not 24): "Cover Flow" + a checkmark must fit the 170px button. */
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        lv_obj_center(lbl);

        s_brstyle_btns[i]   = btn;
        s_brstyle_labels[i] = lbl;
    }

    refresh_settings_selection();
    refresh_theme_selection();
    refresh_accent_selection();
    refresh_browser_style_selection();
}

static void on_open_settings(lv_event_t *e)
{
    (void)e;
    /* Settings enter/exit are always instant -- a utility screen, and it keeps
     * the (animated) transition styles off the entry path entirely. */
    if (s_screen_settings) lv_screen_load(s_screen_settings);
}

static void on_settings_back(lv_event_t *e)
{
    (void)e;
    if (s_screen_browser) lv_screen_load(s_screen_browser);
}

static void on_transition_option(lv_event_t *e)
{
    ui_transition_t style = (ui_transition_t)(uintptr_t)lv_event_get_user_data(e);
    ui_set_transition_style(style);
    save_transition(s_transition);
    refresh_settings_selection();
    ESP_LOGI(TAG, "transition style -> %s", k_transition_names[s_transition]);
}

static void on_theme_option(lv_event_t *e)
{
    uint8_t idx = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    if (idx >= THEME_COUNT || idx == s_theme) return;
    s_theme = idx;
    s_th    = (idx == THEME_LIGHT_IDX) ? &THEME_LIGHT : &THEME_DARK;
    save_theme(idx);
    ESP_LOGI(TAG, "theme -> %s", k_theme_names[idx]);
    /* Re-skin by rebuilding all three screens, but defer it: deleting the
     * active settings screen from inside its own button handler is unsafe. */
    lv_async_call(apply_theme_cb, NULL);
}

static void on_accent_option(lv_event_t *e)
{
    uint8_t idx = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    if (idx >= ACCENT_COUNT || idx == s_accent) return;
    s_accent = idx;
    save_accent(idx);
    ESP_LOGI(TAG, "accent -> %s", k_accent_names[idx]);
    /* Same rebuild path as a mode change: every screen re-reads accent_color()
     * for highlights + progress, so a full rebuild repaints the accent. */
    lv_async_call(apply_theme_cb, NULL);
}

static void apply_theme_cb(void *unused)
{
    (void)unused;
    lv_obj_t *active      = lv_screen_active();
    bool      was_np      = (active == s_screen_np);
    bool      was_setting = (active == s_screen_settings);
    int       saved_card  = s_centered_card;   /* preserve carousel position */

    lv_obj_t *old_browser  = s_screen_browser;
    lv_obj_t *old_np       = s_screen_np;
    lv_obj_t *old_settings = s_screen_settings;

    build_browser_screen();
    build_np_screen();
    build_settings_screen();

    /* Restore carousel position -- build always starts at card 0. Force the
     * layout so scroll bounds are computed before we set the offset. */
    if (saved_card > 0 && s_browser_scroller) {
        lv_obj_update_layout(s_browser_scroller);
        lv_obj_scroll_to_x(s_browser_scroller,
                           (int32_t)saved_card * (cs() + cg()),
                           LV_ANIM_OFF);
        s_centered_card = saved_card;
        s_target_card   = saved_card;
        const album_entry_t *a = albums_get((size_t)saved_card);
        if (a && s_browser_title && s_browser_artist) {
            lv_label_set_text(s_browser_title,  a->title);
            lv_label_set_text(s_browser_artist, a->artist);
        }
        apply_card_transforms();
    }

    /* Restore now-playing labels from cached track state. */
    if (s_track.title[0]  && s_np_title)  lv_label_set_text(s_np_title,  s_track.title);
    if (s_track.artist[0] && s_np_artist) lv_label_set_text(s_np_artist, s_track.artist);
    update_progress_bar();

    /* Activate the equivalent new screen first -- the active screen can't be
     * deleted -- then drop the old ones. */
    lv_screen_load(was_np ? s_screen_np : was_setting ? s_screen_settings : s_screen_browser);
    lv_obj_delete(old_browser);
    lv_obj_delete(old_np);
    lv_obj_delete(old_settings);
}

static void load_settings(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_SETTINGS_NS, NVS_READONLY, &h) != ESP_OK) return;  /* unset -> defaults */
    uint8_t v = UI_TRANSITION_NONE;
    if (nvs_get_u8(h, NVS_KEY_TRANSITION, &v) == ESP_OK && v < UI_TRANSITION_COUNT) {
        s_transition = (ui_transition_t)v;
    }
    uint8_t t = THEME_DARK_IDX;
    if (nvs_get_u8(h, NVS_KEY_THEME, &t) == ESP_OK && t < THEME_COUNT) {
        s_theme = t;
        s_th    = (t == THEME_LIGHT_IDX) ? &THEME_LIGHT : &THEME_DARK;
    }
    uint8_t ac = ACCENT_ORANGE;
    if (nvs_get_u8(h, NVS_KEY_ACCENT, &ac) == ESP_OK && ac < ACCENT_COUNT) {
        s_accent = ac;
    }
    uint8_t bs = BROWSER_CAROUSEL;
    if (nvs_get_u8(h, NVS_KEY_BROWSER_STYLE, &bs) == ESP_OK && bs < BROWSER_STYLE_COUNT) {
        s_browser_style = bs;
    }
    nvs_close(h);
}

static void save_transition(ui_transition_t style)
{
    nvs_handle_t h;
    if (nvs_open(NVS_SETTINGS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY_TRANSITION, (uint8_t)style);
    nvs_commit(h);
    nvs_close(h);
}

static void save_theme(uint8_t idx)
{
    nvs_handle_t h;
    if (nvs_open(NVS_SETTINGS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY_THEME, idx);
    nvs_commit(h);
    nvs_close(h);
}

static void save_accent(uint8_t idx)
{
    nvs_handle_t h;
    if (nvs_open(NVS_SETTINGS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY_ACCENT, idx);
    nvs_commit(h);
    nvs_close(h);
}

static void save_browser_style(uint8_t idx)
{
    nvs_handle_t h;
    if (nvs_open(NVS_SETTINGS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY_BROWSER_STYLE, idx);
    nvs_commit(h);
    nvs_close(h);
}

void ui_init(lv_image_dsc_t *art_dsc)
{
    s_art_dsc = art_dsc;

    /* Build runtime TTF fonts from the embedded flash blobs.  The data pointers
     * stay valid forever (read-only flash segment), so lv_tiny_ttf can reference
     * them without copying.  DejaVu covers accented Latin, Cyrillic, Greek, etc.;
     * it is chained as a fallback so Montserrat handles ASCII and DejaVu fills in
     * any glyph Montserrat lacks. */
    extern const uint8_t mont_start[] asm("_binary_Montserrat_Medium_ttf_start");
    extern const uint8_t mont_end[]   asm("_binary_Montserrat_Medium_ttf_end");
    extern const uint8_t deja_start[] asm("_binary_DejaVuSans_ttf_start");
    extern const uint8_t deja_end[]   asm("_binary_DejaVuSans_ttf_end");

    /* KERNING DISABLED on purpose. lv_tiny_ttf's kerning cache (an lv_rb tree)
     * corrupts the heap in LVGL 9.4 (upstream issue #6304) -- it crashed here
     * under sustained scrolling, sometimes in tiny_ttf_kerning_cache_compare_cb
     * directly, sometimes as a later double-free (tlsf_free "block already
     * marked as free") once the damaged metadata was freed. create_data()
     * defaults to LV_FONT_KERNING_NORMAL; the _ex form lets us pass NONE, which
     * skips the kerning cache path entirely. Visual cost is negligible (slightly
     * looser spacing on a few letter pairs). */
    s_font_28 = lv_tiny_ttf_create_data_ex(mont_start, (size_t)(mont_end - mont_start), 28,
                                           LV_FONT_KERNING_NONE, TTF_GLYPH_CACHE_CNT);
    s_font_24 = lv_tiny_ttf_create_data_ex(mont_start, (size_t)(mont_end - mont_start), 24,
                                           LV_FONT_KERNING_NONE, TTF_GLYPH_CACHE_CNT);

    lv_font_t *deja_28 = lv_tiny_ttf_create_data_ex(deja_start, (size_t)(deja_end - deja_start), 28,
                                                    LV_FONT_KERNING_NONE, TTF_GLYPH_CACHE_CNT);
    lv_font_t *deja_24 = lv_tiny_ttf_create_data_ex(deja_start, (size_t)(deja_end - deja_start), 24,
                                                    LV_FONT_KERNING_NONE, TTF_GLYPH_CACHE_CNT);

    if (s_font_28 && deja_28) s_font_28->fallback = deja_28;
    if (s_font_24 && deja_24) s_font_24->fallback = deja_24;

    bsp_display_lock(-1);

    load_settings();   /* restore saved transition style (default NONE if unset) */
    build_browser_screen();
    build_np_screen();
    build_settings_screen();
    lv_screen_load(s_screen_browser);

    /* Local-progress simulation -- ticks 200 ms of progress every 200 ms
     * so the bar advances smoothly between Spotify polls. */
    lv_timer_create(progress_timer_cb, 200, NULL);

    /* WiFi-strength indicator: poll RSSI every 5 s. */
    lv_timer_create(wifi_timer_cb, 5000, NULL);

    bsp_display_unlock();
}

void ui_set_track_info(const spotify_track_t *info)
{
    bsp_display_lock(-1);
    if (!info) {
        /* No active playback (HTTP 204) or transient error -- stop the
         * progress simulation but keep the last track title/artist visible
         * rather than blanking them. Labels only clear on the first boot
         * before any track has been seen (title still empty). */
        s_track.is_playing = false;
    } else {
        s_track = *info;
        if (s_np_title)  lv_label_set_text(s_np_title, info->title);
        if (s_np_artist) lv_label_set_text(s_np_artist, info->artist);
    }
    update_progress_bar();
    bsp_display_unlock();
}

void ui_art_refresh(const uint8_t *rgb_data, uint16_t w, uint16_t h)
{
    if (!s_art_dsc || !rgb_data || w == 0 || h == 0) return;
    bsp_display_lock(-1);
    s_art_dsc->header.cf  = LV_COLOR_FORMAT_RGB565;
    s_art_dsc->header.w   = w;
    s_art_dsc->header.h   = h;
    s_art_dsc->data       = rgb_data;
    s_art_dsc->data_size  = (uint32_t)w * h * 2;
    if (s_np_art) {
        lv_image_set_src(s_np_art, s_art_dsc);
        lv_obj_invalidate(s_np_art);
    }
    bsp_display_unlock();
}

/* Single source of truth for every browser <-> now-playing switch. `to_np`
 * picks the slide direction for the animated styles (up to now-playing, down
 * to the browser). Must be called under the LVGL lock. */
static void load_screen(lv_obj_t *target, bool to_np)
{
    if (!target || target == lv_screen_active()) return;

    if (s_transition != UI_TRANSITION_NONE) {
        uint32_t now = lv_tick_get();
        if (now < s_anim_block_until) return;   /* a transition is still animating */
        s_anim_block_until = now + 250 + 80;    /* anim duration + margin; self-heals */
    }

    switch (s_transition) {
    case UI_TRANSITION_OVER:
        lv_screen_load_anim(target,
            to_np ? LV_SCR_LOAD_ANIM_OVER_TOP : LV_SCR_LOAD_ANIM_OVER_BOTTOM,
            250, 0, false);
        break;
    case UI_TRANSITION_MOVE:
        lv_screen_load_anim(target,
            to_np ? LV_SCR_LOAD_ANIM_MOVE_TOP : LV_SCR_LOAD_ANIM_MOVE_BOTTOM,
            250, 0, false);
        break;
    case UI_TRANSITION_FADE:
        lv_screen_load_anim(target, LV_SCR_LOAD_ANIM_FADE_IN, 250, 0, false);
        break;
    case UI_TRANSITION_NONE:
    default:
        lv_screen_load(target);
        break;
    }
}

void ui_set_transition_style(ui_transition_t style)
{
    if (style >= 0 && style < UI_TRANSITION_COUNT) s_transition = style;
}

ui_transition_t ui_get_transition_style(void)
{
    return s_transition;
}

static void on_np_tap(lv_event_t *e) { (void)e; ui_request_toggle_play(); }

static void on_seek_start(lv_event_t *e)        { (void)e; s_seeking = true; }
static void on_seek_click_absorb(lv_event_t *e) { lv_event_stop_bubbling(e); }

static void on_seek_pressing(lv_event_t *e)
{
    (void)e;
    if (!s_np_progress || s_track.duration_ms == 0) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);
    /* Map finger X onto the bar's coordinate range. */
    lv_area_t coords;
    lv_obj_get_coords(s_np_progress, &coords);
    int32_t rel = pt.x - coords.x1;
    int32_t w   = coords.x2 - coords.x1;
    if (rel < 0) rel = 0;
    if (rel > w) rel = w;
    int32_t pct = rel * 1000 / w;
    s_track.progress_ms = (uint32_t)((uint64_t)pct * s_track.duration_ms / 1000);
    lv_bar_set_value(s_np_progress, pct, LV_ANIM_OFF);
}

static void on_seek_released(lv_event_t *e)
{
    (void)e;
    if (!s_seeking) return;          /* RELEASED + PRESS_LOST can both fire */
    s_seeking = false;
    if (s_track.duration_ms == 0) return;
    /* progress_ms was kept in sync by on_seek_pressing; send the final value. */
    ui_request_seek(s_track.progress_ms);
}

static void on_gesture(lv_event_t *e)
{
    /* A drag that began on the progress bar is a scrub, not a swipe. on_seek_start
     * (PRESSED) runs before the gesture is detected, so s_seeking is already set. */
    if (s_seeking) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    lv_obj_t *active = lv_screen_active();

    if (dir == LV_DIR_TOP && active == s_screen_browser) {
        lv_indev_wait_release(indev);
        load_screen(s_screen_np, true);
    } else if (dir == LV_DIR_BOTTOM && active == s_screen_np) {
        lv_indev_wait_release(indev);
        load_screen(s_screen_browser, false);
    } else if (dir == LV_DIR_LEFT && active == s_screen_np) {
        lv_indev_wait_release(indev);
        ui_request_next();
    } else if (dir == LV_DIR_RIGHT && active == s_screen_np) {
        lv_indev_wait_release(indev);
        /* Spotify-style: restart from the beginning if more than 3 s into the
         * track; go to previous track if still in the opening few seconds. */
        if (s_track.progress_ms > 3000)
            ui_request_seek(0);
        else
            ui_request_prev();
    }
    (void)e;
}

static void on_card_clicked(lv_event_t *e)
{
    size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
    const album_entry_t *a = albums_get(idx);
    if (!a) return;
    ESP_LOGI(TAG, "play album: %s -- %s", a->artist, a->title);
    /* Hand the URI off to the Spotify task; ui_request_play() must NOT
     * block (HTTPS PUT runs on the other task). The screen transition
     * happens immediately and the play completes asynchronously. */
    ui_request_play(a->uri);
    load_screen(s_screen_np, true);
}

static int find_centered_card(void)
{
    if (!s_browser_scroller || s_card_count == 0) return -1;
    /* Derive the centred index from scroll position + the fixed card layout.
     * Card i's layout slot starts at pad_left + i*step; the slot snapped under
     * the viewport centre is round(scroll_x / step) since the scroller
     * centre-snaps each card. (Matches the snap convention in rebuild_browser_cb
     * and ui_scroll_browser: card i is centred when scroll_left == i*step.) */
    int32_t scroll_x = lv_obj_get_scroll_left(s_browser_scroller);
    int32_t step     = cs() + cg();
    if (step <= 0) return 0;
    int idx = (int)((scroll_x + step / 2) / step);
    if (idx < 0) idx = 0;
    if (idx >= (int)s_card_count) idx = (int)s_card_count - 1;
    return idx;
}

static void on_browser_scroll(lv_event_t *e)
{
    (void)e;
    apply_card_transforms(); /* no-op in carousel mode */
    int idx = find_centered_card();
    /* When a touch/pointer is driving the scroll, adopt the visually centered
     * card as the encoder's target so the two input paths stay in sync. During
     * our own programmatic scroll animation no indev is active, so the target
     * set in ui_scroll_browser is preserved. */
    if (idx >= 0 && lv_indev_active() != NULL) s_target_card = idx;
    if (idx < 0 || idx == s_centered_card) return;
    s_centered_card = idx;
    const album_entry_t *a = albums_get((size_t)idx);
    if (!a) return;
    if (s_browser_title)  lv_label_set_text(s_browser_title, a->title);
    if (s_browser_artist) lv_label_set_text(s_browser_artist, a->artist);
}

static void update_progress_bar(void)
{
    if (!s_np_progress || s_seeking) return;
    int32_t pct = 0;
    if (s_track.duration_ms > 0) {
        uint32_t p = s_track.progress_ms;
        if (p > s_track.duration_ms) p = s_track.duration_ms;
        pct = (int32_t)((uint64_t)p * 1000 / s_track.duration_ms);
    }
    lv_bar_set_value(s_np_progress, pct, LV_ANIM_OFF);
}

static void progress_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_seeking || !s_track.is_playing || s_track.duration_ms == 0) return;
    /* Advance the cached progress at the timer rate. The next spotify
     * poll will overwrite this value with the server's truth. */
    s_track.progress_ms += 200;
    if (s_track.progress_ms > s_track.duration_ms) {
        s_track.progress_ms = s_track.duration_ms;
    }
    update_progress_bar();
}

/* Scale + dim each album cover by its distance from the viewport centre, per
 * style. Layout math only (scroller pad + step), no lv_obj_get_coords, so it's
 * correct before first paint and immune to the transforms feeding back in.
 *
 * IMPORTANT: the transform is applied to the child lv_image (scale + recolor),
 * NOT to the card object. Setting transform_scale/opa on an lv_obj forces LVGL
 * to snapshot it into a per-frame layer, which this board's DIRECT-mode rotated
 * DSI flush mis-composited -- side cards faded to black as you scrolled. Image
 * transforms draw directly (no layer), so they render correctly. (We also tried
 * the 3x3 matrix path for a real iPod skew/tilt, but it crashed the SW blender
 * outright -- see git history; no true 3D tilt is possible safely here.)
 *
 *  - FOCUS:    uniform shrink + dim, centre cover prominent.
 *  - COVERFLOW: side covers squash HORIZONTALLY (scale_x falls faster than
 *               scale_y) so they read as rotated away on a vertical axis -- the
 *               iPod "turning cover" look, faked with non-uniform image scale
 *               since real skew/perspective isn't available safely here. */
static void apply_card_transforms(void)
{
    if (!BROWSER_STYLE_TRANSFORMS(s_browser_style) || !s_browser_scroller) return;
    bool cf = (s_browser_style == BROWSER_COVERFLOW);
    int32_t dim_rise   = cf ? 150 : 95;   /* per-step recolor-toward-black */
    int32_t dim_max    = cf ? 160 : 110;
    int32_t scroll_x   = lv_obj_get_scroll_left(s_browser_scroller);
    int32_t pad_left   = (SCREEN_W - cs()) / 2;
    int32_t step       = cs() + cg();
    int32_t scr_center = SCREEN_W / 2;
    for (size_t i = 0; i < s_card_count; i++) {
        if (!s_card_imgs[i]) continue;
        int32_t card_cx = pad_left + (int32_t)i * step + cs() / 2 - scroll_x;
        int32_t dist = card_cx - scr_center;
        if (dist < 0) dist = -dist;

        if (cf) {
            /* Horizontal squash dominates (the "turning" illusion); vertical
             * shrinks only a little (depth). At one step out: ~40% wide, ~80%
             * tall. Both 100% at centre. */
            int32_t sx = LV_SCALE_NONE - dist * 150 / step;
            if (sx < 70) sx = 70;
            int32_t sy = LV_SCALE_NONE - dist * 55 / step;
            if (sy < 170) sy = 170;
            lv_image_set_scale_x(s_card_imgs[i], (uint32_t)sx);
            lv_image_set_scale_y(s_card_imgs[i], (uint32_t)sy);
        } else {
            int32_t scale = LV_SCALE_NONE - dist * 76 / step;
            if (scale < 150) scale = 150;
            lv_image_set_scale(s_card_imgs[i], (uint32_t)scale);
        }
        /* Dim side covers by recoloring toward black (image-draw path, no
         * layer). 0 at centre, rising with distance. */
        int32_t dim = dist * dim_rise / step;
        if (dim > dim_max) dim = dim_max;
        lv_obj_set_style_image_recolor(s_card_imgs[i], lv_color_black(), 0);
        lv_obj_set_style_image_recolor_opa(s_card_imgs[i], (lv_opa_t)dim, 0);
    }
}

static void refresh_browser_style_selection(void)
{
    for (int i = 0; i < BROWSER_STYLE_COUNT; i++) {
        if (!s_brstyle_btns[i] || !s_brstyle_labels[i]) continue;
        bool sel = (i == (int)s_browser_style);
        lv_obj_set_style_bg_color(s_brstyle_btns[i],
            sel ? lv_color_hex(accent_color()) : lv_color_hex(s_th->surface), 0);
        lv_obj_set_style_text_color(s_brstyle_labels[i],
            sel ? lv_color_black() : lv_color_hex(s_th->text2), 0);
        lv_label_set_text(s_brstyle_labels[i], k_browser_style_names[i]);
    }
}

static void on_browser_style_option(lv_event_t *e)
{
    uint8_t idx = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    if (idx >= BROWSER_STYLE_COUNT || idx == s_browser_style) return;
    s_browser_style = idx;
    save_browser_style(idx);
    refresh_browser_style_selection();
    ESP_LOGI(TAG, "browser style -> %s", k_browser_style_names[idx]);
    /* Rebuild the browser screen in the background -- user stays in settings. */
    lv_async_call(rebuild_browser_cb, NULL);
}

static void rebuild_browser_cb(void *unused)
{
    (void)unused;
    int     saved_card  = s_centered_card;
    lv_obj_t *old_browser = s_screen_browser;

    build_browser_screen();

    if (saved_card > 0 && s_browser_scroller) {
        lv_obj_update_layout(s_browser_scroller);
        lv_obj_scroll_to_x(s_browser_scroller,
                           (int32_t)saved_card * (cs() + cg()),
                           LV_ANIM_OFF);
        s_centered_card = saved_card;
        s_target_card   = saved_card;
        const album_entry_t *a = albums_get((size_t)saved_card);
        if (a && s_browser_title && s_browser_artist) {
            lv_label_set_text(s_browser_title,  a->title);
            lv_label_set_text(s_browser_artist, a->artist);
        }
    }
    apply_card_transforms();
    lv_obj_delete(old_browser);
}

static void wifi_timer_cb(lv_timer_t *t)
{
    (void)t;
    int bars = 0;
    int rssi = 0;
    if (esp_wifi_sta_get_rssi(&rssi) == ESP_OK) {
        if      (rssi >= -55) bars = 4;
        else if (rssi >= -65) bars = 3;
        else if (rssi >= -75) bars = 2;
        else                  bars = 1;
    }
    for (int i = 0; i < 4; i++) {
        if (!s_wifi_bars[i]) continue;
        lv_color_t c = (i < bars)
            ? lv_color_hex(s_th->text)
            : lv_color_hex(s_th->track);
        lv_obj_set_style_bg_color(s_wifi_bars[i], c, 0);
    }
}

bool ui_is_now_playing(void)
{
    return lv_screen_active() == s_screen_np;
}

void ui_toggle_view(void)
{
    lv_obj_t *active = lv_screen_active();
    if (active == s_screen_browser) {
        load_screen(s_screen_np, true);
    } else {
        load_screen(s_screen_browser, false);
    }
}

void ui_play_centered_album(void)
{
    int idx = find_centered_card();
    if (idx < 0) return;
    const album_entry_t *a = albums_get((size_t)idx);
    if (!a) return;
    ESP_LOGI(TAG, "play album (encoder): %s -- %s", a->artist, a->title);
    ui_request_play(a->uri);
    load_screen(s_screen_np, true);
}

void ui_scroll_browser(int32_t delta)
{
    if (!s_browser_scroller || delta == 0 || s_card_count == 0) return;

    int t = s_target_card + (int)delta;
    if (t < 0) t = 0;
    if (t >= (int)s_card_count) t = (int)s_card_count - 1;
    s_target_card = t;
    if (!s_cards[t]) return;

    /* Scroll straight to the target card's layout slot (t*step), matching the
     * snap convention in rebuild_browser_cb/find_centered_card: card i is
     * centred when scroll_left == i*step. */
    lv_obj_scroll_to_x(s_browser_scroller, (int32_t)t * (cs() + cg()),
                       LV_ANIM_ON);
}

uint32_t ui_get_progress_ms(void)
{
    return s_track.progress_ms;
}

static void vol_hud_hide_cb(lv_timer_t *t)
{
    (void)t;
    if (s_vol_hud) lv_obj_add_flag(s_vol_hud, LV_OBJ_FLAG_HIDDEN);
    s_vol_hud_timer = NULL;
}

void ui_show_volume_hud(int pct, bool muted)
{
    if (!s_vol_hud) return;
    char buf[20];
    if (muted) {
        snprintf(buf, sizeof(buf), "MUTED");
    } else {
        snprintf(buf, sizeof(buf), "VOL %d%%", pct);
    }
    lv_label_set_text(s_vol_hud, buf);
    lv_obj_remove_flag(s_vol_hud, LV_OBJ_FLAG_HIDDEN);

    if (s_vol_hud_timer) {
        lv_timer_reset(s_vol_hud_timer);
    } else {
        s_vol_hud_timer = lv_timer_create(vol_hud_hide_cb, 2000, NULL);
        lv_timer_set_repeat_count(s_vol_hud_timer, 1);
    }
}
