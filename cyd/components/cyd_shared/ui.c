/*
 * LVGL UI implementation -- see ui.h for the high-level contract.
 *
 * Layout (320x240 landscape):
 *
 *   Browser screen
 *     y= 15..145  : horizontal scroll carousel (130 px tall)
 *                   each card 120x120 with rounded corners, palette-
 *                   coloured placeholder + first-letter initial.
 *     y=160       : album title (montserrat 16, white)
 *     y=185       : artist  (montserrat 12, light grey)
 *     y=222       : "^ now playing" hint
 *
 *   Now-playing screen
 *     y=  2       : "v albums" hint
 *     y= 22..182  : 160x160 album art, centred horizontally (x=80..240)
 *     y=188       : track title (montserrat 16, white)
 *     y=208       : artist (montserrat 12, light grey)
 *     y=226       : 200x6 progress bar, white indicator on dark grey track
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
#include "esp_lvgl_port.h"
#include "esp_wifi.h"
#include "player.h"

#include <string.h>

static const char *TAG = "ui";

#define SCREEN_W 320
#define SCREEN_H 240

#define CARD_SIZE     120
#define CARD_GAP       20
#define SCROLLER_Y     15
#define SCROLLER_H    130

#define ART_W         160
#define ART_H         160
#define ART_X          ((SCREEN_W - ART_W) / 2)
#define ART_Y          22

#define PROG_W        200
#define PROG_H          6
#define PROG_X         ((SCREEN_W - PROG_W) / 2)
#define PROG_Y        226

#define NP_TITLE_Y    188
#define NP_ARTIST_Y   208

#define BR_TITLE_Y    160
#define BR_ARTIST_Y   185

static lv_obj_t *s_screen_np      = NULL;
static lv_obj_t *s_screen_browser = NULL;

static lv_obj_t *s_np_art      = NULL;
static lv_obj_t *s_np_title    = NULL;
static lv_obj_t *s_np_artist   = NULL;
static lv_obj_t *s_np_progress = NULL;
static lv_obj_t *s_vol_hud     = NULL;
/* Last device volume reported by the poll (-1 = unknown). Published here under
 * the LVGL lock so input_update (which also runs under it) can adopt it as the
 * encoder/HUD base without a cross-task race. */
static int       s_device_vol  = -1;

static lv_timer_t *s_vol_hud_timer = NULL;

static lv_obj_t *s_browser_scroller = NULL;
static lv_obj_t *s_browser_title    = NULL;
static lv_obj_t *s_browser_artist   = NULL;

static lv_obj_t *s_wifi_bars[4]     = {0};

#define MAX_CARDS 32
static lv_obj_t       *s_cards[MAX_CARDS]    = {0};
static lv_image_dsc_t  s_card_dscs[MAX_CARDS] = {0};
static size_t          s_card_count          = 0;
static int             s_centered_card       = -1;
/* Logical target card for encoder scrolling. Tracked independently of the
 * live (possibly mid-animation) scroll position so fast spins don't lose
 * detents. Re-synced to the visually centered card on touch-driven scrolls. */
static int             s_target_card         = 0;

static lv_image_dsc_t *s_art_dsc = NULL;

/* Cached track state. The LVGL progress timer reads progress_ms /
 * duration_ms / is_playing from here and ticks the bar between
 * Spotify polls so the bar moves at ~5 Hz instead of 0.2 Hz. */
static spotify_track_t s_track = {0};

static void on_gesture(lv_event_t *e);
static void on_card_clicked(lv_event_t *e);
static void on_browser_scroll(lv_event_t *e);
static void progress_timer_cb(lv_timer_t *t);
static void wifi_timer_cb(lv_timer_t *t);
static void update_progress_bar(void);

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
    lv_obj_set_style_bg_color(s_screen_browser, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_screen_browser, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_screen_browser, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_screen_browser, on_gesture, LV_EVENT_GESTURE, NULL);

    s_browser_scroller = lv_obj_create(s_screen_browser);
    lv_obj_set_size(s_browser_scroller, SCREEN_W, SCROLLER_H);
    lv_obj_set_pos(s_browser_scroller, 0, SCROLLER_Y);
    lv_obj_set_style_bg_color(s_browser_scroller, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_browser_scroller, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_browser_scroller, 0, 0);
    lv_obj_set_style_pad_top(s_browser_scroller, 0, 0);
    lv_obj_set_style_pad_bottom(s_browser_scroller, 0, 0);
    /* Pad left/right so the first and last cards can fully snap to centre. */
    lv_obj_set_style_pad_left (s_browser_scroller, (SCREEN_W - CARD_SIZE) / 2, 0);
    lv_obj_set_style_pad_right(s_browser_scroller, (SCREEN_W - CARD_SIZE) / 2, 0);
    lv_obj_set_style_pad_column(s_browser_scroller, CARD_GAP, 0);
    lv_obj_set_flex_flow(s_browser_scroller, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_browser_scroller, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(s_browser_scroller, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(s_browser_scroller, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_snap_x(s_browser_scroller, LV_SCROLL_SNAP_CENTER);
    lv_obj_add_event_cb(s_browser_scroller, on_browser_scroll, LV_EVENT_SCROLL, NULL);

    s_card_count = albums_count();
    if (s_card_count > MAX_CARDS) {
        ESP_LOGW(TAG, "album list has %u entries but MAX_CARDS is %d; showing first %d",
                 (unsigned)albums_count(), MAX_CARDS, MAX_CARDS);
        s_card_count = MAX_CARDS;
    }

    for (size_t i = 0; i < s_card_count; i++) {
        const album_entry_t *a    = albums_get(i);
        const uint16_t      *thumb = album_thumb_data(i);

        lv_obj_t *card = lv_obj_create(s_browser_scroller);
        lv_obj_set_size(card, CARD_SIZE, CARD_SIZE);
        lv_obj_set_style_radius(card, 0, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, on_card_clicked, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);

        if (thumb) {
            /* Real artwork: use the embedded 120x120 RGB565 thumb as
             * the card's background image. Rounded corners are masked
             * by the card's radius + CLIP_CORNER flag. */
            s_card_dscs[i].header.cf  = LV_COLOR_FORMAT_RGB565;
            s_card_dscs[i].header.w   = ALBUM_THUMB_W;
            s_card_dscs[i].header.h   = ALBUM_THUMB_H;
            s_card_dscs[i].data       = (const uint8_t *)thumb;
            s_card_dscs[i].data_size  = ALBUM_THUMB_BYTES;
            lv_obj_set_style_bg_color(card, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_image_src(card, &s_card_dscs[i], 0);
            lv_obj_set_style_bg_image_opa(card, LV_OPA_COVER, 0);
        } else {
            /* No embedded thumb (shouldn't happen with the generated
             * blob, but fall back gracefully): coloured square with
             * first-letter initial. */
            lv_obj_set_style_bg_color(card, card_color(i), 0);
            lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
            lv_obj_t *letter = lv_label_create(card);
            char ini[2] = { a->title[0] ? a->title[0] : '?', '\0' };
            lv_label_set_text(letter, ini);
            lv_obj_set_style_text_color(letter, lv_color_white(), 0);
            lv_obj_set_style_text_font(letter, &lv_font_montserrat_16, 0);
            lv_obj_center(letter);
        }

        s_cards[i] = card;
    }

    s_browser_title = lv_label_create(s_screen_browser);
    style_label(s_browser_title, &lv_font_montserrat_16, lv_color_white(), BR_TITLE_Y);

    s_browser_artist = lv_label_create(s_screen_browser);
    style_label(s_browser_artist, &lv_font_montserrat_12,
                lv_color_hex(0xA0A0A0), BR_ARTIST_Y);

    if (s_card_count > 0) {
        const album_entry_t *a = albums_get(0);
        lv_label_set_text(s_browser_title, a->title);
        lv_label_set_text(s_browser_artist, a->artist);
        s_centered_card = 0;
    }

    /* WiFi-strength indicator: four bars top-left, updated by wifi_timer_cb. */
    for (int i = 0; i < 4; i++) {
        lv_obj_t *bar = lv_obj_create(s_screen_browser);
        int h = 4 + i * 3;
        lv_obj_set_size(bar, 4, h);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_radius(bar, 0, 0);
        lv_obj_set_style_pad_all(bar, 0, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0x303030), 0);
        lv_obj_set_pos(bar, 4 + i * 6, 16 - h);
        s_wifi_bars[i] = bar;
    }

    /* "^ now playing" hint at the bottom edge. */
    lv_obj_t *hint = lv_label_create(s_screen_browser);
    lv_label_set_text(hint, LV_SYMBOL_UP " now playing");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x606060), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -2);
}

static void build_np_screen(void)
{
    s_screen_np = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen_np, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_screen_np, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_screen_np, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_screen_np, on_gesture, LV_EVENT_GESTURE, NULL);

    /* "v albums" hint at the top so the user knows the swipe-down gesture. */
    lv_obj_t *hint = lv_label_create(s_screen_np);
    lv_label_set_text(hint, LV_SYMBOL_DOWN " albums");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x606060), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 2);

    s_np_art = lv_image_create(s_screen_np);
    lv_obj_set_size(s_np_art, ART_W, ART_H);
    lv_obj_set_pos(s_np_art, ART_X, ART_Y);
    if (s_art_dsc && s_art_dsc->data) {
        lv_image_set_src(s_np_art, s_art_dsc);
    }

    s_np_title = lv_label_create(s_screen_np);
    style_label(s_np_title, &lv_font_montserrat_16, lv_color_white(), NP_TITLE_Y);

    s_np_artist = lv_label_create(s_screen_np);
    style_label(s_np_artist, &lv_font_montserrat_12,
                lv_color_hex(0xA0A0A0), NP_ARTIST_Y);

    s_np_progress = lv_bar_create(s_screen_np);
    lv_obj_set_size(s_np_progress, PROG_W, PROG_H);
    lv_obj_set_pos(s_np_progress, PROG_X, PROG_Y);
    lv_bar_set_range(s_np_progress, 0, 1000);
    lv_bar_set_value(s_np_progress, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_np_progress, lv_color_hex(0x303030), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_np_progress, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_np_progress, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_np_progress, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_np_progress, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(s_np_progress, 1, LV_PART_INDICATOR);

    s_vol_hud = lv_label_create(s_screen_np);
    lv_label_set_text(s_vol_hud, "");
    lv_obj_set_style_text_color(s_vol_hud, lv_color_hex(0xFF4040), 0);
    lv_obj_set_style_text_font(s_vol_hud, &lv_font_montserrat_12, 0);
    lv_obj_align(s_vol_hud, LV_ALIGN_TOP_RIGHT, -4, 2);
    lv_obj_add_flag(s_vol_hud, LV_OBJ_FLAG_HIDDEN);
}

void ui_init(lv_image_dsc_t *art_dsc)
{
    s_art_dsc = art_dsc;

    lvgl_port_lock(0);

    build_browser_screen();
    build_np_screen();
    lv_screen_load(s_screen_browser);

    /* Local-progress simulation -- ticks 200 ms of progress every 200 ms
     * so the bar advances smoothly between Spotify polls. */
    lv_timer_create(progress_timer_cb, 200, NULL);
    /* WiFi strength refresh. */
    lv_timer_create(wifi_timer_cb, 2000, NULL);

    lvgl_port_unlock();
}

void ui_set_track_info(const spotify_track_t *info)
{
    lvgl_port_lock(0);
    if (!info) {
        /* No active playback (HTTP 204) or transient error -- stop the
         * progress simulation but keep the last track title/artist visible
         * rather than blanking them. Labels only clear on the first boot
         * before any track has been seen (title still empty). */
        s_track.is_playing = false;
    } else {
        s_track = *info;
        if (info->volume_pct >= 0) s_device_vol = info->volume_pct;
        if (s_np_title)  lv_label_set_text(s_np_title, info->title);
        if (s_np_artist) lv_label_set_text(s_np_artist, info->artist);
    }
    update_progress_bar();
    lvgl_port_unlock();
}

int ui_get_device_volume(void)
{
    return s_device_vol;
}

void ui_art_refresh(const uint8_t *rgb_data, uint16_t w, uint16_t h)
{
    if (!s_art_dsc || !rgb_data || w == 0 || h == 0) return;
    lvgl_port_lock(0);
    s_art_dsc->header.cf  = LV_COLOR_FORMAT_RGB565;
    s_art_dsc->header.w   = w;
    s_art_dsc->header.h   = h;
    s_art_dsc->data       = rgb_data;
    s_art_dsc->data_size  = (uint32_t)w * h * 2;
    if (s_np_art) {
        lv_image_set_src(s_np_art, s_art_dsc);
        lv_obj_invalidate(s_np_art);
    }
    lvgl_port_unlock();
}

static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    lv_obj_t *active = lv_screen_active();

    if (dir == LV_DIR_TOP && active == s_screen_browser) {
        lv_indev_wait_release(indev);
        lv_screen_load_anim(s_screen_np, LV_SCR_LOAD_ANIM_OVER_TOP,
                            250, 0, false);
    } else if (dir == LV_DIR_BOTTOM && active == s_screen_np) {
        lv_indev_wait_release(indev);
        lv_screen_load_anim(s_screen_browser, LV_SCR_LOAD_ANIM_OVER_BOTTOM,
                            250, 0, false);
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
    lv_screen_load_anim(s_screen_np, LV_SCR_LOAD_ANIM_OVER_TOP,
                        250, 0, false);
}

static int find_centered_card(void)
{
    if (!s_browser_scroller || s_card_count == 0) return -1;
    /* Use absolute screen coordinates so we don't have to reason about
     * LVGL's content-vs-visual coordinate semantics. lv_obj_get_coords()
     * reflects each card's real on-screen position (it moves as the
     * carousel scrolls), so the card whose centre is closest to the
     * scroller's screen centre is the one snapped under the viewport. */
    lv_area_t sa;
    lv_obj_get_coords(s_browser_scroller, &sa);
    int32_t target = (sa.x1 + sa.x2) / 2;

    int best_i = 0;
    int32_t best_d = INT32_MAX;
    for (size_t i = 0; i < s_card_count; i++) {
        if (!s_cards[i]) continue;
        lv_area_t ca;
        lv_obj_get_coords(s_cards[i], &ca);
        int32_t cx = (ca.x1 + ca.x2) / 2;
        int32_t d  = (cx > target) ? (cx - target) : (target - cx);
        if (d < best_d) {
            best_d = d;
            best_i = (int)i;
        }
    }
    return best_i;
}

static void on_browser_scroll(lv_event_t *e)
{
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
    (void)e;
}

static void update_progress_bar(void)
{
    if (!s_np_progress) return;
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
    if (!s_track.is_playing || s_track.duration_ms == 0) return;
    /* Advance the cached progress at the timer rate. The next spotify
     * poll will overwrite this value with the server's truth. */
    s_track.progress_ms += 200;
    if (s_track.progress_ms > s_track.duration_ms) {
        s_track.progress_ms = s_track.duration_ms;
    }
    update_progress_bar();
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
        lv_color_t c = (i < bars) ? lv_color_white() : lv_color_hex(0x303030);
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
        lv_screen_load_anim(s_screen_np, LV_SCR_LOAD_ANIM_OVER_TOP, 250, 0, false);
    } else {
        lv_screen_load_anim(s_screen_browser, LV_SCR_LOAD_ANIM_OVER_BOTTOM, 250, 0, false);
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
    lv_screen_load_anim(s_screen_np, LV_SCR_LOAD_ANIM_OVER_TOP, 250, 0, false);
}

void ui_scroll_browser(int32_t delta)
{
    if (!s_browser_scroller || delta == 0 || s_card_count == 0) return;

    int t = s_target_card + (int)delta;
    if (t < 0) t = 0;
    if (t >= (int)s_card_count) t = (int)s_card_count - 1;
    s_target_card = t;
    if (!s_cards[t]) return;

    /* Scroll by the exact distance to bring the target card's centre onto the
     * scroller's centre. Self-correcting: any accumulated drift is absorbed
     * each step, so the carousel always lands snapped on a card. */
    lv_area_t sa, ca;
    lv_obj_get_coords(s_browser_scroller, &sa);
    lv_obj_get_coords(s_cards[t], &ca);
    int32_t v = ((sa.x1 + sa.x2) - (ca.x1 + ca.x2)) / 2;
    if (v != 0) lv_obj_scroll_by(s_browser_scroller, v, 0, LV_ANIM_ON);
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
