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

#include "ui_tune.h"   /* user-tweakable layout/colour knobs -- edit THAT file */
#include "albums.h"
#include "album_thumbs.h"
#include "audio.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "bsp/esp-bsp.h"
#include "spotify.h"
#include "nvs.h"
#include "esp_random.h"

#include <string.h>
#include <math.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"   /* esp_timer_get_time() for FPS render-duration timing */

static const char *TAG = "ui";

#define SCREEN_W 800
#define SCREEN_H 480

/* Centre album is drawn at 286 px in ALL browser styles (matches Cover Flow's
 * 220px thumb x CF_CARD_SCALE 1.30). The flex scroller is sized to fit it. */
#define CARD_SIZE     286
#define CARD_GAP       28
#define SCROLLER_Y     30
#define SCROLLER_H    296

/* Now-playing layout, 8px grid. Art is centred up top; below it the title,
 * artist, a thin progress bar flanked by elapsed/remaining timestamps, then a
 * row of three square transport keys at the bottom edge. Swipe gestures still
 * work as shortcuts. */
#define ART_W         256
#define ART_H         256
#define ART_X          ((SCREEN_W - ART_W) / 2)
#define ART_Y          44

#define PROG_W        520
#define PROG_H          6
#define PROG_X         ((SCREEN_W - PROG_W) / 2)
/* Per-mode (ticks, timestamps, seek overlay and the GLYPH gas-tank all derive
 * from PROG_Y automatically). Values live in ui_tune.h. */
#define PROG_Y        (k_tune_prog_y[s_mode])

/* Generous touch band around the thin progress bar: a drag that starts here is
 * a scrub, not a screen swipe. Wider than the bar so 0%/100% are easy to grab.
 * The transport keys sit just below and are created after the overlay so they
 * stay on top in the small region where the two touch zones meet. */
#define SEEK_OV_W     (PROG_W + 80)
#define SEEK_OV_H      48
#define SEEK_OV_X     ((SCREEN_W - SEEK_OV_W) / 2)
#define SEEK_OV_Y     (PROG_Y + PROG_H / 2 - SEEK_OV_H / 2)

/* Elapsed / remaining timestamps either side of the bar (width per mode in
 * ui_tune.h -- mono/pixel digits are ~2x as wide as Montserrat's). */
#define TS_W          (k_tune_ts_w[s_mode])
#define TS_Y          (PROG_Y - 14)

/* Transport keys: three 56px squares, gap 28, centred (group centre = 400). */
#define TKEY_SZ        56
#define TKEY_GAP       28
#define TKEY_Y        (k_tune_tkey_y[s_mode])

/* Draggable seek thumb -- shown only while scrubbing. */
#define THUMB_W        14
#define THUMB_H        26

#define NP_TITLE_Y    (k_tune_np_title_y[s_mode])
#define NP_ARTIST_Y   (k_tune_np_artist_y[s_mode])
#define NP_DEVICE_Y   374   /* small device-name text below artist */

/* All browser styles share one strip geometry (286px cover ends ~y321), so
 * title/artist sit at one per-mode position below it -- see ui_tune.h. CF_*
 * kept equal for the per-style call sites. */
#define BR_TITLE_Y    (k_tune_br_title_y[s_mode])
#define BR_ARTIST_Y   (k_tune_br_artist_y[s_mode])
#define CF_TITLE_Y    BR_TITLE_Y
#define CF_ARTIST_Y   BR_ARTIST_Y


/* Runtime TTF fonts -- created once from embedded flash blobs, shared across
 * all screen builds (title/artist labels; hints keep the built-in bitmap font
 * because it carries the LVGL symbol glyphs). */
/* Fonts are all compiled-in: runtime tiny_ttf is disabled on this target (its
 * stb_truetype rasteriser crashes on the P4). SANS = LVGL's built-in Montserrat;
 * SLAB = Arvo Bold baked by scripts/gen_lvgl_font.py (with a Montserrat fallback
 * for accented glyphs outside its ASCII range). */
extern const lv_font_t lv_font_arvo_28;
extern const lv_font_t lv_font_arvo_24;
#define FONT_SANS  0
#define FONT_SLAB  1
static uint8_t s_font_choice = FONT_SANS;

static lv_obj_t *s_screen_np      = NULL;
static lv_obj_t *s_screen_browser = NULL;

static lv_obj_t *s_np_art      = NULL;
static lv_obj_t *s_np_title    = NULL;
static lv_obj_t *s_np_artist   = NULL;
static lv_obj_t *s_np_progress = NULL;
static lv_obj_t *s_np_elapsed  = NULL;   /* M:SS left of the bar */
static lv_obj_t *s_np_remain   = NULL;   /* -M:SS (or total M:SS) right of the bar */
static bool      s_remain_show_total = false;  /* tap toggles remaining <-> total */
static lv_obj_t *s_seek_thumb  = NULL;   /* drag knob, shown only while scrubbing */
static lv_obj_t *s_np_play_lbl = NULL;   /* centre transport-key icon (play/pause) */
static lv_obj_t *s_vol_hud     = NULL;
static lv_obj_t *s_np_volume   = NULL;     /* now-playing volume fader */
static lv_obj_t *s_np_device   = NULL;     /* small device-name label below artist */
static uint32_t  s_vol_hold_until = 0;     /* suppress poll-driven fader updates while the user adjusts it */

static lv_timer_t *s_vol_hud_timer = NULL;
static bool        s_seeking        = false;
static bool        s_vol_dragging   = false; /* guard: vertical slider drag must not fire swipe-to-browser */
static bool        s_hint_bounced   = false;   /* browser hint bounces once, first boot only */

/* Seek reconciliation. After a local seek the /me/player poll keeps reporting
 * the pre-seek position for a cycle or two (Spotify lags the PUT), which would
 * snap the bar backwards then forwards again. While the guard is armed we keep
 * the locally-sought position and ignore the server's progress_ms, until either
 * the server catches up (within TOL of where we expect to be) or the deadline
 * passes. A track change drops the guard immediately. */
#define SEEK_GUARD_MAX_MS  8000   /* hard cap on ignoring server progress */
#define SEEK_GUARD_TOL_MS  3000   /* server within this of expected => trust it */
static uint32_t s_seek_guard_until = 0;   /* lv_tick deadline; 0 = inactive */
static uint32_t s_seek_anchor_ms   = 0;   /* position we sought to */
static uint32_t s_seek_anchor_tick = 0;   /* lv_tick when we sought */

static lv_obj_t *s_browser_scroller = NULL;
static lv_obj_t *s_browser_title    = NULL;
static lv_obj_t *s_browser_artist   = NULL;
static lv_obj_t *s_wifi_bars[4]     = {0};

/* Upper bound on browsable albums. s_card_count tracks albums_count() (derived
 * from the generated albums.c, itself from spotify-albums-list.txt) -- this cap
 * only bounds the static card arrays; raise it if the list ever grows past it. */
#define MAX_CARDS 128
static lv_obj_t       *s_cards[MAX_CARDS]    = {0};
static lv_obj_t       *s_card_imgs[MAX_CARDS] = {0};  /* child lv_image per card */
static lv_image_dsc_t  s_card_dscs[MAX_CARDS] = {0};
/* Last FOCUS transform applied per card (pre-base scale / dim opa). Both values
 * saturate beyond ~1.4 card-steps from centre, so on any one scroll event only
 * the handful of cards near the viewport actually change -- caching lets
 * apply_card_transforms skip the LVGL writes (and their invalidations) for the
 * rest. -1 = nothing applied yet; reset in build_browser_screen. */
static int16_t         s_card_scale_last[MAX_CARDS];
static int16_t         s_card_dim_last[MAX_CARDS];
static size_t          s_card_count          = 0;
static int             s_centered_card       = -1;
/* Logical target card for encoder scrolling. Tracked independently of the
 * live (possibly mid-animation) scroll position so fast spins don't lose
 * detents. Re-synced to the visually centered card on touch-driven scrolls. */
static int             s_target_card         = 0;
/* Index of the currently playing album in the carousel, -1 if not matched.
 * Drives the accent border + auto-snap in ui_set_track_info (3C). */
static int             s_playing_card_idx    = -1;

/* True when WiFi has dropped (bars == 0). wifi_timer_cb replaces s_np_title
 * with "OFFLINE" on transition in so the user isn't lied to with a stale
 * track, and restores the cached title on transition out. */
static bool            s_offline             = false;

/* Generic auto-hiding toast on the now-playing screen ("No active device",
 * etc). Uses the same hide-via-timer pattern as the volume HUD. */
static lv_obj_t       *s_toast               = NULL;
static lv_timer_t     *s_toast_timer         = NULL;

/* Auto-dim state: ramp the backlight down from s_brightness when the user
 * hasn't touched the screen for a while, so the always-on IPS panel doesn't
 * waste ~150 mA of backlight 24/7. lv_disp_get_inactive_time() resets on any
 * input, so any touch / button event snaps brightness back to s_brightness.
 * 0 = awake (full s_brightness), 1 = dimmed (30%), 2 = very-dim (10%). */
#define AUTO_DIM_AFTER_MS     60000UL    /* 1 min */
#define AUTO_DIM_DEEP_AFTER_MS 300000UL  /* 5 min */
static uint8_t s_dim_state = 0;

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

/* Every MODE has a dark and a light palette; a separate DARK/LIGHT toggle in
 * Settings picks which of the pair is live (the old standalone DARK/BLACK
 * charcoal modes were merged -- BLACK is BASIC's dark face, the warm off-white
 * LIGHT is its light face). { bg, surface, text, text2, dim, track }. */
static const theme_t THEME_BLACK = { 0x000000, 0x141414, 0xFAFAFA, 0x9A9A9A, 0x5E5E5E, 0x242424 };
static const theme_t THEME_LIGHT = { 0xECEAE6, 0xDAD6CF, 0x1A1A1A, 0x57534C, 0x8C877E, 0xC6C1B8 };
/* GLYPH: Nothing-OS light. Warm light-grey ground + black ink, dot-matrix
 * type for HEADINGS ONLY (body/labels/icons are clean small type), hairline
 * outline pills with solid-ink selection, and instrument chrome drawn in ink
 * dots (gas-tank progress, dot volume page, WiFi meter). The accent is
 * reserved for live elements: playhead, selection line, volume shortcut.
 * { bg, surface(chip), text(ink), text2, dim, track(hairline) }. */
static const theme_t THEME_GLYPH  = { 0xEDEBE7, 0xF6F4F0, 0x141414, 0x4E4B46, 0x98948C, 0xD4D0C8 };
/* GLYPH DARK: the same Nothing-OS instrument language inverted -- warm
 * near-black ground, off-white "ink". All GLYPH chrome (dot headings, hairline
 * pills, gas-tank/WiFi/volume dots) reads s_th->text/track, so the one palette
 * swap inverts the whole instrument. Appended at the END of the enum so
 * NVS-persisted MODE indices (PIXEL=4, PAPER=5) stay valid. */
static const theme_t THEME_GLYPH_DARK = { 0x141412, 0x1D1D1B, 0xF0EEE8, 0xA6A29A, 0x636057, 0x3A3833 };
/* PIXEL: dark CRT near-black with high-contrast off-white text; 1bpp pixel font +
 * Bayer-dithered pixelated art. Accent drives progress bar and selection highlights.
 * NOTE: porting to a future waveshare/esp-idf-ha/ is automatic (ui.c is copied);
 * CYD cyd_shared/ui.c would require a separate port. */
static const theme_t THEME_PIXEL  = { 0x0A0C0A, 0x161616, 0xE6E6E6, 0x8A8A8A, 0x4A4A4A, 0x1C1C1C };
/* PIXEL light: pale LCD-calculator greenish-grey ground with near-black pixels
 * (the dark face is the CRT, the light face is the handheld LCD). */
static const theme_t THEME_PIXEL_LIGHT = { 0xE2E6DE, 0xD0D6CA, 0x10140F, 0x3E463C, 0x76806F, 0xB6BEAE };
/* PAPER: teletype data-brutalism. Warm cream "paper" + near-black ink, set in
 * the unscii-8 monospace bitmap font; album art and thumbnails are reduced to
 * a 1-bit ink-on-paper ordered dither; chrome is printed-form furniture --
 * ruled frames, tick rulers, inverted ink title chips, corner data labels.
 * The accent supplies the vermilion "live" pops (ORANGE is the canonical
 * pairing; RED gives the maroon ledger look). */
static const theme_t THEME_PAPER  = { 0xE8E0CC, 0xDED4BC, 0x26211A, 0x4A4438, 0x8A8170, 0xC4BAA0 };
/* PAPER dark: the cream sheet photo-negative -- dark sepia "blueprint" ground,
 * parchment ink. The light face keeps the canonical cream. */
static const theme_t THEME_PAPER_DARK = { 0x211D14, 0x2B2719, 0xE4DCC4, 0xB0A88E, 0x6E6754, 0x4A4434 };
static const theme_t *s_th = &THEME_BLACK;

/* MODE picks the design language (BASIC / GLYPH / PIXEL / PAPER); a separate
 * DARK/LIGHT toggle picks which face of that mode's palette pair is live.
 * COLOUR (accent) overlays a single accent on any combination. */
enum { MODE_BASIC = 0, MODE_GLYPH, MODE_PIXEL, MODE_PAPER, MODE_COUNT };
static uint8_t s_mode = MODE_BASIC;
static bool    s_dark = true;
/* [mode][0] = dark face, [mode][1] = light face. */
static const theme_t *const k_mode_palettes[MODE_COUNT][2] = {
    [MODE_BASIC] = { &THEME_BLACK,      &THEME_LIGHT },
    [MODE_GLYPH] = { &THEME_GLYPH_DARK, &THEME_GLYPH },
    [MODE_PIXEL] = { &THEME_PIXEL,      &THEME_PIXEL_LIGHT },
    [MODE_PAPER] = { &THEME_PAPER_DARK, &THEME_PAPER },
};
static void apply_palette(void) { s_th = k_mode_palettes[s_mode][s_dark ? 0 : 1]; }
/* THEME ALBUM ART: when off, PIXEL/PAPER keep their chrome but the covers stay
 * unstyled (no dither/pixelation). NVS-persisted. */
static bool s_theme_art = true;

/* ---- Layout knobs, indexed by s_mode (values live in ui_tune.h) ---------- */
static const int16_t k_tune_br_title_y[MODE_COUNT]  = TUNE_BR_TITLE_Y;
static const int16_t k_tune_br_artist_y[MODE_COUNT] = TUNE_BR_ARTIST_Y;
static const int16_t k_tune_title_lsp[MODE_COUNT]   = TUNE_TITLE_LETTER_SP;
static const int16_t k_tune_sel_dy[MODE_COUNT]      = TUNE_SEL_LINE_DY;
static const int16_t k_tune_fps_x[MODE_COUNT]       = TUNE_FPS_X;
static const int16_t k_tune_fps_y[MODE_COUNT]       = TUNE_FPS_Y;
static const int16_t k_tune_topbtn_y[MODE_COUNT]    = TUNE_TOPBTN_Y;
static const int16_t k_tune_np_title_y[MODE_COUNT]  = TUNE_NP_TITLE_Y;
static const int16_t k_tune_np_artist_y[MODE_COUNT] = TUNE_NP_ARTIST_Y;
static const int16_t k_tune_prog_y[MODE_COUNT]      = TUNE_PROG_Y;
static const int16_t k_tune_ts_w[MODE_COUNT]        = TUNE_TS_W;
static const int16_t k_tune_tkey_y[MODE_COUNT]      = TUNE_TKEY_Y;
static const int16_t k_tune_fader_x[MODE_COUNT]     = TUNE_FADER_X;
static const int16_t k_tune_fader_y[MODE_COUNT]     = TUNE_FADER_Y;
static const int16_t k_tune_fader_h[MODE_COUNT]     = TUNE_FADER_H;

/* Accent palette: a 4-hue x 3-variant grid (vivid/deep/soft rows). The hex
 * values are user-tweakable in ui_tune.h (TUNE_ACCENTS). */
#define ACCENT_COUNT TUNE_ACCENT_COUNT
static uint8_t s_accent = 8;   /* deep orange -- first swatch of the DEEP row */
static const uint32_t k_accents[ACCENT_COUNT] = TUNE_ACCENTS;
static uint32_t accent_color(void) { return k_accents[s_accent]; }

/* Browser styles:
 *  - CAROUSEL:  flat row, all cards same size (the original).
 *  - FOCUS:     centre card full size, side cards scale down + dim gently.
 *  - COVERFLOW: true 3D perspective tilt via a PSRAM column rasteriser.
 *               Each card is rendered as a trapezoid (left edge full height,
 *               right edge foreshortened) into a 800×296 pixel buffer that
 *               sits on top of the transparent LVGL scroller.  Centre card
 *               drawn last so it always appears in front of turned side covers.
 *               LVGL card objects are hidden; scroll physics use the flex layout
 *               unchanged (step = cs() + cg() = 110 px).
 * NVS persists the index; the old build's "Cover Flow" (index 1) is now FOCUS,
 * which is the correct migration -- index 1 was always the scale+dim mode. */
enum { BROWSER_CAROUSEL = 0, BROWSER_FOCUS = 1, BROWSER_COVERFLOW = 2,
       BROWSER_STYLE_COUNT = 3 };
static uint8_t s_browser_style = BROWSER_CAROUSEL;
static bool    s_show_sel_line = true;   /* centred-card underline (Settings toggle) */

/* Backlight brightness (BSP LEDC PWM, 0..100). Floored at BRIGHTNESS_MIN so the
 * screen can never be dimmed to fully black (which would read as a hang). The
 * value is NVS-persisted and re-applied at boot. */
#define BRIGHTNESS_MIN      10
#define BRIGHTNESS_MAX     100
#define BRIGHTNESS_DEFAULT 100
static uint8_t s_brightness = BRIGHTNESS_DEFAULT;

/* === Glyph-only integrated dot UI features === */

/* 1. Gas-particle progress bar: 24 dots confined to the played zone, bouncing
 *    elastically inside the expanding container as the song progresses. */
#define PROG_PART_COUNT  44
/* The dots live inside a black "tank" box with white walls that encloses the
 * thin progress bar (taller than the bar so the gas has headroom). */
#define PROG_TANK_H   (PROG_H + 38)
#define PROG_TANK_Y   (PROG_Y + PROG_H / 2 - PROG_TANK_H / 2)
typedef struct { int16_t x, y; int8_t vx, vy; } prog_pt_t;
static prog_pt_t   s_prog_pts[PROG_PART_COUNT]   = {0};
static lv_obj_t   *s_prog_objs[PROG_PART_COUNT]  = {0};
static lv_timer_t *s_prog_particle_timer         = NULL;
static lv_obj_t   *s_prog_tank                   = NULL;   /* dim-framed gas chamber */
static lv_obj_t   *s_prog_head                   = NULL;   /* bright playhead at the progress point */

/* 2. Volume page: full-screen dot-matrix volume display. */
#define VOL_PAGE_COLS   8
#define VOL_PAGE_ROWS  10
#define VOL_PAGE_DOTS  (VOL_PAGE_COLS * VOL_PAGE_ROWS)
static lv_obj_t *s_screen_volume   = NULL;
static lv_obj_t *s_vol_page_dots[VOL_PAGE_DOTS] = {0};
static lv_obj_t *s_vol_page_label  = NULL;   /* "XX%" readout */
static lv_timer_t *s_vol_release_timer = NULL;

/* 3. WiFi dot strength meter: 4 dots of rising size in the browser top-left
 *    corner; the first `s_wifi_dot_count` are lit in the accent, the rest dim.
 *    Static (no timer) -- replaces the rising bar indicators when Glyph active. */
static lv_obj_t   *s_wifi_dots[4]   = {0};
static int         s_wifi_dot_count = 0;

/* 4. Offline title dissipation: temporary dots that animate on offline transition. */
#define DISSOLVE_DOT_COUNT  8
static lv_obj_t *s_dissolve_dots[DISSOLVE_DOT_COUNT] = {0};

/* === PIXEL retro theme state ===
 * All pixelation is pre-computed once per art/thumbnail change; no per-frame cost.
 * Thumbnail pool lives in PSRAM, allocated when PIXEL activates, freed on switch-away.
 * Art buffer (8 KB) is allocated once and kept to avoid repeated alloc/free on art
 * changes.  s_last_raw_* caches the current raw art pointer/dims so apply_theme_cb
 * can re-pixelate immediately when switching into PIXEL without waiting for next poll.
 */
#define PIX_THUMB_RES  64                          /* logical pixels for browser thumbs */
#define PIX_ART_RES    64                          /* logical pixels for now-playing art */
static uint16_t       *s_pix_thumbs     = NULL;   /* PSRAM: s_card_count * PIX_THUMB_RES^2 */

/* PSRAM thumb pools. The embedded thumbs are flash rodata (EMBED_FILES), so
 * every blit -- LVGL card draws AND the Cover Flow rasteriser's per-column
 * sampling -- reads through the flash XIP cache. s_thumbs_psram (~5.4 MB) is
 * a one-time copy that takes that pressure off every browser style.
 * s_card_pool (~9.2 MB) goes further for Carousel/Focus: thumbs pre-scaled to
 * the card slot, so the centred-card blit is a plain 1:1 copy instead of a
 * per-pixel nearest resample on every frame. It is rewritten on each browser
 * build so it carries the current theme's look (PIXEL's chunky blocks
 * included). Either pool failing to allocate falls back to the old paths. */
static uint16_t       *s_thumbs_psram   = NULL;   /* raw ALBUM_THUMB_W^2 copies */
static uint16_t       *s_card_pool      = NULL;   /* CARD_SIZE^2, current theme */
static uint16_t       *s_pix_art_buf   = NULL;    /* PSRAM: PIX_ART_RES^2 px art scratch */
static lv_image_dsc_t  s_pix_art_dsc  = {0};
static const uint8_t  *s_last_raw_art  = NULL;    /* pointer into PSRAM art decode buf */
static uint16_t        s_last_raw_w    = 0;
static uint16_t        s_last_raw_h    = 0;

/* === PAPER data-brutalist theme state ===
 * Art + thumbs are reduced to TWO colours (paper + ink) by an 8x8 ordered
 * Bayer dither on luminance -- the printed-halftone look of the teletype /
 * data-sheet references. Pools mirror the PIXEL ones: rebuilt per browser
 * build / art change, freed on switch-away. The card pool re-dithers at card
 * resolution so the on-screen dither grain is 1:1 (never resampled). */
#define PAPER_ART_RES  256                        /* == ART_W: 1:1 in the art box, no resample */
static uint16_t       *s_paper_thumbs  = NULL;    /* PSRAM: dithered ALBUM_THUMB-res copies (CF source) */
static uint16_t       *s_paper_art_buf = NULL;    /* PSRAM: PAPER_ART_RES^2 dithered now-playing art */
static lv_image_dsc_t  s_paper_art_dsc = {0};
/* Ruler-tick progress: a printed tick scale under the bar with a solid ink
 * block riding the playhead (this theme's sibling of the GLYPH gas tank). */
#define PAPER_TICK_COUNT  41                      /* 13 px pitch across PROG_W, every 5th taller */
#define PAPER_TICK_PITCH  (PROG_W / (PAPER_TICK_COUNT - 1))
#define PAPER_CUR_W   5
#define PAPER_CUR_H   20
static lv_obj_t *s_paper_cursor   = NULL;         /* playhead block (child of the NP screen) */
static lv_obj_t *s_br_index_lbl   = NULL;         /* browser "NN / NN" album counter (PAPER) */

/* True for any style that transforms cards per scroll position (Focus + CF). */
#define BROWSER_STYLE_TRANSFORMS(s) ((s) == BROWSER_FOCUS || (s) == BROWSER_COVERFLOW)

/* Settings screen + its option rows (transition + mode + colour + browser). */
static lv_obj_t *s_screen_settings = NULL;
static lv_obj_t *s_opt_btns[UI_TRANSITION_COUNT]          = {0};
static lv_obj_t *s_opt_labels[UI_TRANSITION_COUNT]        = {0};
static lv_obj_t *s_theme_btns[MODE_COUNT]                 = {0};
static lv_obj_t *s_theme_labels[MODE_COUNT]               = {0};
static lv_obj_t *s_dl_btns[2]                             = {0};   /* DARK / LIGHT toggle */
static lv_obj_t *s_dl_labels[2]                           = {0};
static lv_obj_t *s_art_toggle_btn                         = NULL;  /* THEME ALBUM ART on/off */
static lv_obj_t *s_art_toggle_lbl                         = NULL;
static lv_obj_t *s_accent_btns[ACCENT_COUNT]             = {0};
static lv_obj_t *s_accent_labels[ACCENT_COUNT]           = {0};
static lv_obj_t *s_brstyle_btns[BROWSER_STYLE_COUNT]      = {0};
static lv_obj_t *s_brstyle_labels[BROWSER_STYLE_COUNT]    = {0};
static lv_obj_t *s_sel_line        = NULL;   /* the centred-card underline object */
static lv_obj_t *s_font_btns[2]   = {0};    /* Settings FONT row: SANS | SLAB */
static lv_obj_t *s_font_labels[2] = {0};

/* Device selector screen: a scrollable list rebuilt by ui_set_devices().
 * s_dev_entries caches the current rows so a row's click handler can look up its
 * id/host + kind by index (the button user_data is that index). */
static lv_obj_t   *s_screen_devices  = NULL;
static lv_obj_t   *s_dev_list        = NULL;
static ui_device_t s_dev_entries[MAX_DEVICES];
static int         s_dev_entry_count = 0;
static lv_obj_t *s_line_toggle_btn = NULL;   /* Settings ON/OFF toggle for it */
static lv_obj_t *s_line_toggle_lbl = NULL;
static lv_obj_t *s_brightness_slider = NULL;   /* Settings backlight slider */
static lv_obj_t *s_brightness_val    = NULL;   /* "NN%" label beside it */
static lv_obj_t *s_sound_toggle_btn  = NULL;   /* Settings UI-sound ON/OFF toggle */
static lv_obj_t *s_sound_toggle_lbl  = NULL;
static lv_obj_t *s_volume_slider     = NULL;   /* Settings UI-sound volume slider */
static lv_obj_t *s_volume_val        = NULL;   /* "NN%" label beside it */

/* Settings is split into category pages reached by a chip row at the top
 * (Appearance / Display / Sound). One page is visible at a time; the rest are
 * hidden. s_set_tab survives a theme rebuild so the active page is preserved. */
#define SET_TAB_COUNT  2
static lv_obj_t *s_set_tabs[SET_TAB_COUNT]     = {0};   /* category chip buttons */
static lv_obj_t *s_set_tab_lbls[SET_TAB_COUNT] = {0};
static lv_obj_t *s_set_pages[SET_TAB_COUNT]    = {0};   /* page containers */
static uint8_t   s_set_tab = 0;                         /* active category page */

/* SOUND SET selector: option 0 = AUTO (follow MODE), then one per named set. */
#define SND_SET_OPTS 8
static lv_obj_t *s_sndset_btns[SND_SET_OPTS] = {0};
static lv_obj_t *s_sndset_lbls[SND_SET_OPTS] = {0};
static int       s_sndset_opt_count          = 0;

/* Live FPS counter -- browser top-bar label updated every 1 s. */
static lv_obj_t  *s_fps_label      = NULL;
static lv_obj_t  *s_fps_toggle_btn = NULL;
static lv_obj_t  *s_fps_toggle_lbl = NULL;
static bool       s_fps_enabled    = false;
/* FPS = frames actually presented while the UI is animating. Each RENDER_READY
 * is one presented frame; consecutive frames closer than FPS_BURST_GAP_US form
 * a "burst" (a scroll, a transition, the GLYPH gas tick...). Rate = frame
 * intervals / elapsed time across the window's bursts, so it includes
 * EVERYTHING the eye waits on -- input/timer handlers (e.g. the Cover Flow
 * rasteriser runs in the scroll handler), render, rotation/flush, and the
 * LV_DEF_REFR_PERIOD cap. The old metric (1e6 / longest render) ignored all
 * cost outside the render pass and so over-read exactly when it mattered.
 * Held across idle windows so the readout stays meaningful when still.
 * NOTE: never call lv_*_invalidate() from a render event -- it asserts
 * (rendering_in_progress) and wedges the LVGL task. Timing only here. */
#define FPS_BURST_GAP_US 150000   /* frames further apart than this start a new burst */
static int64_t    s_fps_prev_ready_us  = 0;   /* last RENDER_READY timestamp */
static int64_t    s_fps_burst_start_us = 0;   /* first frame of the running burst */
static uint32_t   s_fps_burst_frames   = 0;   /* frames in the running burst */
static uint32_t   s_fps_acc_intervals  = 0;   /* closed-burst frame intervals this window */
static uint32_t   s_fps_acc_span_us    = 0;   /* closed-burst elapsed time this window */
static uint32_t   s_fps_last_rate      = 0;   /* last computed FPS (held on idle) */

/* CF perspective canvas: SCREEN_W × CF_PERSP_H PSRAM RGB565 buffer rasterized
 * on each scroll event.  Only allocated when BROWSER_COVERFLOW is active.
 * Pinned just below the top strip (CF_PERSP_Y) and grown downward so the
 * enlarged centre cover (CF_CARD_SCALE × the 220px thumb) fills the space down
 * to CF_TITLE_Y -- bottom (CF_PERSP_Y + CF_PERSP_H = 326) stays clear of it. */
#define CF_PERSP_W   SCREEN_W    /* 800 */
#define CF_PERSP_H   296
#define CF_PERSP_Y   30
static uint16_t       *s_cf_buf = NULL;
static lv_image_dsc_t  s_cf_dsc = {0};
static lv_obj_t       *s_cf_img = NULL;

#define NVS_SETTINGS_NS       "settings"
#define NVS_KEY_TRANSITION    "transition"
/* "ui_mode" stores the 4-value MODE (BASIC/GLYPH/PIXEL/PAPER); the old
 * 7-value "theme" key is retired (its indices don't map; defaults apply once). */
#define NVS_KEY_MODE          "ui_mode"
#define NVS_KEY_DARK          "ui_dark"
#define NVS_KEY_THEME_ART     "ui_themeart"
#define NVS_KEY_ACCENT        "accent"
#define NVS_KEY_BROWSER_STYLE "browser_style"
#define NVS_KEY_SEL_LINE      "sel_line"
#define NVS_KEY_BRIGHTNESS    "brightness"
#define NVS_KEY_FONT          "font"
#define NVS_KEY_FPS           "fps_disp"
#define NVS_KEY_SOUND         "ui_sound"
#define NVS_KEY_VOLUME        "ui_vol"
#define NVS_KEY_SOUND_SET     "ui_sndset"

static const char *const k_transition_names[UI_TRANSITION_COUNT] = {
    "OVER (SLIDE)", "MOVE (PUSH)", "FADE", "NONE (INSTANT)",
};
static const char *const k_mode_names[MODE_COUNT] = { "BASIC", "GLYPH", "PIXEL", "PAPER" };
static const char *const k_darklight_names[2]     = { "DARK", "LIGHT" };
static const char *const k_browser_style_names[BROWSER_STYLE_COUNT] = { "CAROUSEL", "FOCUS", "COVER FLOW" };

/* Cached track state. The LVGL progress timer reads progress_ms /
 * duration_ms / is_playing from here and ticks the bar between
 * Spotify polls so the bar moves at ~5 Hz instead of 0.2 Hz. */
static spotify_track_t s_track = {0};
static uint32_t        s_last_progress_tick = 0; /* lv_tick_get() at last server sync */

static void on_gesture(lv_event_t *e);
static void on_card_clicked(lv_event_t *e);
static void on_browser_scroll(lv_event_t *e);
static void progress_timer_cb(lv_timer_t *t);
static void update_progress_bar(void);
static void on_open_settings(lv_event_t *e);
static void on_settings_back(lv_event_t *e);
static void on_transition_option(lv_event_t *e);
static void on_theme_option(lv_event_t *e);
static void on_darklight_option(lv_event_t *e);
static void on_art_toggle(lv_event_t *e);
static void on_accent_option(lv_event_t *e);
static void on_np_tap(lv_event_t *e);
static void vol_hud_show(int pct, bool muted);
static void on_seek_start(lv_event_t *e);
static void on_seek_pressing(lv_event_t *e);
static void on_seek_released(lv_event_t *e);
static void on_seek_click_absorb(lv_event_t *e);
static void on_remain_tap(lv_event_t *e);
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
static void save_dark(uint8_t v);
static void save_theme_art(uint8_t v);
static void save_accent(uint8_t idx);
static void save_browser_style(uint8_t idx);
static void save_sel_line(uint8_t v);
static void save_brightness(uint8_t v);
static void save_font(uint8_t v);
static void on_browser_style_option(lv_event_t *e);
static void on_line_toggle(lv_event_t *e);
static void refresh_line_selection(void);
static void on_font_option(lv_event_t *e);
static void refresh_font_selection(void);
static void on_fps_toggle(lv_event_t *e);
static void refresh_fps_selection(void);
static void on_sound_toggle(lv_event_t *e);
static void refresh_sound_selection(void);
static void save_sound(uint8_t v);
static void on_volume_changed(lv_event_t *e);
static void on_volume_released(lv_event_t *e);
static void save_volume(uint8_t v);
static void apply_audio_theme(void);
static void on_settings_tab(lv_event_t *e);
static void refresh_settings_tabs(void);
static void on_sound_set_option(lv_event_t *e);
static void refresh_sound_set_selection(void);
static void save_sound_set(int8_t v);
static void fps_render_ready_cb(lv_event_t *e);
static void fps_timer_cb(lv_timer_t *t);
static void cf_init(lv_obj_t *screen);
static void cf_deinit(void);
static void cf_render(void);
static void on_brightness_changed(lv_event_t *e);
static void on_brightness_released(lv_event_t *e);
static void idle_timer_cb(lv_timer_t *t);
static void on_hint_to_np(lv_event_t *e);
static void on_hint_to_browser(lv_event_t *e);
static void on_open_devices(lv_event_t *e);
static void on_devices_back(lv_event_t *e);
static void on_device_tap(lv_event_t *e);
static void build_devices_screen(void);
static void on_transport_prev(lv_event_t *e);
static void on_transport_toggle(lv_event_t *e);
static void on_transport_next(lv_event_t *e);
static void on_vol_changed(lv_event_t *e);
static void on_vol_released(lv_event_t *e);
static void on_vol_press(lv_event_t *e);
static void on_vol_press_lost(lv_event_t *e);
static void refresh_play_icon(void);
static void position_seek_thumb(int32_t pct);
static bool is_glyph_theme(void);
static bool is_pixel_theme(void);
static bool is_paper_theme(void);
static const lv_font_t *font_lg(void);
static const lv_font_t *font_md(void);
static const lv_font_t *font_sm(void);
/* GLYPH heading font: round-dot matrix (Nothing-style), used by font_lg only.
 * Its fallback chain (dot_24 -> dot_sym_24 -> montserrat) keeps symbols and
 * accented glyphs rendering inside dotted headings. */
extern const lv_font_t lv_font_dot_24;
static void pixelate_rgb565(const uint16_t *src, uint16_t sw, uint16_t sh,
                             uint16_t *dst, uint16_t dw, uint16_t dh);
static void paperize_rgb565(const uint16_t *src, uint16_t sw, uint16_t sh,
                            uint16_t *dst, uint16_t dw, uint16_t dh);
static lv_obj_t *paper_rule(lv_obj_t *parent, int x, int y, int w, int h);
/* Gas-particle progress bar */
static void prog_particles_start(lv_obj_t *screen);
static void prog_particles_stop(void);
static void prog_particle_tick_cb(lv_timer_t *t);
/* Volume page */
static void build_volume_screen(void);
static void vol_page_dots_update(int pct);
static void on_open_volume(lv_event_t *e);
static void on_vol_page_back(lv_event_t *e);
static void on_vol_page_drag(lv_event_t *e);
static void vol_release_timer_cb(lv_timer_t *t);
/* WiFi dot strength meter */
static void wifi_dots_start(lv_obj_t *screen);
static void wifi_dots_stop(void);
static void wifi_dots_update_count(int bars);
/* Offline dissolve */
static void title_dissolve(void);
static void title_reform(void);
static void dissolve_done_cb(lv_anim_t *a);

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

/* Carousel/Focus use the same 220px slot + 28px gap. Cover Flow only uses
 * cs()/cg() to drive the scroll step + snap points -- the covers themselves are
 * drawn by the PSRAM rasteriser (cf_render), not by these flex slots (which are
 * hidden in CF mode). cs=165 + cg=-55 give a 110px step -- smaller than a turned
 * side cover (~143px wide at full tilt), so every side cover overlaps its
 * neighbour (no gaps, even at the extremes) and they crowd in toward the centre
 * iPod-style. cf_render draws far->near so nearer covers paint on top. */
static int cs(void) { return (s_browser_style == BROWSER_COVERFLOW) ? 165 : CARD_SIZE; }
static int cg(void) { return (s_browser_style == BROWSER_COVERFLOW) ? -55 : CARD_GAP; }

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

/* Pressed-state feedback: flash the fill to the accent while the button is
 * held, for a tactile cue. NOTE: object-level transform_scale (the obvious
 * "shrink on press") is unsafe on this board -- it forces a layer the
 * DIRECT-mode rotated DSI flush mis-composites (same reason cards use
 * image-direct transforms). A colour flash needs no layer. */
static void style_button_press(lv_obj_t *btn)
{
    lv_obj_set_style_bg_color(btn, lv_color_hex(accent_color()),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_PRESSED);
}

/* Selected-option colours for the settings pills/tabs. Accent fill + black
 * text normally; GLYPH (Nothing-style light) selects with a solid INK pill +
 * light text -- like the reference's filled "SIMPLE" chip -- keeping the
 * accent for live elements only (playhead, selection line). */
static lv_color_t opt_sel_bg(void)
{
    return lv_color_hex(is_glyph_theme() ? s_th->text : accent_color());
}
static lv_color_t opt_sel_fg(void)
{
    if (is_glyph_theme()) return lv_color_hex(s_th->bg);
    /* Black ink on the bright accents (TE-style black-on-orange); white on the
     * darker red/purple inks where black would lose contrast. */
    uint32_t  c    = accent_color();
    unsigned  luma = (299u * ((c >> 16) & 0xFF) + 587u * ((c >> 8) & 0xFF) +
                      114u * (c & 0xFF)) / 1000u;
    return (luma >= 120u) ? lv_color_black() : lv_color_white();
}

/* Shared flat-key styling for every boxy button (settings rows, tabs, back
 * keys, transport keys). Radius-3 charcoal keys normally; PAPER squares them
 * off and frames each in a thin ink border (ruled data-sheet cells); GLYPH
 * rounds them into full pills with a hairline outline (Nothing-style). */
static void style_key_btn(lv_obj_t *btn)
{
    lv_obj_set_style_radius(btn,
        is_paper_theme() ? 0 : is_glyph_theme() ? LV_RADIUS_CIRCLE : 3, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    if (is_paper_theme()) {
        lv_obj_set_style_border_width(btn, 2, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(s_th->text), 0);
        lv_obj_set_style_border_opa(btn, LV_OPA_COVER, 0);
    } else if (is_glyph_theme()) {
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(s_th->track), 0);
        lv_obj_set_style_border_opa(btn, LV_OPA_COVER, 0);
    }
}

/* A printed hairline: 1px (or thicker) ink rule used to divide the PAPER
 * screens into form zones. Inert to input. */
static lv_obj_t *paper_rule(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_set_size(r, w, h);
    lv_obj_set_pos(r, x, y);
    lv_obj_set_style_radius(r, 0, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_pad_all(r, 0, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(s_th->text), 0);
    lv_obj_set_style_bg_opa(r, (lv_opa_t)200, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return r;
}

/* Full-screen ink frame: the printed-form outer border every PAPER screen
 * gets. Transparent fill, border only, inert to input -- children created
 * after it draw on top. */
static void paper_frame(lv_obj_t *screen)
{
    lv_obj_t *f = lv_obj_create(screen);
    lv_obj_set_size(f, SCREEN_W - 8, SCREEN_H - 8);
    lv_obj_set_pos(f, 4, 4);
    lv_obj_set_style_radius(f, 0, 0);
    lv_obj_set_style_bg_opa(f, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(f, 2, 0);
    lv_obj_set_style_border_color(f, lv_color_hex(s_th->text), 0);
    lv_obj_set_style_border_opa(f, LV_OPA_COVER, 0);
    lv_obj_remove_flag(f, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
}

/* PAPER screen titles print as inverted ink chips (8562-style title bars):
 * ink slab behind paper-coloured letter-spaced text. No-op in other themes. */
static void paper_title_chip(lv_obj_t *title)
{
    if (!is_paper_theme()) return;
    lv_obj_set_style_text_color(title, lv_color_hex(s_th->bg), 0);
    lv_obj_set_style_bg_color(title, lv_color_hex(s_th->text), 0);
    lv_obj_set_style_bg_opa(title, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(title, 10, 0);
    lv_obj_set_style_pad_ver(title, 3, 0);
}

/* Tiny tracked-out uppercase field label in the accent -- the "APPROXIMATE
 * TIME OF DELIVERY" corner labels of the reference sheets. PAPER chrome. */
static lv_obj_t *paper_field_label(lv_obj_t *parent, const char *txt, int x, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(accent_color()), 0);
    lv_obj_set_style_text_font(l, font_sm(), 0);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

/* Three small vertical faders (mixer look) drawn from rects -- a "controls"
 * glyph for the settings button. Avoids embedding a new symbol font. The button
 * must have pad_all 0 so the TOP_LEFT-aligned children sit at known offsets. */
/* Shared "hint pill": a tappable rounded chip with a chevron + letter-spaced
 * uppercase label. Used at the bottom of the browser ("^ NOW PLAYING") and the
 * top of now-playing ("v ALBUMS") so the two navigation affordances match. */
static lv_obj_t *make_hint_pill(lv_obj_t *parent, const char *txt, lv_event_cb_t cb)
{
    lv_obj_t *pill = lv_button_create(parent);
    lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    /* PAPER: the pill prints as an inverted ink tab (square, ink slab, paper
     * text) sitting on the screen's rules -- not a floating rounded chip. */
    lv_obj_set_style_bg_color(pill,
        lv_color_hex(is_paper_theme() ? s_th->text : s_th->surface), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pill, is_paper_theme() ? 0 : 14, 0);
    /* GLYPH: hairline-outlined pill on the light ground (Nothing-style). */
    if (is_glyph_theme()) {
        lv_obj_set_style_border_width(pill, 1, 0);
        lv_obj_set_style_border_color(pill, lv_color_hex(s_th->track), 0);
        lv_obj_set_style_border_opa(pill, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_border_width(pill, 0, 0);
    }
    lv_obj_set_style_shadow_width(pill, 0, 0);
    lv_obj_set_style_pad_hor(pill, 14, 0);
    lv_obj_set_style_pad_ver(pill, 5, 0);
    style_button_press(pill);
    lv_obj_add_event_cb(pill, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(pill);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_color(lbl,
        lv_color_hex(is_paper_theme() ? s_th->bg : s_th->text2), 0);
    lv_obj_set_style_text_font(lbl, font_sm(), 0);
    lv_obj_set_style_text_letter_space(lbl, 2, 0);
    lv_obj_center(lbl);
    return pill;
}

/* Raw thumb source: the PSRAM copy when available, else the flash original. */
static const uint16_t *thumb_src(size_t i)
{
    if (s_thumbs_psram)
        return s_thumbs_psram + i * ALBUM_THUMB_W * ALBUM_THUMB_H;
    return album_thumb_data(i);
}

/* Nearest-neighbour RGB565 resize (16.16 fixed-point stepping, no per-pixel
 * divide). Same hard-edged look as LVGL's antialias=false image transform,
 * which is what the per-frame path used before the pre-scaled pool. */
static void nearest_resize_rgb565(const uint16_t *src, int sw, int sh,
                                  uint16_t *dst, int dw, int dh)
{
    uint32_t xstep = ((uint32_t)sw << 16) / (uint32_t)dw;
    uint32_t ystep = ((uint32_t)sh << 16) / (uint32_t)dh;
    uint32_t yacc  = 0;
    for (int y = 0; y < dh; y++, yacc += ystep) {
        const uint16_t *srow = src + (size_t)(yacc >> 16) * (size_t)sw;
        uint16_t       *drow = dst + (size_t)y * (size_t)dw;
        uint32_t xacc = 0;
        for (int x = 0; x < dw; x++, xacc += xstep) {
            drow[x] = srow[xacc >> 16];
        }
    }
}

/* 0xRRGGBB -> RGB565. (The same packing is inlined in the dither paths; this is
 * the named version for the frame/clear helpers.) */
static inline uint16_t rgb888_to_565(uint32_t c)
{
    return (uint16_t)((((c >> 19) & 0x1Fu) << 11) |
                      (((c >> 10) & 0x3Fu) <<  5) |
                       ((c >>  3) & 0x1Fu));
}

/* Draw a `t`px frame of `px` into an RGB565 tile, in place. Baking the frame
 * into the card-pool PIXELS (rather than a border on the card object) means it
 * scales WITH the art in Focus mode -- a container border stayed full-size
 * while the art shrank. Used for the PAPER ink frame and the playing accent. */
static void frame_rgb565(uint16_t *buf, int w, int h, uint16_t px, int t)
{
    for (int y = 0; y < h; y++) {
        bool edge_row = (y < t) || (y >= h - t);
        uint16_t *row = buf + (size_t)y * w;
        if (edge_row) {
            for (int x = 0; x < w; x++) row[x] = px;
        } else {
            for (int x = 0; x < t; x++)       row[x]         = px;
            for (int x = 0; x < t; x++)       row[w - 1 - x] = px;
        }
    }
}

/* Fill one CARD_SIZE pool tile with album i's art in the active theme look,
 * then bake the frames (PAPER ink on every cover, accent on the playing one).
 * Used by the pool build and by the per-tile re-bake on track change. */
static void fill_card_tile(size_t i, uint16_t *dst)
{
    if (is_pixel_theme() && s_theme_art && s_pix_thumbs) {
        nearest_resize_rgb565(s_pix_thumbs + i * PIX_THUMB_RES * PIX_THUMB_RES,
                              PIX_THUMB_RES, PIX_THUMB_RES, dst, CARD_SIZE, CARD_SIZE);
    } else if (is_paper_theme() && s_theme_art) {
        /* Dither AT card resolution (resample inside paperize) so the on-screen
         * halftone grain is exactly 1px -- upscaling a pre-dithered thumb smears. */
        const uint16_t *t = thumb_src(i);
        if (t) paperize_rgb565(t, ALBUM_THUMB_W, ALBUM_THUMB_H, dst, CARD_SIZE, CARD_SIZE);
        else   memset(dst, 0, (size_t)CARD_SIZE * CARD_SIZE * sizeof(uint16_t));
    } else {
        const uint16_t *t = thumb_src(i);
        if (t) nearest_resize_rgb565(t, ALBUM_THUMB_W, ALBUM_THUMB_H, dst, CARD_SIZE, CARD_SIZE);
        else   memset(dst, 0, (size_t)CARD_SIZE * CARD_SIZE * sizeof(uint16_t));
    }
    if (is_paper_theme())
        frame_rgb565(dst, CARD_SIZE, CARD_SIZE, rgb888_to_565(s_th->text), 2);
    if ((int)i == s_playing_card_idx)
        frame_rgb565(dst, CARD_SIZE, CARD_SIZE, rgb888_to_565(accent_color()), 3);
}

/* Re-bake one card's pool tile + repaint it -- used when the playing album
 * changes so its accent frame (baked into the pixels, so it scales in Focus)
 * moves to the new card without a full browser rebuild. */
static void rebake_card_tile(size_t i)
{
    if (!s_card_pool || i >= s_card_count || !s_card_imgs[i]) return;
    fill_card_tile(i, s_card_pool + i * (size_t)CARD_SIZE * CARD_SIZE);
    lv_obj_invalidate(s_card_imgs[i]);
}

static void build_browser_screen(void)
{
    /* Cards are recreated below: invalidate the FOCUS transform cache so the
     * first apply_card_transforms pass writes every card once. */
    for (size_t i = 0; i < MAX_CARDS; i++) {
        s_card_scale_last[i] = -1;
        s_card_dim_last[i]   = -1;
    }

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
    if (s_card_count > MAX_CARDS) {
        size_t total = albums_count();
        ESP_LOGW(TAG, "album list has %u entries but MAX_CARDS is %d; showing first %d",
                 (unsigned)total, MAX_CARDS, MAX_CARDS);
        s_card_count = MAX_CARDS;
        /* Surface the silent truncation on-screen (top-left, below the WiFi
         * bars and clear of the gear/devices buttons on the right). */
        char warn[40];
        snprintf(warn, sizeof warn, "+%u more (raise MAX_CARDS)",
                 (unsigned)(total - MAX_CARDS));
        lv_obj_t *wlbl = lv_label_create(s_screen_browser);
        lv_label_set_text(wlbl, warn);
        lv_obj_set_style_text_color(wlbl, lv_color_hex(0xFFA000), 0);
        lv_obj_set_style_text_font(wlbl, font_sm(), 0);
        lv_obj_align(wlbl, LV_ALIGN_TOP_LEFT, 60, 4);
    }

    /* Debug: print thumbnail / album counts and a few thumb pointers to help
     * diagnose missing/corrupt embedded blobs when Cover Flow shows blank cards. */
    ESP_LOGI(TAG, "albums_count=%zu s_card_count=%zu album_thumb_count=%zu ALBUM_THUMB=%dx%d browser_style=%s",
             albums_count(), s_card_count, album_thumb_count(), (int)ALBUM_THUMB_W, (int)ALBUM_THUMB_H,
             k_browser_style_names[s_browser_style]);
    for (size_t __j = 0; __j < (s_card_count < 4 ? s_card_count : 4); __j++) {
        const uint16_t *__t = album_thumb_data(__j);
        ESP_LOGD(TAG, "thumb[%zu]=%p", __j, (const void *)__t);
    }

    /* One-time PSRAM copy of the raw embedded thumbs (see the pool note at the
     * declarations). Feeds the Cover Flow rasteriser, the PIXEL pixelate pass
     * and the card pool below without flash XIP reads. */
    if (!s_thumbs_psram && s_card_count > 0) {
        size_t raw_sz = s_card_count * ALBUM_THUMB_BYTES;
        s_thumbs_psram = heap_caps_malloc(raw_sz, MALLOC_CAP_SPIRAM);
        if (s_thumbs_psram) {
            for (size_t i = 0; i < s_card_count; i++) {
                uint16_t *dst = s_thumbs_psram + i * ALBUM_THUMB_W * ALBUM_THUMB_H;
                const uint16_t *t = album_thumb_data(i);
                if (t) memcpy(dst, t, ALBUM_THUMB_BYTES);
                else   memset(dst, 0, ALBUM_THUMB_BYTES);
            }
        } else {
            ESP_LOGW(TAG, "thumb PSRAM pool alloc failed (%zu B)", raw_sz);
        }
    }

    /* PIXEL theme: pre-pixelate all thumbnails into PSRAM pool (once here;
     * no per-scroll work).  Free of any previous pool already done by
     * apply_theme_cb before this call; allocate fresh here. */
    if (is_pixel_theme() && s_theme_art) {
        size_t pix_pool_sz = s_card_count * PIX_THUMB_RES * PIX_THUMB_RES * sizeof(uint16_t);
        s_pix_thumbs = heap_caps_malloc(pix_pool_sz, MALLOC_CAP_SPIRAM);
        if (s_pix_thumbs) {
            for (size_t __pi = 0; __pi < s_card_count; __pi++) {
                const uint16_t *__src = thumb_src(__pi);
                if (__src) {
                    pixelate_rgb565(__src, ALBUM_THUMB_W, ALBUM_THUMB_H,
                                    s_pix_thumbs + __pi * PIX_THUMB_RES * PIX_THUMB_RES,
                                    PIX_THUMB_RES, PIX_THUMB_RES);
                }
            }
        } else {
            ESP_LOGW(TAG, "PIXEL: thumb pool alloc failed (%zu B SPIRAM)", pix_pool_sz);
        }
    }

    /* PAPER theme: pre-dither every thumbnail to ink-on-paper duotone at the
     * raw thumb resolution. Cover Flow and the pool-failure fallback sample
     * these; Carousel/Focus get a sharper re-dither at card size below. */
    if (is_paper_theme() && s_theme_art && s_card_count > 0) {
        size_t pool_sz = s_card_count * ALBUM_THUMB_BYTES;
        s_paper_thumbs = heap_caps_malloc(pool_sz, MALLOC_CAP_SPIRAM);
        if (s_paper_thumbs) {
            for (size_t i = 0; i < s_card_count; i++) {
                const uint16_t *src = thumb_src(i);
                if (src) {
                    paperize_rgb565(src, ALBUM_THUMB_W, ALBUM_THUMB_H,
                                    s_paper_thumbs + i * ALBUM_THUMB_W * ALBUM_THUMB_H,
                                    ALBUM_THUMB_W, ALBUM_THUMB_H);
                }
            }
        } else {
            ESP_LOGW(TAG, "PAPER: thumb pool alloc failed (%zu B SPIRAM)", pool_sz);
        }
    }

    /* Card-native pool: every thumb pre-scaled to CARD_SIZE with this theme's
     * look, so Carousel/Focus card images blit 1:1 (scale == LV_SCALE_NONE)
     * instead of resampling 220 -> 286 on every frame. Allocated once,
     * rewritten per build (theme look can change); also built in Cover Flow
     * style so a later style switch needs no special casing (its images are
     * hidden there). */
    if (s_card_count > 0) {
        size_t pool_sz = s_card_count * (size_t)CARD_SIZE * CARD_SIZE * sizeof(uint16_t);
        if (!s_card_pool)
            s_card_pool = heap_caps_malloc(pool_sz, MALLOC_CAP_SPIRAM);
        if (s_card_pool) {
            for (size_t i = 0; i < s_card_count; i++)
                fill_card_tile(i, s_card_pool + i * (size_t)CARD_SIZE * CARD_SIZE);
        } else {
            ESP_LOGW(TAG, "card pool alloc failed (%zu B SPIRAM)", pool_sz);
        }
    }

    for (size_t i = 0; i < s_card_count; i++) {
        const album_entry_t *a    = albums_get(i);
        const uint16_t      *thumb = album_thumb_data(i);

        lv_obj_t *card = lv_obj_create(s_browser_scroller);
        lv_obj_set_size(card, cs(), cs());
        lv_obj_set_style_radius(card, 0, 0);
        /* Frames (PAPER ink + playing accent) are baked into the card-pool
         * pixels above, so they scale with the art in Focus mode -- no object
         * border here when the pool is live. The container border is the
         * FALLBACK only (pool alloc failed); it stays full-size in Focus, but
         * that path is rare. */
        if (s_card_pool) {
            lv_obj_set_style_border_width(card, 0, 0);
        } else if ((int)i == s_playing_card_idx) {
            lv_obj_set_style_border_color(card, lv_color_hex(accent_color()), 0);
            lv_obj_set_style_border_width(card, 3, 0);
        } else if (is_paper_theme()) {
            lv_obj_set_style_border_color(card, lv_color_hex(s_th->text), 0);
            lv_obj_set_style_border_width(card, 2, 0);
        } else {
            lv_obj_set_style_border_width(card, 0, 0);
        }
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
            /* Preferred source is the card-native pool: 1:1 pixels at the
             * Carousel/Focus slot size, current theme look baked in. Fallbacks
             * keep the old per-frame-scaled sources if the pool failed. */
            if (s_card_pool) {
                s_card_dscs[i].header.cf  = LV_COLOR_FORMAT_RGB565;
                s_card_dscs[i].header.w   = CARD_SIZE;
                s_card_dscs[i].header.h   = CARD_SIZE;
                s_card_dscs[i].data       = (const uint8_t *)(s_card_pool + i * (size_t)CARD_SIZE * CARD_SIZE);
                s_card_dscs[i].data_size  = (uint32_t)CARD_SIZE * CARD_SIZE * sizeof(uint16_t);
            } else if (is_pixel_theme() && s_pix_thumbs) {
                const uint16_t *pix = s_pix_thumbs + i * PIX_THUMB_RES * PIX_THUMB_RES;
                s_card_dscs[i].header.cf  = LV_COLOR_FORMAT_RGB565;
                s_card_dscs[i].header.w   = PIX_THUMB_RES;
                s_card_dscs[i].header.h   = PIX_THUMB_RES;
                s_card_dscs[i].data       = (const uint8_t *)pix;
                s_card_dscs[i].data_size  = PIX_THUMB_RES * PIX_THUMB_RES * sizeof(uint16_t);
            } else if (is_paper_theme() && s_paper_thumbs) {
                const uint16_t *pap = s_paper_thumbs + i * ALBUM_THUMB_W * ALBUM_THUMB_H;
                s_card_dscs[i].header.cf  = LV_COLOR_FORMAT_RGB565;
                s_card_dscs[i].header.w   = ALBUM_THUMB_W;
                s_card_dscs[i].header.h   = ALBUM_THUMB_H;
                s_card_dscs[i].data       = (const uint8_t *)pap;
                s_card_dscs[i].data_size  = ALBUM_THUMB_BYTES;
            } else {
                s_card_dscs[i].header.cf  = LV_COLOR_FORMAT_RGB565;
                s_card_dscs[i].header.w   = ALBUM_THUMB_W;
                s_card_dscs[i].header.h   = ALBUM_THUMB_H;
                s_card_dscs[i].data       = (const uint8_t *)thumb;
                s_card_dscs[i].data_size  = ALBUM_THUMB_BYTES;
            }
            lv_obj_t *img = lv_image_create(card);
            lv_image_set_src(img, &s_card_dscs[i]);
            lv_obj_center(img);
            /* Fill the cs() card slot from whatever source landed in the dsc
             * (scale about the centre so side covers shrink inward). With the
             * card pool that ratio is exactly LV_SCALE_NONE in Carousel/Focus
             * -- the fast untransformed blit path. FOCUS overrides per-scroll
             * in apply_card_transforms; Cover Flow hides these images. */
            lv_image_set_pivot(img, s_card_dscs[i].header.w / 2, s_card_dscs[i].header.h / 2);
            lv_image_set_scale(img, (uint32_t)cs() * LV_SCALE_NONE / s_card_dscs[i].header.w);
            /* Nearest-neighbour: no anti-aliased (bilinear) resample, which
             * is ~4x the per-pixel cost -- the main reason FOCUS was ~2x
             * slower. Only the scaled side covers lose a little smoothness. */
            lv_image_set_antialias(img, false);
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
            lv_obj_set_style_text_font(letter, font_lg(), 0);
            lv_obj_center(letter);
        }

        s_cards[i] = card;
    }

    bool cf = (s_browser_style == BROWSER_COVERFLOW);
    s_browser_title = lv_label_create(s_screen_browser);
    style_label(s_browser_title, font_lg(),
                lv_color_hex(s_th->text), cf ? CF_TITLE_Y : BR_TITLE_Y);
    lv_obj_set_style_text_letter_space(s_browser_title, k_tune_title_lsp[s_mode], 0);
    /* Titles that fit stay centred; ones too wide for the screen scroll
     * horizontally (radio-style marquee) instead of being clipped/ellipsised.
     * Speed must be set BEFORE set_long_mode -- that call creates the scroll
     * animation and reads the style at that moment. NB: lv_anim_speed() caps the
     * duration at ~10.23 s (encoding limit), same as LVGL's default, so it can't
     * slow a long title down -- use a plain fixed duration (ms) per traversal. */
    lv_obj_set_style_anim_duration(s_browser_title, 150000, LV_PART_MAIN); /* 150 s/loop, very slow */
    lv_label_set_long_mode(s_browser_title, LV_LABEL_LONG_SCROLL_CIRCULAR);

    s_browser_artist = lv_label_create(s_screen_browser);
    style_label(s_browser_artist, font_md(),
                lv_color_hex(s_th->text2), cf ? CF_ARTIST_Y : BR_ARTIST_Y);

    if (s_card_count > 0) {
        const album_entry_t *a = albums_get(0);
        lv_label_set_text(s_browser_title, a->title);
        lv_label_set_text(s_browser_artist, a->artist);
        s_centered_card = 0;
    } else {
        /* No albums configured -- explain rather than show a blank carousel. */
        lv_label_set_text(s_browser_title, "No albums configured");
        lv_label_set_text(s_browser_artist,
                          "edit spotify-albums-list.txt + reflash");
    }

    /* CF perspective mode: make scroller transparent, hide the LVGL card images
     * (scroll physics still use the flex layout), then init the PSRAM canvas.
     * Must happen before apply_card_transforms() which calls cf_render().
     * The canvas is added as a child here so the selection line / wifi bars
     * created after it have a higher z-index and draw on top. */
    if (s_browser_style == BROWSER_COVERFLOW) {
        lv_obj_set_style_bg_opa(s_browser_scroller, LV_OPA_TRANSP, 0);
        for (size_t i = 0; i < s_card_count; i++) {
            if (s_card_imgs[i])
                lv_obj_add_flag(s_card_imgs[i], LV_OBJ_FLAG_HIDDEN);
            /* The containers stay (they ARE the scroll/flex layout), but go
             * fully invisible: the PAPER 2px ink frame / playing-album accent
             * border otherwise shows as a fixed-size box that doesn't follow
             * the rasterised covers (the canvas frames covers itself now). */
            if (s_cards[i]) {
                lv_obj_set_style_border_width(s_cards[i], 0, 0);
                lv_obj_set_style_bg_opa(s_cards[i], LV_OPA_TRANSP, 0);
            }
        }
        cf_init(s_screen_browser);
    }
    /* Apply initial transforms (CF calls cf_render; Focus uses image scales). */
    apply_card_transforms();

    /* PAPER printed-form furniture. The frame is created after the scroller so
     * its border paints over cards sliding past the screen edge -- covers run
     * "under" the printed frame. The index counter is the data-sheet "NN / NN"
     * record readout, kept current by on_browser_scroll. */
    s_br_index_lbl = NULL;
    if (is_paper_theme()) {
        paper_frame(s_screen_browser);
        /* Header band is a full-height strip (frame top -> y40) so the cog /
         * devices icons fit whole inside it; the rule prints below them, over
         * the empty band above the covers (cards start ~y68). */
        paper_rule(s_screen_browser, 8, TUNE_PAPER_RULE_Y, SCREEN_W - 16, 1);
        s_br_index_lbl = lv_label_create(s_screen_browser);
        lv_obj_set_style_text_color(s_br_index_lbl, lv_color_hex(accent_color()), 0);
        lv_obj_set_style_text_font(s_br_index_lbl, font_sm(), 0);
        lv_obj_set_style_text_letter_space(s_br_index_lbl, 2, 0);
        lv_obj_align(s_br_index_lbl, LV_ALIGN_TOP_MID, 0, 8);  /* clear of the y4 frame border */
        char ib[20];
        snprintf(ib, sizeof ib, "%02d / %02d", s_card_count ? 1 : 0, (int)s_card_count);
        lv_label_set_text(s_br_index_lbl, ib);
    }

    /* Selection marker: a short accent underline fixed beneath the centred card
     * slot (cards always centre-snap to the screen middle), so the active album
     * is unambiguous even in flat Carousel mode. Toggleable in Settings. */
    s_sel_line = lv_obj_create(s_screen_browser);
    lv_obj_set_size(s_sel_line, 88, 3);
    lv_obj_set_style_radius(s_sel_line, is_paper_theme() ? 0 : 2, 0);
    lv_obj_set_style_border_width(s_sel_line, 0, 0);
    lv_obj_set_style_bg_color(s_sel_line, lv_color_hex(accent_color()), 0);
    lv_obj_set_style_bg_opa(s_sel_line, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_sel_line, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    /* The 286px covers reach ~y321 in the 296px strip (CF and carousel alike),
     * so the line sits below the strip: clear of the art above and the
     * (pushed-down) title below. */
    lv_obj_align(s_sel_line, LV_ALIGN_TOP_MID, 0,
                 SCROLLER_Y + SCROLLER_H + k_tune_sel_dy[s_mode]);
    if (!s_show_sel_line) lv_obj_add_flag(s_sel_line, LV_OBJ_FLAG_HIDDEN);

    /* WiFi-strength indicator: rising bars normally; orbiting dot cluster for Glyph. */
    memset(s_wifi_bars, 0, sizeof s_wifi_bars);
    if (!is_glyph_theme()) {
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
    }

    /* Live FPS readout: sits right of the WiFi bars in the top strip.
     * Hidden unless s_fps_enabled; updated every second by fps_timer_cb. */
    s_fps_label = lv_label_create(s_screen_browser);
    lv_label_set_text(s_fps_label, "--");
    lv_obj_set_style_text_color(s_fps_label, lv_color_hex(s_th->dim), 0);
    lv_obj_set_style_text_font(s_fps_label, font_sm(), 0);
    lv_obj_align(s_fps_label, LV_ALIGN_TOP_LEFT,
                 k_tune_fps_x[s_mode], k_tune_fps_y[s_mode]);
    lv_obj_remove_flag(s_fps_label, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    if (!s_fps_enabled) lv_obj_add_flag(s_fps_label, LV_OBJ_FLAG_HIDDEN);

    /* Tappable "now playing" hint pill at the bottom edge (matches the "albums"
     * pill on now-playing). Tap or swipe up to open now-playing. Bounces once on
     * first boot to advertise the gesture. */
    lv_obj_t *pill = make_hint_pill(s_screen_browser, LV_SYMBOL_UP "  NOW PLAYING",
                                    on_hint_to_np);
    lv_obj_align(pill, LV_ALIGN_BOTTOM_MID, 0, -8);

    if (!s_hint_bounced) {
        s_hint_bounced = true;   /* first boot only -- not on theme/accent rebuilds */
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, pill);
        lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
        lv_anim_set_values(&a, -8 + 24, -8);   /* y is the offset from the bottom align */
        lv_anim_set_time(&a, 650);
        lv_anim_set_path_cb(&a, lv_anim_path_bounce);
        lv_anim_start(&a);
    }

    /* Gear button (top-right) -> settings. Sits in the empty strip above the
     * carousel so it never overlaps a card. A cog glyph (LV_SYMBOL_SETTINGS,
     * 0xF013) -- the universal settings affordance; font_md carries the symbol
     * range in every theme (GLYPH included, where icons are clean strokes).
     * No surface box; transparent at rest, faint accent flash on press.
     * PAPER: the buttons are transparent, so the printed frame border (y4)
     * would strike through them at y0 -- start them at y8, inside the taller
     * header band (the rule prints at y40, below the icons). */
    int tb_y = k_tune_topbtn_y[s_mode];
    lv_obj_t *gear = lv_button_create(s_screen_browser);
    lv_obj_set_size(gear, 44, 28);
    lv_obj_align(gear, LV_ALIGN_TOP_RIGHT, TUNE_GEAR_X, tb_y);
    lv_obj_set_style_pad_all(gear, 0, 0);
    lv_obj_set_style_bg_opa(gear, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(gear, 3, 0);
    lv_obj_set_style_shadow_width(gear, 0, 0);
    lv_obj_set_style_bg_color(gear, lv_color_hex(accent_color()),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(gear, LV_OPA_40, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(gear, on_open_settings, LV_EVENT_CLICKED, NULL);
    lv_obj_t *gearlbl = lv_label_create(gear);
    lv_label_set_text(gearlbl, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_color(gearlbl, lv_color_hex(s_th->text2), 0);
    lv_obj_set_style_text_font(gearlbl, font_md(), 0);
    lv_obj_center(gearlbl);

    /* Devices button (left of the gear) -> the device selector. Same flat,
     * transparent-at-rest treatment as the gear. */
    lv_obj_t *devbtn = lv_button_create(s_screen_browser);
    lv_obj_set_size(devbtn, 44, 28);
    lv_obj_align(devbtn, LV_ALIGN_TOP_RIGHT, TUNE_DEVBTN_X, tb_y);
    lv_obj_set_style_pad_all(devbtn, 0, 0);
    lv_obj_set_style_bg_opa(devbtn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(devbtn, 3, 0);
    lv_obj_set_style_shadow_width(devbtn, 0, 0);
    lv_obj_set_style_bg_color(devbtn, lv_color_hex(accent_color()),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(devbtn, LV_OPA_40, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(devbtn, on_open_devices, LV_EVENT_CLICKED, NULL);
    lv_obj_t *devlbl = lv_label_create(devbtn);
    /* Icon is a tune knob -- see TUNE_DEVICES_ICON in ui_tune.h for the
     * candidate glyphs and swap freely. */
    lv_label_set_text(devlbl, TUNE_DEVICES_ICON);
    lv_obj_set_style_text_color(devlbl, lv_color_hex(s_th->text2), 0);
    lv_obj_set_style_text_font(devlbl, font_md(), 0);
    lv_obj_center(devlbl);
}

static void build_np_screen(void)
{
    s_screen_np = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen_np, lv_color_hex(s_th->bg), 0);
    lv_obj_set_style_bg_opa(s_screen_np, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_screen_np, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_screen_np, on_gesture, LV_EVENT_GESTURE, NULL);

    /* PAPER printed-form furniture: outer ink frame + horizontal rules dividing
     * the sheet into its zones (top bar / art + data fields / title block).
     * Created first so every functional element prints on top of the rules. */
    if (is_paper_theme()) {
        paper_frame(s_screen_np);
        paper_rule(s_screen_np, 8, 42,  SCREEN_W - 16, 1);
        paper_rule(s_screen_np, 8, 298, SCREEN_W - 16, 1);
    }

    /* Tappable "albums" hint pill at the top (matches the "now playing" pill on
     * the browser). Tap or swipe down to go back to the browser. */
    lv_obj_t *hint = make_hint_pill(s_screen_np, LV_SYMBOL_DOWN "  ALBUMS",
                                    on_hint_to_browser);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 6);

    /* Tap anywhere on the screen to play/pause. The art widget would intercept
     * taps before they reach the screen, so mark it non-clickable. */
    lv_obj_add_event_cb(s_screen_np, on_np_tap, LV_EVENT_CLICKED, NULL);

    s_np_art = lv_image_create(s_screen_np);
    lv_obj_set_size(s_np_art, ART_W, ART_H);
    lv_obj_set_pos(s_np_art, ART_X, ART_Y);
    lv_obj_remove_flag(s_np_art, LV_OBJ_FLAG_CLICKABLE);
    /* Fit any cover (Spotify art decodes to ~320) into the box, centered and
     * aspect-preserved, no crop. CONTAIN re-applies on every src change. */
    lv_image_set_inner_align(s_np_art, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_set_style_radius(s_np_art, 0, 0);
    /* PAPER: the cover prints inside an ink plate frame (PAPER art is dithered
     * at exactly ART_W so the frame sits flush on the image). */
    if (is_paper_theme()) {
        lv_obj_set_style_border_width(s_np_art, 2, 0);
        lv_obj_set_style_border_color(s_np_art, lv_color_hex(s_th->text), 0);
        lv_obj_set_style_border_opa(s_np_art, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_border_width(s_np_art, 0, 0);
    }
    if (s_art_dsc && s_art_dsc->data) {
        lv_image_set_src(s_np_art, s_art_dsc);
    }

    s_np_title = lv_label_create(s_screen_np);
    lv_obj_set_style_text_letter_space(s_np_title, k_tune_title_lsp[s_mode], 0);
    style_label(s_np_title, font_lg(),
                lv_color_hex(s_th->text), NP_TITLE_Y);
    /* Long track titles scroll horizontally instead of being clipped.
     * Style must precede set_long_mode (which creates the scroll anim). Plain
     * fixed duration (ms), not lv_anim_speed() -- that caps at ~10.23 s. */
    lv_obj_set_style_anim_duration(s_np_title, 150000, LV_PART_MAIN); /* 150 s/loop, very slow */
    lv_label_set_long_mode(s_np_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(s_np_title, "Nothing playing");

    s_np_artist = lv_label_create(s_screen_np);
    style_label(s_np_artist, font_md(),
                lv_color_hex(s_th->text2), NP_ARTIST_Y);

    s_np_device = lv_label_create(s_screen_np);
    lv_label_set_text(s_np_device, "");
    if (is_paper_theme()) {
        /* PAPER: the device becomes a labelled data field in the empty left
         * column beside the art -- "OUTPUT" corner label over the value,
         * left-aligned like the reference sheets' carrier/transport cells. */
        paper_field_label(s_screen_np, "OUTPUT", 28, 58);
        lv_obj_set_width(s_np_device, ART_X - 44);
        lv_obj_set_style_text_align(s_np_device, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_text_color(s_np_device, lv_color_hex(s_th->text), 0);
        lv_obj_set_style_text_font(s_np_device, font_sm(), 0);
        lv_label_set_long_mode(s_np_device, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(s_np_device, 28, 84);
    } else {
        lv_obj_set_width(s_np_device, SCREEN_W - 32);
        lv_obj_set_style_text_align(s_np_device, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(s_np_device, lv_color_hex(s_th->dim), 0);
        lv_obj_set_style_text_font(s_np_device, font_sm(), 0);
        lv_obj_set_pos(s_np_device, 16, NP_DEVICE_Y);
    }

    s_np_progress = lv_bar_create(s_screen_np);
    lv_obj_set_size(s_np_progress, PROG_W, PROG_H);
    lv_obj_set_pos(s_np_progress, PROG_X, PROG_Y);
    lv_bar_set_range(s_np_progress, 0, 1000);
    lv_bar_set_value(s_np_progress, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_np_progress, lv_color_hex(s_th->track), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_np_progress, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_np_progress, lv_color_hex(accent_color()), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_np_progress, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_np_progress, is_paper_theme() ? 0 : 3, LV_PART_MAIN);
    lv_obj_set_style_radius(s_np_progress, is_paper_theme() ? 0 : 3, LV_PART_INDICATOR);
    lv_obj_remove_flag(s_np_progress, LV_OBJ_FLAG_CLICKABLE);

    /* Elapsed (left) / remaining (right) timestamps flanking the bar. Montserrat
     * isn't monospaced, so the labels are fixed-width and edge-aligned toward the
     * bar -- the M:SS text shifts at most a pixel as digits change, not the layout. */
    /* Label width is per-mode (TS_W, ui_tune.h): mono/pixel digits are ~2x as
     * wide as Montserrat's and "-12:34" wrapped onto two lines at 64px. */
    s_np_elapsed = lv_label_create(s_screen_np);
    lv_label_set_text(s_np_elapsed, "0:00");
    lv_obj_set_width(s_np_elapsed, TS_W);
    lv_obj_set_style_text_align(s_np_elapsed, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(s_np_elapsed, lv_color_hex(s_th->text2), 0);
    lv_obj_set_style_text_font(s_np_elapsed, font_sm(), 0);
    lv_obj_set_pos(s_np_elapsed, PROG_X - 8 - TS_W, TS_Y);

    /* Tap the right-hand timecode to flip it between time-remaining ("-1:23")
     * and total track length ("3:45"), like Spotify. The label doesn't bubble
     * CLICKED, so the tap never reaches the screen's play/pause handler. */
    s_np_remain = lv_label_create(s_screen_np);
    lv_label_set_text(s_np_remain, "0:00");
    lv_obj_set_width(s_np_remain, TS_W);
    lv_obj_set_style_text_align(s_np_remain, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(s_np_remain, lv_color_hex(s_th->text2), 0);
    lv_obj_set_style_text_font(s_np_remain, font_sm(), 0);
    lv_obj_set_pos(s_np_remain, PROG_X + PROG_W + 8, TS_Y);
    lv_obj_add_flag(s_np_remain, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_np_remain, 12);
    lv_obj_add_event_cb(s_np_remain, on_remain_tap, LV_EVENT_CLICKED, NULL);

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

    /* Drag thumb: accent knob centred on the bar, hidden until a scrub starts.
     * A plain rounded rect (no transform), so it's safe under the rotated flush. */
    s_seek_thumb = lv_obj_create(s_screen_np);
    lv_obj_set_size(s_seek_thumb, THUMB_W, THUMB_H);
    lv_obj_set_style_radius(s_seek_thumb, THUMB_W / 2, 0);
    lv_obj_set_style_bg_color(s_seek_thumb, lv_color_hex(accent_color()), 0);
    lv_obj_set_style_bg_opa(s_seek_thumb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_seek_thumb, 0, 0);
    lv_obj_remove_flag(s_seek_thumb, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_seek_thumb, LV_OBJ_FLAG_HIDDEN);
    position_seek_thumb(0);

    /* PAPER ruler: a printed tick scale under the bar (taller tick every 5th,
     * like the reference radio dial) and a solid ink block riding the playhead.
     * The block doubles as the scrub indicator -- the round thumb stays hidden
     * in this theme (see on_seek_start). All inert to input so the seek overlay
     * underneath keeps receiving the drag. */
    s_paper_cursor = NULL;
    if (is_paper_theme()) {
        for (int i = 0; i < PAPER_TICK_COUNT; i++) {
            bool major = (i % 5) == 0;
            lv_obj_t *tk = paper_rule(s_screen_np,
                                      PROG_X + i * PAPER_TICK_PITCH,
                                      PROG_Y + PROG_H + 3,
                                      1, major ? 10 : 5);
            if (!major) lv_obj_set_style_bg_opa(tk, (lv_opa_t)130, 0);
        }
        s_paper_cursor = lv_obj_create(s_screen_np);
        lv_obj_set_size(s_paper_cursor, PAPER_CUR_W, PAPER_CUR_H);
        lv_obj_set_style_radius(s_paper_cursor, 0, 0);
        lv_obj_set_style_border_width(s_paper_cursor, 0, 0);
        lv_obj_set_style_bg_color(s_paper_cursor, lv_color_hex(s_th->text), 0);
        lv_obj_set_style_bg_opa(s_paper_cursor, LV_OPA_COVER, 0);
        lv_obj_remove_flag(s_paper_cursor, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(s_paper_cursor, PROG_X - PAPER_CUR_W / 2,
                       PROG_Y + PROG_H / 2 - PAPER_CUR_H / 2);
    }

    /* Transport keys: prev / play-pause / next. Square flat tactile keys
     * matching the settings buttons (radius 3), with a pressed accent flash.
     * Created after the seek overlay so they win the hit-test where the two
     * touch zones overlap. Gestures still work as shortcuts. */
    struct { const char *sym; lv_event_cb_t cb; } keys[3] = {
        { LV_SYMBOL_PREV,  on_transport_prev   },
        { LV_SYMBOL_PLAY,  on_transport_toggle },
        { LV_SYMBOL_NEXT,  on_transport_next   },
    };
    int group_w = 3 * TKEY_SZ + 2 * TKEY_GAP;
    int x0      = (SCREEN_W - group_w) / 2;
    for (int i = 0; i < 3; i++) {
        lv_obj_t *key = lv_button_create(s_screen_np);
        lv_obj_set_size(key, TKEY_SZ, TKEY_SZ);
        lv_obj_set_pos(key, x0 + i * (TKEY_SZ + TKEY_GAP), TKEY_Y);
        style_key_btn(key);
        lv_obj_set_style_bg_color(key, lv_color_hex(s_th->surface), 0);
        lv_obj_set_style_bg_opa(key, LV_OPA_COVER, 0);
        style_button_press(key);
        lv_obj_add_event_cb(key, keys[i].cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *lbl = lv_label_create(key);
        lv_label_set_text(lbl, keys[i].sym);
        lv_obj_set_style_text_color(lbl, lv_color_hex(s_th->text), 0);
        /* Compiled Montserrat bundles the symbol range (Arvo/Pixel/mono
         * don't), so the keys always show real prev/play/next icons. GLYPH
         * uses it too: the Nothing reference draws icons as clean thin
         * strokes, keeping the dot-matrix voice for headings only. */
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, 0);
        lv_obj_center(lbl);
        if (i == 1) s_np_play_lbl = lbl;   /* centre key reflects play state */
    }
    refresh_play_icon();

    /* Faint edge chevrons hinting swipe left/right = next/prev. */
    lv_obj_t *ch_l = lv_label_create(s_screen_np);
    lv_label_set_text(ch_l, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(ch_l, lv_color_hex(s_th->dim), 0);
    lv_obj_set_style_text_font(ch_l, font_sm(), 0);
    lv_obj_align(ch_l, LV_ALIGN_LEFT_MID, 6, -20);

    lv_obj_t *ch_r = lv_label_create(s_screen_np);
    lv_label_set_text(ch_r, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(ch_r, lv_color_hex(s_th->dim), 0);
    lv_obj_set_style_text_font(ch_r, font_sm(), 0);
    lv_obj_align(ch_r, LV_ALIGN_RIGHT_MID, -6, -20);

    s_vol_hud = lv_label_create(s_screen_np);
    lv_label_set_text(s_vol_hud, "");
    /* The fixed warm alert hues vanish on the light grounds (PAPER cream,
     * GLYPH light grey) -- use the accent there. */
    lv_obj_set_style_text_color(s_vol_hud,
        lv_color_hex((is_paper_theme() || is_glyph_theme()) ? accent_color()
                                                            : 0xFF4040), 0);
    lv_obj_set_style_text_font(s_vol_hud, font_md(), 0);
    lv_obj_align(s_vol_hud, LV_ALIGN_TOP_RIGHT, -8, 6);
    lv_obj_add_flag(s_vol_hud, LV_OBJ_FLAG_HIDDEN);

    /* Toast: brief auto-hide notification at the bottom of the screen. Used
     * by ui_show_toast to surface async failures (e.g. play returned 404).
     * Same hide-via-timer pattern as the volume HUD. */
    s_toast = lv_label_create(s_screen_np);
    lv_label_set_text(s_toast, "");
    lv_obj_set_width(s_toast, SCREEN_W - 40);
    lv_obj_set_style_text_align(s_toast, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_toast,
        lv_color_hex((is_paper_theme() || is_glyph_theme()) ? accent_color()
                                                            : 0xFFA000), 0);
    lv_obj_set_style_text_font(s_toast, font_md(), 0);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);

    /* Vertical volume fader in the open right column (clear of the art, the
     * remaining-time label below, and the swipe chevron to its right). It
     * reflects the active device's level; the command fires on release (one per
     * drag, not per pixel) with the HUD giving live feedback during the drag. */
    lv_obj_t *vol_ico = lv_label_create(s_screen_np);
    lv_label_set_text(vol_ico, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_color(vol_ico, lv_color_hex(s_th->text2), 0);
    lv_obj_set_style_text_font(vol_ico, font_sm(), 0);
    lv_obj_set_pos(vol_ico, 715, 40);
    /* In Glyph mode the icon is a tappable shortcut to the volume page. */
    if (is_glyph_theme()) {
        lv_obj_set_style_text_color(vol_ico, lv_color_hex(accent_color()), 0);
        lv_obj_add_flag(vol_ico, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(vol_ico, on_open_volume, LV_EVENT_CLICKED, NULL);
    }
    /* PAPER: the fader is a labelled data field like OUTPUT -- swap the icon
     * for a tracked-out accent "LEVEL" corner label. */
    if (is_paper_theme()) {
        lv_obj_add_flag(vol_ico, LV_OBJ_FLAG_HIDDEN);
        paper_field_label(s_screen_np, "LEVEL", 700, 40);
    }

    s_np_volume = lv_slider_create(s_screen_np);
    /* Position + height are per-mode (ui_tune.h). The square PIXEL/PAPER knob
     * overhangs the track ends by ~26px, so PAPER uses a shorter track to clear
     * the LEVEL label above and the printed rule below. */
    lv_obj_set_size(s_np_volume, 44, k_tune_fader_h[s_mode]);
    lv_obj_set_pos(s_np_volume, k_tune_fader_x[s_mode], k_tune_fader_y[s_mode]);
    lv_slider_set_range(s_np_volume, 0, 100);
    lv_slider_set_value(s_np_volume, 50, LV_ANIM_OFF);
    {
        lv_obj_set_style_bg_color(s_np_volume, lv_color_hex(s_th->track), LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_np_volume, lv_color_hex(accent_color()), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(s_np_volume, lv_color_hex(accent_color()), LV_PART_KNOB);
        /* PAPER and PIXEL square the fader off completely -- a 6px radius on
         * the indicator read as "round top, flat bottom" once the fill met the
         * square track ends in PIXEL. */
        bool sq = is_paper_theme() || is_pixel_theme();
        lv_obj_set_style_radius(s_np_volume, sq ? 0 : 6, LV_PART_MAIN);
        lv_obj_set_style_radius(s_np_volume, sq ? 0 : 6, LV_PART_INDICATOR);
        lv_obj_set_style_radius(s_np_volume, sq ? 0 : 22, LV_PART_KNOB);
        lv_obj_set_style_pad_all(s_np_volume, 4, LV_PART_KNOB);
        /* GLYPH: hairline outline + ink track to match the Nothing chrome. The
         * fader is now SHOWN here (it used to be hidden in favour of the dot
         * volume page, which left no visible control on now-playing); the page
         * shortcut on the accent icon still works as a bonus. */
        if (is_glyph_theme()) {
            lv_obj_set_style_border_width(s_np_volume, 1, LV_PART_MAIN);
            lv_obj_set_style_border_color(s_np_volume, lv_color_hex(s_th->text2), LV_PART_MAIN);
            lv_obj_set_style_border_opa(s_np_volume, LV_OPA_COVER, LV_PART_MAIN);
        }
    }
    /* PRESSED sets s_vol_dragging so on_gesture won't misread the drag as a
     * swipe-to-browser. PRESS_LOST handles the case where LVGL reclassifies the
     * touch mid-drag. Both RELEASED paths clear the flag. */
    lv_obj_add_event_cb(s_np_volume, on_vol_press,    LV_EVENT_PRESSED,       NULL);
    lv_obj_add_event_cb(s_np_volume, on_vol_changed,  LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_np_volume, on_vol_released, LV_EVENT_RELEASED,      NULL);
    lv_obj_add_event_cb(s_np_volume, on_vol_press_lost, LV_EVENT_PRESS_LOST,  NULL);
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
            sel ? opt_sel_bg() : lv_color_hex(s_th->surface), 0);
        lv_obj_set_style_text_color(s_opt_labels[i],
            sel ? opt_sel_fg() : lv_color_hex(s_th->text2), 0);
        lv_label_set_text(s_opt_labels[i], k_transition_names[i]);
    }
}

static void refresh_theme_selection(void)
{
    for (int i = 0; i < MODE_COUNT; i++) {
        if (!s_theme_btns[i] || !s_theme_labels[i]) continue;
        bool sel = (i == (int)s_mode);
        lv_obj_set_style_bg_color(s_theme_btns[i],
            sel ? opt_sel_bg() : lv_color_hex(s_th->surface), 0);
        lv_obj_set_style_text_color(s_theme_labels[i],
            sel ? opt_sel_fg() : lv_color_hex(s_th->text2), 0);
        lv_label_set_text(s_theme_labels[i], k_mode_names[i]);
    }
    for (int i = 0; i < 2; i++) {
        if (!s_dl_btns[i] || !s_dl_labels[i]) continue;
        bool sel = (i == (s_dark ? 0 : 1));
        lv_obj_set_style_bg_color(s_dl_btns[i],
            sel ? opt_sel_bg() : lv_color_hex(s_th->surface), 0);
        lv_obj_set_style_text_color(s_dl_labels[i],
            sel ? opt_sel_fg() : lv_color_hex(s_th->text2), 0);
        lv_label_set_text(s_dl_labels[i], k_darklight_names[i]);
    }
    if (s_art_toggle_btn && s_art_toggle_lbl) {
        lv_obj_set_style_bg_color(s_art_toggle_btn,
            s_theme_art ? opt_sel_bg() : lv_color_hex(s_th->surface), 0);
        lv_obj_set_style_text_color(s_art_toggle_lbl,
            s_theme_art ? opt_sel_fg() : lv_color_hex(s_th->text2), 0);
        lv_label_set_text(s_art_toggle_lbl, s_theme_art ? "ON" : "OFF");
    }
}

/* Colour grid. Each button is a swatch filled with its own accent colour (the
 * grid itself is the legend -- no names); the selected one gets a ring + check.
 * The ring uses the theme's text colour so it reads on light grounds too. */
static void refresh_accent_selection(void)
{
    for (int i = 0; i < ACCENT_COUNT; i++) {
        if (!s_accent_btns[i] || !s_accent_labels[i]) continue;
        bool sel = (i == (int)s_accent);
        lv_obj_set_style_bg_color(s_accent_btns[i], lv_color_hex(k_accents[i]), 0);
        lv_obj_set_style_border_width(s_accent_btns[i], sel ? 3 : 0, 0);
        lv_obj_set_style_border_color(s_accent_btns[i], lv_color_hex(s_th->text), 0);
        /* Check mark contrast: dark ink on light (soft/pastel) swatches, white
         * on the vivid/deep ones. Rec.601 luma of the swatch fill. */
        uint32_t c = k_accents[i];
        uint32_t luma = (((c >> 16) & 0xFF) * 77 + ((c >> 8) & 0xFF) * 150 + (c & 0xFF) * 29) >> 8;
        lv_obj_set_style_text_color(s_accent_labels[i],
                                    luma > 150 ? lv_color_black() : lv_color_white(), 0);
        lv_label_set_text(s_accent_labels[i], sel ? LV_SYMBOL_OK : "");
    }
}

static bool is_glyph_theme(void)
{
    return s_mode == MODE_GLYPH;
}

static bool is_pixel_theme(void)
{
    return s_mode == MODE_PIXEL;
}

static bool is_paper_theme(void)
{
    return s_mode == MODE_PAPER;
}

/* 1bpp bitmap fonts for the PIXEL retro theme (Press Start 2P + FA5 symbols).
 * Generated offline by lv_font_conv; committed as .c files in main/. */
extern const lv_font_t lv_font_pixel_16;
extern const lv_font_t lv_font_pixel_24;

/* Monospace teletype fonts for the PAPER theme: unscii-8 baked at 2x/3x its
 * 8px grid by scripts/gen_lvgl_font.py (clean pixel render, no --dots), with
 * Montserrat fallbacks carrying the LVGL symbol glyphs + accented characters. */
extern const lv_font_t lv_font_mono_16;
extern const lv_font_t lv_font_mono_24;

/* Bespoke round-dot matrix fonts for the GLYPH theme (each glyph is built from
 * round dots; generated by scripts/gen_lvgl_font.py --dots). Declared up with the
 * forward declarations. Their fallback chain (dot_NN -> dot_sym_NN -> montserrat)
 * carries dotted FontAwesome symbols, so icons in GLYPH render as dots too. */

/* Font accessor helpers: return the PIXEL 1bpp font when PIXEL theme is
 * active, else the normal runtime TTF / bitmap font.  Route all
 * lv_obj_set_style_text_font calls through these so a single theme change
 * automatically updates every label on rebuild. */
static const lv_font_t *font_lg(void)
{
    if (is_pixel_theme()) return &lv_font_pixel_24;
    /* GLYPH (Nothing-style): the round-dot font is the HEADING voice only --
     * titles and screen names -- exactly like Nothing OS's dot-matrix
     * "EQUALISER" headings. It's monospace + wide, so 24px (a tidy 3x the 8px
     * grid) keeps long album names workable; they scroll (see build_*_screen).
     * Body text and icons are clean small type via font_md/font_sm. */
    if (is_glyph_theme()) return &lv_font_dot_24;
    if (is_paper_theme()) return &lv_font_mono_24;
    if (s_font_choice == FONT_SLAB) return &lv_font_arvo_28;
    return &lv_font_montserrat_28;
}
static const lv_font_t *font_md(void)
{
    if (is_pixel_theme()) return &lv_font_pixel_16;
    /* GLYPH: clean SMALL type under the dotted headings (the reference pairs
     * a dot-matrix heading with airy small labels, not dots everywhere).
     * Montserrat also carries the LVGL symbols, so the cog/devices/audio
     * icons render as clean strokes -- which retires the old "dotted cog
     * reads muddy" nit. */
    if (is_glyph_theme()) return &lv_font_montserrat_20;
    if (is_paper_theme()) return &lv_font_mono_16;
    if (s_font_choice == FONT_SLAB) return &lv_font_arvo_24;
    return &lv_font_montserrat_24;
}
static const lv_font_t *font_sm(void)
{
    if (is_pixel_theme()) return &lv_font_pixel_16;
    if (is_paper_theme()) return &lv_font_mono_16;
    return &lv_font_montserrat_20;
}

/* Pixelation pipeline: nearest-neighbour downsample + Bayer 4x4 ordered dither
 * + RGB444 bit-mask quantize.  Applied once per art/thumbnail change; no
 * per-frame cost.  Produces authentic retro dithered stipple on gradients.
 * src/dst are RGB565 (little-endian, same as LVGL LV_COLOR_FORMAT_RGB565). */
static void pixelate_rgb565(const uint16_t *src, uint16_t sw, uint16_t sh,
                             uint16_t *dst, uint16_t dw, uint16_t dh)
{
    static const uint8_t bayer4[4][4] = {
        {  0,  8,  2, 10 },
        { 12,  4, 14,  6 },
        {  3, 11,  1,  9 },
        { 15,  7, 13,  5 },
    };
    for (uint16_t dy = 0; dy < dh; dy++) {
        for (uint16_t dx = 0; dx < dw; dx++) {
            int sx = (int)dx * sw / dw;
            int sy = (int)dy * sh / dh;
            uint16_t pix = src[(size_t)sy * sw + sx];
            /* Expand RGB565 → 8-bit per channel. */
            int r = ((pix >> 11) & 0x1F) << 3;
            int g = ((pix >>  5) & 0x3F) << 2;
            int b = ( pix        & 0x1F) << 3;
            /* Bayer dither: threshold 0..15, quantisation grid step = 16. */
            int t = bayer4[dy & 3][dx & 3];
            r += t; if (r > 255) r = 255;
            g += t; if (g > 255) g = 255;
            b += t; if (b > 255) b = 255;
            /* RGB444 quantize: mask lower 4 bits of each 8-bit channel. */
            r &= 0xF0;
            g &= 0xF0;
            b &= 0xF0;
            /* Repack to RGB565. */
            dst[(size_t)dy * dw + dx] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    }
}

/* PAPER duotone pipeline: nearest resample + 8x8 ordered Bayer dither of the
 * LUMINANCE down to one bit, mapped to the active palette's paper/ink colours
 * -- printed halftone, exactly the 1-bit imagery of the reference sheets. The
 * 64-level matrix keeps full covers reading as fine print grain (the 4x4 used
 * by PIXEL is too coarse once there's no colour left to carry the image).
 * Same call shape as pixelate_rgb565; resamples internally, so callers dither
 * directly at the resolution that hits the screen. */
static const uint8_t k_bayer8[8][8] = {
    {  0, 32,  8, 40,  2, 34, 10, 42 },
    { 48, 16, 56, 24, 50, 18, 58, 26 },
    { 12, 44,  4, 36, 14, 46,  6, 38 },
    { 60, 28, 52, 20, 62, 30, 54, 22 },
    {  3, 35, 11, 43,  1, 33,  9, 41 },
    { 51, 19, 59, 27, 49, 17, 57, 25 },
    { 15, 47,  7, 39, 13, 45,  5, 37 },
    { 63, 31, 55, 23, 61, 29, 53, 21 },
};

static void paperize_rgb565(const uint16_t *src, uint16_t sw, uint16_t sh,
                            uint16_t *dst, uint16_t dw, uint16_t dh)
{
    uint32_t ink = s_th->text, pap = s_th->bg;
    uint16_t ink_px = (uint16_t)((((ink >> 19) & 0x1Fu) << 11) |
                                 (((ink >> 10) & 0x3Fu) <<  5) |
                                  ((ink >>  3) & 0x1Fu));
    uint16_t pap_px = (uint16_t)((((pap >> 19) & 0x1Fu) << 11) |
                                 (((pap >> 10) & 0x3Fu) <<  5) |
                                  ((pap >>  3) & 0x1Fu));
    for (uint16_t dy = 0; dy < dh; dy++) {
        for (uint16_t dx = 0; dx < dw; dx++) {
            int sx = (int)dx * sw / dw;
            int sy = (int)dy * sh / dh;
            uint16_t pix = src[(size_t)sy * sw + sx];
            int r = ((pix >> 11) & 0x1F) << 3;
            int g = ((pix >>  5) & 0x3F) << 2;
            int b = ( pix        & 0x1F) << 3;
            /* Rec.601 luma; threshold 2..254 from the Bayer cell. */
            int lum = (r * 299 + g * 587 + b * 114) / 1000;
            int thr = k_bayer8[dy & 7][dx & 7] * 4 + 2;
            dst[(size_t)dy * dw + dx] = (lum > thr) ? pap_px : ink_px;
        }
    }
}


/* =====================================================================
 * Cover Flow perspective rasteriser
 *
 * A PSRAM-backed CF_PERSP_W×CF_PERSP_H (800×296) RGB565 canvas covers the
 * browser scroller strip.  On every scroll event cf_render() clears the canvas
 * to the theme background, then rasterises each visible album as a trapezoid
 * (base = 220px thumb × CF_CARD_SCALE) using per-column perspective math:
 *
 *   left edge:    full base height
 *   right edge:   base_h × (1 − tilt × HEIGHT_SHRINK)
 *   display width: base_w × (1 − tilt × WIDTH_SHRINK)
 *
 * where tilt = min(|dist_norm|, 1.0), dist_norm = signed distance from
 * the scroll centre in units of one card step.  Source art is never mirrored.
 *
 * Drawing order: far cards first, near-centre card last, so the centre
 * cover renders on top of turned side covers at their overlap edges —
 * exactly the iPod Cover Flow z-order.
 *
 * LVGL card images are hidden; scroll snapping/momentum continue via the
 * flex layout (step = cs() + cg() = 110 px).
 * ===================================================================== */

/* Tilt geometry constants. CF_CARD_SCALE upsizes the 220px thumb before the
 * perspective taper so the centre cover dominates the strip; CF_PERSP_H is sized
 * to fit base_h = 220*CF_CARD_SCALE without clipping. */
#define CF_CARD_SCALE    1.30f   /* drawn base = 220*1.30 = ~286 px (fits 296 canvas) */
/* Side-cover tilt for Cover Flow -- fixed at the value chosen on hardware (was
 * step 7/9 of the temporary CF ANGLE test slider): a gentle turn. Width
 * foreshorten + height taper, both scaled by tilt in cf_render_card. */
#define CF_WIDTH_SHRINK  0.272f   /* w_disp  = base_w*(1-tilt*0.272) */
#define CF_HEIGHT_SHRINK 0.404f   /* h_far(inner) = base_h*(1-tilt*0.404) */
/* Perspective fan: side covers are placed on a converging curve, not a linear
 * step, so ±1 sits far enough out to stay mostly visible while covers further
 * out bunch toward CF_FAN_SPREAD at the screen edges (iPod Cover Flow look). */
#define CF_FAN_SPREAD    320.0f  /* px: limit of a side cover's offset from centre */
#define CF_FAN_RATE      0.76f   /* convergence; ±1 lands ~170px out, outer covers crowd */
#define CF_MAX_SIDE      3       /* only rasterise covers within ±this of centre. Bounds
                                  * per-scroll work (covers beyond are occluded/squished) --
                                  * without it the fan keeps every cover on-screen => all
                                  * rasterise each scroll event => sluggish. */
#define CF_LEAN_FLIP     0       /* 0: covers face CENTRE (outer edge near/tall -- correct).
                                  * Set 1 if the rotated panel mirrors the fan so they face
                                  * outward. One-line orientation flip. */

static void cf_draw_col(int dx, int h_col, int cy_mid,
                        const uint16_t *src, int src_w, int src_h, int src_x)
{
    if ((unsigned)dx >= CF_PERSP_W || h_col <= 0) return;
    if (src_x < 0) src_x = 0;
    if (src_x >= src_w) src_x = src_w - 1;
    int y_top = cy_mid - h_col / 2;
    for (int dy = y_top; dy < y_top + h_col; dy++) {
        if ((unsigned)dy >= CF_PERSP_H) continue;
        int src_y = (dy - y_top) * src_h / h_col;
        s_cf_buf[(size_t)dy * CF_PERSP_W + (size_t)dx] =
            src[(size_t)src_y * (size_t)src_w + (size_t)src_x];
    }
}

/* =====================================================================
 * Cover Flow CANONICAL GEOMETRY -- do not regress (mirrored in CLAUDE.md,
 * waveshare README, and memory/project_coverflow_geometry.md):
 *
 *   - Centre album: flat, facing the viewer, on top (highest z), largest.
 *   - Each side album is rotated to FACE THE CENTRE: its OUTER edge is nearest
 *     the viewer (drawn TALLEST), its INNER edge (toward centre) recedes (drawn
 *     SHORTEST) and tucks BEHIND its more-central neighbour.
 *       left album : outer = LEFT edge (near/tall),  inner = right edge (far/short)
 *       right album: outer = RIGHT edge (near/tall), inner = left edge (far/short)
 *   - Z-order: centre on top; each album drawn UNDER the one nearer centre.
 *   - Art is perspective-foreshortened: compresses toward the FAR (inner) edge.
 *     Source art is never mirrored (reads left->right) -- the height taper +
 *     horizontal compression convey the 3-D turn.
 *
 * Renders one album into s_cf_buf at its current (already fan-remapped) x. The
 * drawn size is a fixed target (220px reference x CF_CARD_SCALE), NOT the source
 * resolution, so low-res PIXEL thumbs fill the same card as full covers. */
static void cf_render_card(int32_t card_cx, float dist_norm,
                           const uint16_t *src, int src_w, int src_h)
{
    float adist = dist_norm < 0.0f ? -dist_norm : dist_norm;
    float tilt  = adist > 1.0f ? 1.0f : adist;

    int   base_w = (int)((float)ALBUM_THUMB_W * CF_CARD_SCALE);
    int   base_h = (int)((float)ALBUM_THUMB_H * CF_CARD_SCALE);

    int   w_disp = (int)((float)base_w * (1.0f - tilt * CF_WIDTH_SHRINK));
    if (w_disp < 4) w_disp = 4;
    float h_near = (float)base_h;                                  /* outer edge: full height */
    float h_far  = h_near * (1.0f - tilt * CF_HEIGHT_SHRINK);      /* inner edge: foreshortened */
    if (h_far < 6.0f) h_far = 6.0f;
    float f = h_far / h_near;                                  /* depth ratio (<1) */

    int x_start = (int)card_cx - w_disp / 2;
    int x_end   = x_start + w_disp;
    int cy_mid  = CF_PERSP_H / 2;

    /* left album: outer (near) edge on the LEFT (t=0); right album: outer edge on
     * the RIGHT (t=1). CF_LEAN_FLIP swaps if the panel mirrors the fan. */
    bool left = (dist_norm < 0.0f);
#if CF_LEAN_FLIP
    left = !left;
#endif

    /* PAPER prints an ink frame around every cover, drawn INTO the canvas so it
     * turns/foreshortens with the trapezoid (an LVGL border can't follow this).
     * When the canvas is dithered afterwards (themed art) the frame is laid
     * down black -- minimum luminance dithers to solid ink in BOTH palette
     * faces; with themed art off it's the ink colour directly. */
    bool     frame    = is_paper_theme();
    uint32_t inkc     = s_th->text;
    uint16_t frame_px = s_theme_art ? 0x0000
                      : (uint16_t)((((inkc >> 19) & 0x1Fu) << 11) |
                                   (((inkc >> 10) & 0x3Fu) <<  5) |
                                    ((inkc >>  3) & 0x1Fu));

    for (int dx = x_start; dx < x_end; dx++) {
        float t = (w_disp > 1)
                ? (float)(dx - x_start) / (float)(w_disp - 1)
                : 0.0f;

        float e   = left ? t : (1.0f - t);          /* 0 at NEAR(outer), 1 at FAR(inner) */
        float u   = (e * f) / (e * f + (1.0f - e)); /* perspective: compress toward FAR */
        float art = left ? u : (1.0f - u);          /* near(outer) shows the outer art side */
        int   src_x = (int)(art * (float)(src_w - 1));
        int   h_col = (int)(h_near + (h_far - h_near) * e); /* tall at outer, short at inner */
        if (h_col < 1) h_col = 1;

        cf_draw_col(dx, h_col, cy_mid, src, src_w, src_h, src_x);

        if (frame && (unsigned)dx < CF_PERSP_W) {
            int y_top = cy_mid - h_col / 2;
            int y_bot = y_top + h_col;
            if ((dx - x_start) < 2 || (x_end - 1 - dx) < 2) {
                /* near/far vertical edges: full-height ink column */
                for (int dy = y_top; dy < y_bot; dy++)
                    if ((unsigned)dy < CF_PERSP_H)
                        s_cf_buf[(size_t)dy * CF_PERSP_W + dx] = frame_px;
            } else {
                for (int k = 0; k < 2; k++) {
                    int dyt = y_top + k, dyb = y_bot - 1 - k;
                    if ((unsigned)dyt < CF_PERSP_H)
                        s_cf_buf[(size_t)dyt * CF_PERSP_W + dx] = frame_px;
                    if ((unsigned)dyb < CF_PERSP_H)
                        s_cf_buf[(size_t)dyb * CF_PERSP_W + dx] = frame_px;
                }
            }
        }
    }
}

/* PAPER Cover Flow duotone: dither the finished canvas IN PLACE, after all the
 * trapezoids have been drawn from RAW covers. Dithering post-projection puts
 * the halftone grain exactly 1px on screen -- sampling the pre-dithered thumb
 * pool through the perspective remap smeared the pattern into grey mush (the
 * old "not stylised" look). Background pixels (the clear colour) are skipped so
 * the empty paper stays clean instead of picking up threshold speckle. */
static void cf_paper_dither(uint16_t bg_px)
{
    uint32_t ink = s_th->text, pap = s_th->bg;
    uint16_t ink_px = (uint16_t)((((ink >> 19) & 0x1Fu) << 11) |
                                 (((ink >> 10) & 0x3Fu) <<  5) |
                                  ((ink >>  3) & 0x1Fu));
    uint16_t pap_px = (uint16_t)((((pap >> 19) & 0x1Fu) << 11) |
                                 (((pap >> 10) & 0x3Fu) <<  5) |
                                  ((pap >>  3) & 0x1Fu));
    for (int dy = 0; dy < CF_PERSP_H; dy++) {
        uint16_t *row = s_cf_buf + (size_t)dy * CF_PERSP_W;
        for (int dx = 0; dx < CF_PERSP_W; dx++) {
            uint16_t pix = row[dx];
            if (pix == bg_px) continue;
            int r = ((pix >> 11) & 0x1F) << 3;
            int g = ((pix >>  5) & 0x3F) << 2;
            int b = ( pix        & 0x1F) << 3;
            int lum = (r * 299 + g * 587 + b * 114) / 1000;
            int thr = k_bayer8[dy & 7][dx & 7] * 4 + 2;
            row[dx] = (lum > thr) ? pap_px : ink_px;
        }
    }
}

/* When the FPS readout is on, log a periodic breakdown of where each Cover Flow
 * scroll frame spends its time -- the full-canvas clear vs. the per-card
 * perspective rasterise -- so the render path can be profiled on hardware
 * without a debugger. Zero cost unless FPS display is enabled. */
static void cf_profile_tick(int64_t t0, int64_t t1, int64_t t2)
{
    static uint64_t acc_clear, acc_rast;
    static uint32_t acc_n;
    acc_clear += (uint64_t)(t1 - t0);
    acc_rast  += (uint64_t)(t2 - t1);
    if (++acc_n >= 30) {
        ESP_LOGI(TAG, "cf_prof: clear=%lu us  rast=%lu us  frame=%lu us  (avg/%lu)",
                 (unsigned long)(acc_clear / acc_n),
                 (unsigned long)(acc_rast / acc_n),
                 (unsigned long)((acc_clear + acc_rast) / acc_n),
                 (unsigned long)acc_n);
        acc_clear = acc_rast = 0;
        acc_n = 0;
    }
}

static void cf_render(void)
{
    if (!s_cf_buf || !s_cf_img || !s_browser_scroller) return;

    int64_t t_prof0 = s_fps_enabled ? esp_timer_get_time() : 0;

    /* Clear canvas to theme background (convert 0xRRGGBB → RGB565). */
    uint32_t bg    = s_th->bg;
    uint16_t bg_px = (uint16_t)(((bg >> 19) & 0x1Fu) << 11)
                   | (uint16_t)(((bg >> 10) & 0x3Fu) << 5)
                   | (uint16_t) ((bg >>  3) & 0x1Fu);
    uint16_t *p = s_cf_buf, *end = p + (size_t)CF_PERSP_W * CF_PERSP_H;
    while (p < end) *p++ = bg_px;

    int64_t t_prof1 = s_fps_enabled ? esp_timer_get_time() : 0;

    int32_t scroll_x = lv_obj_get_scroll_left(s_browser_scroller);
    int32_t pad_left = (SCREEN_W - cs()) / 2;
    int32_t step     = cs() + cg();
    int32_t scr_cx   = SCREEN_W / 2;

    /* Draw farthest cards first and the centre card last, so each nearer cover
     * paints over the turned cover behind it -- correct iPod z-order on BOTH
     * sides (the old index-order passes drew right-side covers back-to-front).
     * Two pointers walk inward from the extremes, always drawing the one with
     * the larger distance-from-centre next. */
    int lo = 0, hi = (int)s_card_count - 1;
    while (lo <= hi) {
        int32_t cx_lo = pad_left + (int32_t)lo * step + cs() / 2 - scroll_x;
        int32_t cx_hi = pad_left + (int32_t)hi * step + cs() / 2 - scroll_x;
        int32_t ad_lo = cx_lo - scr_cx; if (ad_lo < 0) ad_lo = -ad_lo;
        int32_t ad_hi = cx_hi - scr_cx; if (ad_hi < 0) ad_hi = -ad_hi;

        size_t i;
        if (lo == hi)            { i = (size_t)lo; lo++; }
        else if (ad_lo >= ad_hi) { i = (size_t)lo; lo++; }
        else                     { i = (size_t)hi; hi--; }

        int32_t logical_cx = pad_left + (int32_t)i * step + cs() / 2 - scroll_x;
        float dist_norm = (float)(logical_cx - scr_cx) / (float)step;
        float ad  = dist_norm < 0.0f ? -dist_norm : dist_norm;

        /* Performance cap: only rasterise covers within ±CF_MAX_SIDE of centre.
         * The fan keeps every cover on-screen (none cull off-screen), so without
         * this all 56 rasterise on every scroll event -> sluggish. Covers beyond
         * the cap are occluded by nearer ones anyway. */
        if (ad > (float)CF_MAX_SIDE) continue;

        /* Remap the linear logical position onto the converging fan (see
         * CF_FAN_*): ±1 sits ~170px out (mostly visible), outer covers bunch. */
        float sgn = dist_norm < 0.0f ? -1.0f : 1.0f;
        int32_t card_cx = scr_cx +
            (int32_t)(sgn * CF_FAN_SPREAD * (1.0f - expf(-CF_FAN_RATE * ad)));
        if (card_cx < -(int32_t)SCREEN_W || card_cx > 2 * (int32_t)SCREEN_W)
            continue;   /* fully off-screen */

        const uint16_t *src;
        int src_w, src_h;
        if (is_pixel_theme() && s_theme_art && s_pix_thumbs) {
            src   = s_pix_thumbs + i * PIX_THUMB_RES * PIX_THUMB_RES;
            src_w = PIX_THUMB_RES;
            src_h = PIX_THUMB_RES;
        } else {
            /* RAW covers (PSRAM copy when available -- the per-column sampling
             * below is exactly the read pattern that hurts through the flash
             * cache). PAPER also renders RAW here: the duotone happens in one
             * whole-canvas pass below, AFTER projection, so the grain stays
             * 1px (sampling the pre-dithered pool through the perspective
             * remap smeared it grey). */
            src   = thumb_src(i);
            src_w = ALBUM_THUMB_W;
            src_h = ALBUM_THUMB_H;
        }
        if (!src) continue;

        cf_render_card(card_cx, dist_norm, src, src_w, src_h);
    }

    if (is_paper_theme() && s_theme_art) cf_paper_dither(bg_px);

    if (s_fps_enabled) cf_profile_tick(t_prof0, t_prof1, esp_timer_get_time());

    lv_obj_invalidate(s_cf_img);
}

static void cf_init(lv_obj_t *screen)
{
    s_cf_buf = heap_caps_malloc((size_t)CF_PERSP_W * CF_PERSP_H * 2, MALLOC_CAP_SPIRAM);
    if (!s_cf_buf) { ESP_LOGW(TAG, "cf_init: PSRAM alloc failed"); return; }
    memset(s_cf_buf, 0, (size_t)CF_PERSP_W * CF_PERSP_H * 2);

    s_cf_dsc.header.cf   = LV_COLOR_FORMAT_RGB565;
    s_cf_dsc.header.w    = CF_PERSP_W;
    s_cf_dsc.header.h    = CF_PERSP_H;
    s_cf_dsc.data        = (const uint8_t *)s_cf_buf;
    s_cf_dsc.data_size   = (uint32_t)CF_PERSP_W * CF_PERSP_H * 2;

    s_cf_img = lv_image_create(screen);
    lv_image_set_src(s_cf_img, &s_cf_dsc);
    lv_obj_set_pos(s_cf_img, 0, CF_PERSP_Y);
    lv_obj_remove_flag(s_cf_img, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_antialias(s_cf_img, false);

    ESP_LOGI(TAG, "cf_init: %u B PSRAM", (unsigned)((size_t)CF_PERSP_W * CF_PERSP_H * 2));
}

static void cf_deinit(void)
{
    /* s_cf_img is a child of s_screen_browser which is deleted by the caller;
     * just null our reference.  Free the PSRAM backing buffer explicitly. */
    s_cf_img = NULL;
    if (s_cf_buf) { free(s_cf_buf); s_cf_buf = NULL; }
    memset(&s_cf_dsc, 0, sizeof s_cf_dsc);
}

/* =====================================================================
 * Feature 1: Gas-particle progress bar (Glyph theme only)
 * Dots bounce elastically inside the played zone [PROG_X, PROG_X+progress_px].
 * As the song progresses the right wall moves outward; the same dots have more
 * space, so bounces become less frequent -- the visual "slowing down" of gas
 * expanding into a larger container.
 * ===================================================================== */
static void prog_particles_stop(void)
{
    if (s_prog_particle_timer) {
        lv_timer_delete(s_prog_particle_timer);
        s_prog_particle_timer = NULL;
    }
    memset(s_prog_objs, 0, sizeof s_prog_objs);
    /* The tank + playhead are children of the screen and are freed when the
     * screen is torn down (same as the dots); just drop our references. */
    s_prog_tank = NULL;
    s_prog_head = NULL;
}

static void prog_particles_start(lv_obj_t *screen)
{
    if (!screen) return;
    prog_particles_stop();

    /* The "tank": a hairline-outlined capsule on the light ground (the
     * reference's fine instrument circles). Created before the dots so they
     * render inside it, and opaque (theme bg) so it hides the plain bar --
     * in Glyph the gas IS the progress indicator. */
    s_prog_tank = lv_obj_create(screen);
    lv_obj_set_size(s_prog_tank, PROG_W + 4, PROG_TANK_H);
    lv_obj_set_pos(s_prog_tank, PROG_X - 2, PROG_TANK_Y);
    lv_obj_set_style_bg_color(s_prog_tank, lv_color_hex(s_th->bg), 0);
    lv_obj_set_style_bg_opa(s_prog_tank, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_prog_tank, lv_color_hex(s_th->dim), 0);
    lv_obj_set_style_border_width(s_prog_tank, 1, 0);
    lv_obj_set_style_radius(s_prog_tank, PROG_TANK_H / 2, 0);
    lv_obj_set_style_pad_all(s_prog_tank, 0, 0);
    lv_obj_remove_flag(s_prog_tank, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    int y_lo = PROG_TANK_Y + 4;
    int y_span = PROG_TANK_H - 10;
    for (int i = 0; i < PROG_PART_COUNT; i++) {
        lv_obj_t *dot = lv_obj_create(screen);
        lv_obj_set_size(dot, 3, 3);
        lv_obj_set_style_radius(dot, 2, 0);    /* round gas molecule */
        lv_obj_set_style_border_width(dot, 0, 0);
        /* Ink dots; the accent is reserved for the playhead below. */
        lv_obj_set_style_bg_color(dot, lv_color_hex(s_th->text), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_80, 0);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        /* Scatter across the left of the chamber + full height so the gas looks
         * like a diffuse cloud from the first frame (not a packed column). */
        int x0 = PROG_X + 2 + (int)(esp_random() % 24);
        int y0 = y_lo + (int)(esp_random() % y_span);
        s_prog_pts[i].x = (int16_t)x0;
        s_prog_pts[i].y = (int16_t)y0;
        lv_obj_set_pos(dot, x0, y0);
        s_prog_objs[i] = dot;
        /* Brownian: independent random velocity on BOTH axes, no preferred
         * direction. Range -3..+3 each, never (0,0). */
        s_prog_pts[i].vx = (int8_t)((int)(esp_random() % 7) - 3);
        s_prog_pts[i].vy = (int8_t)((int)(esp_random() % 7) - 3);
        if (s_prog_pts[i].vx == 0 && s_prog_pts[i].vy == 0) s_prog_pts[i].vx = 2;
    }

    /* Bright playhead: a thin accent bar at the current progress point so the
     * play position is always visible (not only when a dot bounces off it). */
    s_prog_head = lv_obj_create(screen);
    lv_obj_set_size(s_prog_head, 3, PROG_TANK_H - 6);
    lv_obj_set_style_radius(s_prog_head, 1, 0);
    lv_obj_set_style_border_width(s_prog_head, 0, 0);
    lv_obj_set_style_bg_color(s_prog_head, lv_color_hex(accent_color()), 0);
    lv_obj_set_style_bg_opa(s_prog_head, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_prog_head, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(s_prog_head, PROG_X + 2, PROG_TANK_Y + 3);

    s_prog_particle_timer = lv_timer_create(prog_particle_tick_cb, 60, NULL);
}

static void prog_particle_tick_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_prog_objs[0]) return;
    if (s_track.duration_ms == 0) return;
    /* The tank lives on the now-playing screen; while another screen is shown
     * the 44 set_pos/set_style writes per tick are pure waste (and LVGL style
     * churn). Freeze the gas off-screen -- it resumes the moment NP returns. */
    if (lv_screen_active() != s_screen_np) return;

    int32_t progress_px = (int32_t)((uint64_t)s_track.progress_ms * PROG_W
                                    / s_track.duration_ms);
    int32_t right_wall  = PROG_X + progress_px;
    if (right_wall < PROG_X + 8) right_wall = PROG_X + 8;   /* min box */

    int16_t xmin = (int16_t)(PROG_X + 2);
    int16_t xmax = (int16_t)(right_wall);
    int16_t ymin = (int16_t)(PROG_TANK_Y + 4);
    int16_t ymax = (int16_t)(PROG_TANK_Y + PROG_TANK_H - 6);

    /* Move the playhead to the current progress point. */
    if (s_prog_head)
        lv_obj_set_pos(s_prog_head, (int)right_wall, PROG_TANK_Y + 3);

    for (int i = 0; i < PROG_PART_COUNT; i++) {
        lv_obj_t *dot = s_prog_objs[i];
        if (!dot) continue;

        /* Brownian kick: randomly nudge each axis so the path wanders in all
         * directions instead of running in straight L-R lines. */
        uint32_t r = esp_random();
        if (r & 1)  s_prog_pts[i].vx += ((r & 2) ? 1 : -1);
        if (r & 4)  s_prog_pts[i].vy += ((r & 8) ? 1 : -1);
        /* Clamp speed to keep the gas lively but bounded. */
        if (s_prog_pts[i].vx >  4) s_prog_pts[i].vx =  4;
        if (s_prog_pts[i].vx < -4) s_prog_pts[i].vx = -4;
        if (s_prog_pts[i].vy >  4) s_prog_pts[i].vy =  4;
        if (s_prog_pts[i].vy < -4) s_prog_pts[i].vy = -4;
        if (s_prog_pts[i].vx == 0 && s_prog_pts[i].vy == 0)
            s_prog_pts[i].vy = (r & 16) ? 2 : -2;   /* never freeze */

        s_prog_pts[i].x += s_prog_pts[i].vx;
        s_prog_pts[i].y += s_prog_pts[i].vy;

        /* Reflect off all four walls (right wall = the moving playhead). */
        if (s_prog_pts[i].x < xmin) { s_prog_pts[i].x = xmin; if (s_prog_pts[i].vx < 0) s_prog_pts[i].vx = -s_prog_pts[i].vx; }
        if (s_prog_pts[i].x > xmax) { s_prog_pts[i].x = xmax; if (s_prog_pts[i].vx > 0) s_prog_pts[i].vx = -s_prog_pts[i].vx; }
        if (s_prog_pts[i].y < ymin) { s_prog_pts[i].y = ymin; if (s_prog_pts[i].vy < 0) s_prog_pts[i].vy = -s_prog_pts[i].vy; }
        if (s_prog_pts[i].y > ymax) { s_prog_pts[i].y = ymax; if (s_prog_pts[i].vy > 0) s_prog_pts[i].vy = -s_prog_pts[i].vy; }

        lv_obj_set_pos(dot, s_prog_pts[i].x, s_prog_pts[i].y);
        /* Twinkle: vary opacity only -- colour stays the accent so the gas reads
         * as a single glowing substance. */
        lv_obj_set_style_bg_opa(dot, (lv_opa_t)(150 + esp_random() % 106), 0);
    }
}

/* =====================================================================
 * Feature 2: Volume page
 * Full-screen dot-matrix display; 8 cols × 10 rows = 80 dots.
 * Bottom row = 0 %, top row = 100 %. Dots below volume threshold are lit
 * in the accent colour; above threshold are dim track-colour.
 * ===================================================================== */
#define VOL_DOT_SZ    10    /* dot size in pixels */
#define VOL_DOT_STEP  20    /* centre-to-centre spacing */
/* Grid origin: centred horizontally, slight upward offset for the label below. */
#define VOL_GRID_X    ((SCREEN_W - (VOL_PAGE_COLS * VOL_DOT_STEP - (VOL_DOT_STEP - VOL_DOT_SZ))) / 2)
#define VOL_GRID_Y    60    /* top of the dot grid */

static void vol_page_dots_update(int pct)
{
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    if (s_vol_page_label) {
        char b[8];
        snprintf(b, sizeof b, "%d%%", pct);
        lv_label_set_text(s_vol_page_label, b);
    }
    for (int r = 0; r < VOL_PAGE_ROWS; r++) {
        /* r=0 is the bottom row (low volume); r=9 is the top row (high volume).
         * Dot is "active" when pct is above the midpoint of its band. */
        int threshold = r * 10 + 5;   /* 5, 15, 25 ... 95 */
        bool active   = (pct >= threshold);
        for (int c = 0; c < VOL_PAGE_COLS; c++) {
            int idx = r * VOL_PAGE_COLS + c;
            lv_obj_t *dot = s_vol_page_dots[idx];
            if (!dot) continue;
            if (active) {
                /* Ink dots on the light ground, like the reference dial. */
                lv_obj_set_style_bg_color(dot, lv_color_hex(s_th->text), 0);
                lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
            } else {
                lv_obj_set_style_bg_color(dot, lv_color_hex(s_th->track), 0);
                lv_obj_set_style_bg_opa(dot, (lv_opa_t)160, 0);
            }
        }
    }
}

static void vol_release_timer_cb(lv_timer_t *t)
{
    (void)t;
    s_vol_release_timer = NULL;
    if (s_np_volume) ui_request_volume(lv_slider_get_value(s_np_volume));
}

static void on_vol_page_drag(lv_event_t *e)
{
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    /* Map touch y to volume: top of grid = 100 %, bottom = 0 %. */
    int grid_top    = VOL_GRID_Y;
    int grid_bottom = VOL_GRID_Y + VOL_PAGE_ROWS * VOL_DOT_STEP;
    int pct = 100 - (int)((p.y - grid_top) * 100 / (grid_bottom - grid_top));
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    vol_page_dots_update(pct);
    if (s_np_volume) lv_slider_set_value(s_np_volume, pct, LV_ANIM_OFF);
    /* Debounce: send command 400 ms after the last move. */
    if (s_vol_release_timer) {
        lv_timer_reset(s_vol_release_timer);
    } else {
        s_vol_release_timer = lv_timer_create(vol_release_timer_cb, 400, NULL);
        lv_timer_set_repeat_count(s_vol_release_timer, 1);
    }
}

static void on_open_volume(lv_event_t *e)
{
    (void)e;
    if (!s_screen_volume) return;
    if (s_np_volume)
        vol_page_dots_update(lv_slider_get_value(s_np_volume));
    lv_screen_load(s_screen_volume);
}

static void on_vol_page_back(lv_event_t *e)
{
    (void)e;
    if (s_screen_np) lv_screen_load(s_screen_np);
}

static void build_volume_screen(void)
{
    s_screen_volume = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen_volume, lv_color_hex(s_th->bg), 0);
    lv_obj_set_style_bg_opa(s_screen_volume, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_screen_volume, LV_OBJ_FLAG_SCROLLABLE);

    /* Back button top-left (outlined pill, same as every GLYPH key). */
    lv_obj_t *back = lv_button_create(s_screen_volume);
    lv_obj_set_size(back, 120, 44);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_obj_set_style_bg_color(back, lv_color_hex(s_th->surface), 0);
    style_key_btn(back);
    lv_obj_add_event_cb(back, on_vol_page_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lbl = lv_label_create(back);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT "  BACK");
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(s_th->text), 0);
    lv_obj_set_style_text_font(back_lbl, font_sm(), 0);
    lv_obj_center(back_lbl);

    /* "VOLUME" title in the dotted heading voice. */
    lv_obj_t *title = lv_label_create(s_screen_volume);
    lv_label_set_text(title, "VOLUME");
    lv_obj_set_style_text_color(title, lv_color_hex(s_th->text), 0);
    lv_obj_set_style_text_font(title, font_lg(), 0);
    lv_obj_set_style_text_letter_space(title, 3, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    /* Dot grid: r=0 bottom row (low), r=9 top row (high).
     * Screen y increases downward, so row 9 is at y=VOL_GRID_Y and row 0
     * is at y = VOL_GRID_Y + 9*VOL_DOT_STEP. */
    memset(s_vol_page_dots, 0, sizeof s_vol_page_dots);
    for (int r = 0; r < VOL_PAGE_ROWS; r++) {
        int dot_y = VOL_GRID_Y + (VOL_PAGE_ROWS - 1 - r) * VOL_DOT_STEP;
        for (int c = 0; c < VOL_PAGE_COLS; c++) {
            int dot_x = VOL_GRID_X + c * VOL_DOT_STEP;
            lv_obj_t *dot = lv_obj_create(s_screen_volume);
            lv_obj_set_size(dot, VOL_DOT_SZ, VOL_DOT_SZ);
            lv_obj_set_style_radius(dot, VOL_DOT_SZ / 2, 0);
            lv_obj_set_style_border_width(dot, 0, 0);
            lv_obj_set_style_bg_opa(dot, (lv_opa_t)160, 0);
            lv_obj_set_style_bg_color(dot, lv_color_hex(s_th->track), 0);
            lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_pos(dot, dot_x, dot_y);
            s_vol_page_dots[r * VOL_PAGE_COLS + c] = dot;
        }
    }

    /* Percentage readout below the grid -- dotted numerals (heading voice). */
    s_vol_page_label = lv_label_create(s_screen_volume);
    lv_label_set_text(s_vol_page_label, "50%");
    lv_obj_set_style_text_color(s_vol_page_label, lv_color_hex(s_th->text), 0);
    lv_obj_set_style_text_font(s_vol_page_label, font_lg(), 0);
    lv_obj_set_style_text_letter_space(s_vol_page_label, 2, 0);
    lv_obj_align(s_vol_page_label, LV_ALIGN_BOTTOM_MID, 0, -24);

    /* Drag anywhere on screen to set volume. */
    lv_obj_add_event_cb(s_screen_volume, on_vol_page_drag, LV_EVENT_PRESSING,  NULL);
    lv_obj_add_event_cb(s_screen_volume, on_vol_page_drag, LV_EVENT_RELEASED,  NULL);
    lv_obj_add_event_cb(s_screen_volume, on_vol_page_drag, LV_EVENT_PRESS_LOST, NULL);
}

/* =====================================================================
 * Feature 3: WiFi dot strength meter (Glyph theme only)
 * Four round dots of rising size in the browser top-left corner. The first
 * `bars` dots light in the accent colour; the rest stay dim. Static -- no
 * timer -- updated only when the signal level changes.
 * ===================================================================== */
#define WIFI_DOT_BASE_Y  20   /* baseline the dots sit on (bottom-aligned) */
#define WIFI_DOT_X       8    /* left edge of the meter */
#define WIFI_DOT_PITCH   11   /* centre-to-centre horizontal spacing */

static void wifi_dots_stop(void)
{
    memset(s_wifi_dots, 0, sizeof s_wifi_dots);
}

/* Light the first `bars` dots in ink (the reference's black instrument dots);
 * dim the remainder to the hairline grey. */
static void wifi_dots_update_count(int bars)
{
    s_wifi_dot_count = bars;
    for (int i = 0; i < 4; i++) {
        lv_obj_t *dot = s_wifi_dots[i];
        if (!dot) continue;
        bool lit = (i < bars);
        lv_obj_set_style_bg_color(dot,
            lit ? lv_color_hex(s_th->text) : lv_color_hex(s_th->track), 0);
        lv_obj_set_style_bg_opa(dot, lit ? LV_OPA_COVER : (lv_opa_t)160, 0);
    }
}

static void wifi_dots_start(lv_obj_t *screen)
{
    if (!screen) return;
    wifi_dots_stop();
    for (int i = 0; i < 4; i++) {
        int sz = 4 + i * 2;   /* 4, 6, 8, 10 -- a rising "signal" ramp */
        lv_obj_t *dot = lv_obj_create(screen);
        lv_obj_set_size(dot, sz, sz);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(s_th->track), 0);
        lv_obj_set_style_bg_opa(dot, (lv_opa_t)90, 0);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        /* Bottom-aligned so the dots grow upward like signal bars. */
        lv_obj_set_pos(dot, WIFI_DOT_X + i * WIFI_DOT_PITCH,
                       WIFI_DOT_BASE_Y - sz);
        s_wifi_dots[i] = dot;
    }
    wifi_dots_update_count(s_wifi_dot_count);
}

/* =====================================================================
 * Feature 4: Offline title dissipation (Glyph theme only)
 * When WiFi drops, the NP title dissolves into rising dots; "OFFLINE"
 * fades in after. On reconnect the reverse happens and the real title
 * is restored.
 * ===================================================================== */
static void anim_set_bg_opa(void *obj, int32_t val)
{
    lv_obj_set_style_bg_opa((lv_obj_t *)obj, (lv_opa_t)val, 0);
}

static void dissolve_done_cb(lv_anim_t *a)
{
    (void)a;
    /* Called when the last dissolve dot's animation completes.
     * Clean up temp dots and show "OFFLINE". */
    for (int i = 0; i < DISSOLVE_DOT_COUNT; i++) {
        if (s_dissolve_dots[i]) {
            lv_obj_delete(s_dissolve_dots[i]);
            s_dissolve_dots[i] = NULL;
        }
    }
    if (s_np_title) {
        if (s_offline) lv_label_set_text(s_np_title, "OFFLINE");
        else           lv_label_set_text(s_np_title, s_track.title[0] ? s_track.title : "Nothing playing");
        lv_obj_remove_flag(s_np_title, LV_OBJ_FLAG_HIDDEN);
    }
}

static void title_dissolve(void)
{
    if (!s_np_title || !s_screen_np) return;
    lv_obj_add_flag(s_np_title, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < DISSOLVE_DOT_COUNT; i++) {
        if (s_dissolve_dots[i]) { lv_obj_delete(s_dissolve_dots[i]); s_dissolve_dots[i] = NULL; }
        int dot_x = 120 + (int)(esp_random() % 560);
        int dot_y = NP_TITLE_Y + 14;
        lv_obj_t *dot = lv_obj_create(s_screen_np);
        lv_obj_set_size(dot, 4, 4);
        lv_obj_set_style_radius(dot, 2, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(s_th->text), 0);
        lv_obj_set_style_bg_opa(dot, (lv_opa_t)200, 0);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(dot, dot_x, dot_y);
        s_dissolve_dots[i] = dot;

        /* Y drift: float upward 25-45 px. */
        lv_anim_t ay;
        lv_anim_init(&ay);
        lv_anim_set_var(&ay, dot);
        lv_anim_set_exec_cb(&ay, (lv_anim_exec_xcb_t)lv_obj_set_y);
        lv_anim_set_values(&ay, dot_y, dot_y - 25 - (int)(esp_random() % 20));
        lv_anim_set_time(&ay, 450);
        lv_anim_set_delay(&ay, (uint32_t)(i * 55));
        lv_anim_set_path_cb(&ay, lv_anim_path_ease_out);
        /* Attach completed callback to the last dot's animation. */
        if (i == DISSOLVE_DOT_COUNT - 1) lv_anim_set_completed_cb(&ay, dissolve_done_cb);
        lv_anim_start(&ay);

        /* Opacity fade-out. */
        lv_anim_t ao;
        lv_anim_init(&ao);
        lv_anim_set_var(&ao, dot);
        lv_anim_set_exec_cb(&ao, anim_set_bg_opa);
        lv_anim_set_values(&ao, 200, 0);
        lv_anim_set_time(&ao, 450);
        lv_anim_set_delay(&ao, (uint32_t)(i * 55));
        lv_anim_start(&ao);
    }
}

static void title_reform(void)
{
    if (!s_np_title || !s_screen_np) return;
    lv_obj_add_flag(s_np_title, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < DISSOLVE_DOT_COUNT; i++) {
        if (s_dissolve_dots[i]) { lv_obj_delete(s_dissolve_dots[i]); s_dissolve_dots[i] = NULL; }
        int dot_x = 120 + (int)(esp_random() % 560);
        int dot_y_from = NP_TITLE_Y + 14 - 30 - (int)(esp_random() % 20);
        int dot_y_to   = NP_TITLE_Y + 14;
        lv_obj_t *dot = lv_obj_create(s_screen_np);
        lv_obj_set_size(dot, 4, 4);
        lv_obj_set_style_radius(dot, 2, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(s_th->text), 0);
        lv_obj_set_style_bg_opa(dot, 0, 0);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(dot, dot_x, dot_y_from);
        s_dissolve_dots[i] = dot;

        lv_anim_t ay;
        lv_anim_init(&ay);
        lv_anim_set_var(&ay, dot);
        lv_anim_set_exec_cb(&ay, (lv_anim_exec_xcb_t)lv_obj_set_y);
        lv_anim_set_values(&ay, dot_y_from, dot_y_to);
        lv_anim_set_time(&ay, 400);
        lv_anim_set_delay(&ay, (uint32_t)(i * 45));
        lv_anim_set_path_cb(&ay, lv_anim_path_ease_in);
        if (i == DISSOLVE_DOT_COUNT - 1) lv_anim_set_completed_cb(&ay, dissolve_done_cb);
        lv_anim_start(&ay);

        lv_anim_t ao;
        lv_anim_init(&ao);
        lv_anim_set_var(&ao, dot);
        lv_anim_set_exec_cb(&ao, anim_set_bg_opa);
        lv_anim_set_values(&ao, 0, 200);
        lv_anim_set_time(&ao, 400);
        lv_anim_set_delay(&ao, (uint32_t)(i * 45));
        lv_anim_start(&ao);
    }
}

/* A left-aligned uppercase section header used throughout the Settings pages.
 * PAPER prints these as the references' accent corner labels -- vermilion
 * tracked-out captions naming each ruled field. */
static lv_obj_t *settings_header(lv_obj_t *parent, const char *txt, int x, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l,
        lv_color_hex(is_paper_theme() ? accent_color() : s_th->text2), 0);
    lv_obj_set_style_text_font(l, font_md(), 0);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, x, y);
    return l;
}

/* A transparent, vertically-scrollable category page filling the area below the
 * tab chips. Controls are placed with page-local coordinates. */
static lv_obj_t *settings_page(void)
{
    lv_obj_t *p = lv_obj_create(s_screen_settings);
    lv_obj_set_size(p, 800, 366);
    lv_obj_align(p, LV_ALIGN_TOP_MID, 0, 114);
    lv_obj_set_style_bg_opa(p, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_radius(p, 0, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_set_scrollbar_mode(p, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_dir(p, LV_DIR_VER);
    return p;
}

static void build_settings_screen(void)
{
    s_screen_settings = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen_settings, lv_color_hex(s_th->bg), 0);
    lv_obj_set_style_bg_opa(s_screen_settings, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(s_screen_settings, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *back = lv_button_create(s_screen_settings);
    lv_obj_set_size(back, 120, 44);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_obj_set_style_bg_color(back, lv_color_hex(s_th->surface), 0);
    style_key_btn(back);
    lv_obj_add_event_cb(back, on_settings_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lbl = lv_label_create(back);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT "  BACK");
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(s_th->text), 0);
    lv_obj_set_style_text_font(back_lbl, font_sm(), 0);
    lv_obj_center(back_lbl);

    lv_obj_t *title = lv_label_create(s_screen_settings);
    lv_label_set_text(title, "SETTINGS");
    lv_obj_set_style_text_color(title, lv_color_hex(s_th->text), 0);
    lv_obj_set_style_text_font(title, font_lg(), 0);
    /* Letter-spacing gives the uppercase title a cleaner, more deliberate feel. */
    lv_obj_set_style_text_letter_space(title, 3, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);
    paper_title_chip(title);

    /* Category tabs: one chip per page; tapping one swaps the visible page. */
    static const char *const k_tab_names[SET_TAB_COUNT] = { "DISPLAY", "SOUND" };
    for (int i = 0; i < SET_TAB_COUNT; i++) {
        lv_obj_t *chip = lv_button_create(s_screen_settings);
        lv_obj_set_size(chip, 220, 44);
        lv_obj_align(chip, LV_ALIGN_TOP_MID, (int)((i - 0.5f) * 232.0f), 60);
        style_key_btn(chip);
        style_button_press(chip);
        lv_obj_add_event_cb(chip, on_settings_tab, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        lv_obj_t *lbl = lv_label_create(chip);
        lv_label_set_text(lbl, k_tab_names[i]);
        lv_obj_set_style_text_font(lbl, font_sm(), 0);
        lv_obj_center(lbl);
        s_set_tabs[i]     = chip;
        s_set_tab_lbls[i] = lbl;
    }

    /* ====== DISPLAY: every visual setting on one scrolling page ====== */
    lv_obj_t *pg_disp = settings_page();
    s_set_pages[0] = pg_disp;

    /* DARK/LIGHT face toggle -- a separate axis from MODE: every mode has both
     * faces, this picks which palette of the pair is live. */
    settings_header(pg_disp, "APPEARANCE", 24, 6);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *btn = lv_button_create(pg_disp);
        lv_obj_set_size(btn, 256, 48);
        lv_obj_align(btn, LV_ALIGN_TOP_MID, (i == 0) ? -132 : 132, 38);
        style_key_btn(btn);
        style_button_press(btn);
        lv_obj_add_event_cb(btn, on_darklight_option, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_obj_set_style_text_font(lbl, font_md(), 0);
        lv_obj_center(lbl);
        s_dl_btns[i]   = btn;
        s_dl_labels[i] = lbl;
    }

    settings_header(pg_disp, "MODE", 24, 102);
    /* 4 design languages in one row, same pitch as the COLOUR swatches. */
    for (int i = 0; i < MODE_COUNT; i++) {
        lv_obj_t *btn = lv_button_create(pg_disp);
        lv_obj_set_size(btn, 168, 48);
        lv_obj_align(btn, LV_ALIGN_TOP_MID, (i * 176) - 264, 134);
        style_key_btn(btn);
        style_button_press(btn);
        lv_obj_add_event_cb(btn, on_theme_option, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_obj_set_style_text_font(lbl, font_sm(), 0);
        lv_obj_center(lbl);
        s_theme_btns[i]   = btn;
        s_theme_labels[i] = lbl;
    }

    /* THEME ALBUM ART directly under MODE (it modifies what MODE does to the
     * covers): OFF keeps real album art while the PIXEL/PAPER chrome stays. */
    settings_header(pg_disp, "THEME ALBUM ART", 24, 198);
    s_art_toggle_btn = lv_button_create(pg_disp);
    lv_obj_set_size(s_art_toggle_btn, 520, 48);
    lv_obj_align(s_art_toggle_btn, LV_ALIGN_TOP_MID, 0, 230);
    style_key_btn(s_art_toggle_btn);
    style_button_press(s_art_toggle_btn);
    lv_obj_add_event_cb(s_art_toggle_btn, on_art_toggle, LV_EVENT_CLICKED, NULL);
    s_art_toggle_lbl = lv_label_create(s_art_toggle_btn);
    lv_obj_set_style_text_font(s_art_toggle_lbl, font_md(), 0);
    lv_obj_center(s_art_toggle_lbl);

    /* COLOUR: 8 hues x 3 variants (vivid/deep/soft) -- values in ui_tune.h. */
    settings_header(pg_disp, "COLOUR", 24, 294);
    for (int i = 0; i < ACCENT_COUNT; i++) {
        lv_obj_t *btn = lv_button_create(pg_disp);
        lv_obj_set_size(btn, 88, 44);
        int row = i / TUNE_ACCENT_COLS, col = i % TUNE_ACCENT_COLS;
        /* 8 columns at 96px pitch, centred (mid column = 3.5). */
        lv_obj_align(btn, LV_ALIGN_TOP_MID,
                     (int)((col - (TUNE_ACCENT_COLS - 1) / 2.0f) * 96.0f),
                     326 + row * 52);
        /* Swatches keep their own border (the selection ring set by
         * refresh_accent_selection) -- square the corners only in PAPER. */
        lv_obj_set_style_radius(btn, is_paper_theme() ? 0 : 3, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, on_accent_option, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_obj_set_style_text_font(lbl, font_sm(), 0);
        lv_obj_center(lbl);
        s_accent_btns[i]   = btn;
        s_accent_labels[i] = lbl;
    }

    settings_header(pg_disp, "BROWSER STYLE", 24, 498);
    for (int i = 0; i < BROWSER_STYLE_COUNT; i++) {
        lv_obj_t *btn = lv_button_create(pg_disp);
        lv_obj_set_size(btn, 170, 48);
        lv_obj_align(btn, LV_ALIGN_TOP_MID, (i - 1) * 176, 530);
        style_key_btn(btn);
        lv_obj_add_event_cb(btn, on_browser_style_option, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_obj_set_style_text_font(lbl, font_sm(), 0);
        lv_obj_center(lbl);
        s_brstyle_btns[i]   = btn;
        s_brstyle_labels[i] = lbl;
    }

    /* FONT chooser: hidden in GLYPH and PAPER -- those themes' bespoke fonts
     * (round-dot / teletype mono) are fixed and the SANS/SLAB choice doesn't
     * apply. Clear the refs so refresh_font_selection() skips the
     * (non-existent) buttons. Everything below flows from y0 so hiding the
     * section closes its slot instead of leaving a 100px hole. */
    int y0 = 694;   /* base of the post-FONT stack when FONT is shown */
    if (is_glyph_theme() || is_paper_theme()) {
        memset(s_font_btns,   0, sizeof s_font_btns);
        memset(s_font_labels, 0, sizeof s_font_labels);
        y0 = 594;   /* FONT hidden: pull the rest of the column up */
    } else {
        settings_header(pg_disp, "FONT", 24, 594);
        static const char *const k_font_names[] = { "SANS", "SLAB" };
        for (int i = 0; i < 2; i++) {
            lv_obj_t *btn = lv_button_create(pg_disp);
            lv_obj_set_size(btn, 256, 48);
            lv_obj_align(btn, LV_ALIGN_TOP_MID, (i == 0) ? -132 : 132, 626);
            style_key_btn(btn);
            style_button_press(btn);
            lv_obj_add_event_cb(btn, on_font_option, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
            lv_obj_t *lbl = lv_label_create(btn);
            lv_label_set_text(lbl, k_font_names[i]);
            lv_obj_set_style_text_font(lbl, font_md(), 0);
            lv_obj_center(lbl);
            s_font_btns[i]   = btn;
            s_font_labels[i] = lbl;
        }
    }

    settings_header(pg_disp, "SELECTION LINE", 24, y0);
    s_line_toggle_btn = lv_button_create(pg_disp);
    lv_obj_set_size(s_line_toggle_btn, 520, 48);
    lv_obj_align(s_line_toggle_btn, LV_ALIGN_TOP_MID, 0, y0 + 32);
    style_key_btn(s_line_toggle_btn);
    style_button_press(s_line_toggle_btn);
    lv_obj_add_event_cb(s_line_toggle_btn, on_line_toggle, LV_EVENT_CLICKED, NULL);
    s_line_toggle_lbl = lv_label_create(s_line_toggle_btn);
    lv_obj_set_style_text_font(s_line_toggle_lbl, font_md(), 0);
    lv_obj_center(s_line_toggle_lbl);

    /* Backlight brightness: header + live "NN%" readout, full-width slider.
     * Live-applies on drag (VALUE_CHANGED); persists to NVS on release. */
    settings_header(pg_disp, "BRIGHTNESS", 24, y0 + 110);
    s_brightness_val = lv_label_create(pg_disp);
    lv_obj_set_style_text_color(s_brightness_val, lv_color_hex(s_th->text), 0);
    lv_obj_set_style_text_font(s_brightness_val, font_md(), 0);
    lv_obj_align(s_brightness_val, LV_ALIGN_TOP_RIGHT, -140, y0 + 110);
    {
        char b[8];
        snprintf(b, sizeof b, "%d%%", s_brightness);
        lv_label_set_text(s_brightness_val, b);
    }
    s_brightness_slider = lv_slider_create(pg_disp);
    lv_obj_set_size(s_brightness_slider, 520, 16);
    lv_obj_align(s_brightness_slider, LV_ALIGN_TOP_MID, 0, y0 + 146);
    lv_slider_set_range(s_brightness_slider, BRIGHTNESS_MIN, BRIGHTNESS_MAX);
    lv_slider_set_value(s_brightness_slider, s_brightness, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_brightness_slider, lv_color_hex(s_th->track), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_brightness_slider, lv_color_hex(accent_color()), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_brightness_slider, lv_color_hex(accent_color()), LV_PART_KNOB);
    lv_obj_set_style_radius(s_brightness_slider, is_paper_theme() ? 0 : 4, LV_PART_MAIN);
    lv_obj_set_style_radius(s_brightness_slider, is_paper_theme() ? 0 : 4, LV_PART_INDICATOR);
    if (is_paper_theme()) lv_obj_set_style_radius(s_brightness_slider, 0, LV_PART_KNOB);
    lv_obj_add_event_cb(s_brightness_slider, on_brightness_changed,  LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_brightness_slider, on_brightness_released, LV_EVENT_RELEASED,      NULL);

    settings_header(pg_disp, "FPS DISPLAY", 24, y0 + 200);
    s_fps_toggle_btn = lv_button_create(pg_disp);
    lv_obj_set_size(s_fps_toggle_btn, 520, 48);
    lv_obj_align(s_fps_toggle_btn, LV_ALIGN_TOP_MID, 0, y0 + 232);
    style_key_btn(s_fps_toggle_btn);
    style_button_press(s_fps_toggle_btn);
    lv_obj_add_event_cb(s_fps_toggle_btn, on_fps_toggle, LV_EVENT_CLICKED, NULL);
    s_fps_toggle_lbl = lv_label_create(s_fps_toggle_btn);
    lv_obj_set_style_text_font(s_fps_toggle_lbl, font_md(), 0);
    lv_obj_center(s_fps_toggle_lbl);

    settings_header(pg_disp, "MENU TRANSITION", 24, y0 + 304);
    for (int i = 0; i < UI_TRANSITION_COUNT; i++) {
        lv_obj_t *btn = lv_button_create(pg_disp);
        lv_obj_set_size(btn, 520, 48);
        lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, y0 + 336 + i * 54);
        style_key_btn(btn);
        lv_obj_add_event_cb(btn, on_transition_option, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_obj_set_style_text_font(lbl, font_md(), 0);
        lv_obj_center(lbl);
        s_opt_btns[i]   = btn;
        s_opt_labels[i] = lbl;
    }

    /* =============================== SOUND ============================== */
    lv_obj_t *pg_snd = settings_page();
    s_set_pages[1] = pg_snd;

    settings_header(pg_snd, "SOUND", 24, 6);
    s_sound_toggle_btn = lv_button_create(pg_snd);
    lv_obj_set_size(s_sound_toggle_btn, 520, 48);
    lv_obj_align(s_sound_toggle_btn, LV_ALIGN_TOP_MID, 0, 38);
    style_key_btn(s_sound_toggle_btn);
    style_button_press(s_sound_toggle_btn);
    lv_obj_add_event_cb(s_sound_toggle_btn, on_sound_toggle, LV_EVENT_CLICKED, NULL);
    s_sound_toggle_lbl = lv_label_create(s_sound_toggle_btn);
    lv_obj_set_style_text_font(s_sound_toggle_lbl, font_md(), 0);
    lv_obj_center(s_sound_toggle_lbl);

    /* VOLUME: UI-sound playback level. Mirrors the BRIGHTNESS control. */
    settings_header(pg_snd, "VOLUME", 24, 96);
    s_volume_val = lv_label_create(pg_snd);
    lv_obj_set_style_text_color(s_volume_val, lv_color_hex(s_th->text), 0);
    lv_obj_set_style_text_font(s_volume_val, font_md(), 0);
    lv_obj_align(s_volume_val, LV_ALIGN_TOP_RIGHT, -140, 96);
    {
        char b[8];
        snprintf(b, sizeof b, "%d%%", audio_get_volume());
        lv_label_set_text(s_volume_val, b);
    }
    s_volume_slider = lv_slider_create(pg_snd);
    lv_obj_set_size(s_volume_slider, 520, 16);
    lv_obj_align(s_volume_slider, LV_ALIGN_TOP_MID, 0, 128);
    lv_slider_set_range(s_volume_slider, 0, 100);
    lv_slider_set_value(s_volume_slider, audio_get_volume(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_volume_slider, lv_color_hex(s_th->track), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_volume_slider, lv_color_hex(accent_color()), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_volume_slider, lv_color_hex(accent_color()), LV_PART_KNOB);
    lv_obj_set_style_radius(s_volume_slider, is_paper_theme() ? 0 : 4, LV_PART_MAIN);
    lv_obj_set_style_radius(s_volume_slider, is_paper_theme() ? 0 : 4, LV_PART_INDICATOR);
    if (is_paper_theme()) lv_obj_set_style_radius(s_volume_slider, 0, LV_PART_KNOB);
    lv_obj_add_event_cb(s_volume_slider, on_volume_changed,  LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_volume_slider, on_volume_released, LV_EVENT_RELEASED,      NULL);

    /* SOUND SET: AUTO (follow MODE) + one chip per named sound design. */
    settings_header(pg_snd, "SOUND SET", 24, 188);
    s_sndset_opt_count = audio_set_count() + 1;   /* AUTO + named sets */
    if (s_sndset_opt_count > SND_SET_OPTS) s_sndset_opt_count = SND_SET_OPTS;
    for (int i = 0; i < s_sndset_opt_count; i++) {
        lv_obj_t *btn = lv_button_create(pg_snd);
        lv_obj_set_size(btn, 124, 44);
        int row = i / 4, col = i % 4;
        lv_obj_align(btn, LV_ALIGN_TOP_MID, (int)((col - 1.5f) * 132.0f), 220 + row * 52);
        style_key_btn(btn);
        style_button_press(btn);
        lv_obj_add_event_cb(btn, on_sound_set_option, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, (i == 0) ? "AUTO" : audio_set_name(i - 1));
        lv_obj_set_style_text_font(lbl, font_sm(), 0);
        lv_obj_center(lbl);
        s_sndset_btns[i] = btn;
        s_sndset_lbls[i] = lbl;
    }

    /* PAPER form furniture goes on LAST so page content scrolling past the
     * bottom edge slides UNDER the printed frame, not over it. */
    if (is_paper_theme()) {
        paper_frame(s_screen_settings);
        paper_rule(s_screen_settings, 8, 108, SCREEN_W - 16, 1);
    }

    /* Show the active page (preserved across theme rebuilds), hide the rest. */
    if (s_set_tab >= SET_TAB_COUNT) s_set_tab = 0;
    for (int i = 0; i < SET_TAB_COUNT; i++) {
        if (i == (int)s_set_tab) lv_obj_remove_flag(s_set_pages[i], LV_OBJ_FLAG_HIDDEN);
        else                     lv_obj_add_flag(s_set_pages[i], LV_OBJ_FLAG_HIDDEN);
    }

    refresh_settings_tabs();
    refresh_settings_selection();
    refresh_theme_selection();
    refresh_accent_selection();
    refresh_browser_style_selection();
    refresh_line_selection();
    refresh_font_selection();
    refresh_fps_selection();
    refresh_sound_selection();
    refresh_sound_set_selection();
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

static void build_devices_screen(void)
{
    s_screen_devices = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen_devices, lv_color_hex(s_th->bg), 0);
    lv_obj_set_style_bg_opa(s_screen_devices, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_screen_devices, LV_OBJ_FLAG_SCROLLABLE);
    if (is_paper_theme()) {
        paper_frame(s_screen_devices);
        paper_rule(s_screen_devices, 8, 60, SCREEN_W - 16, 1);
    }

    lv_obj_t *back = lv_button_create(s_screen_devices);
    lv_obj_set_size(back, 120, 44);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_obj_set_style_bg_color(back, lv_color_hex(s_th->surface), 0);
    style_key_btn(back);
    lv_obj_add_event_cb(back, on_devices_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lbl = lv_label_create(back);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT "  BACK");
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(s_th->text), 0);
    lv_obj_set_style_text_font(back_lbl, font_sm(), 0);
    lv_obj_center(back_lbl);

    lv_obj_t *title = lv_label_create(s_screen_devices);
    lv_label_set_text(title, "DEVICES");
    lv_obj_set_style_text_color(title, lv_color_hex(s_th->text), 0);
    lv_obj_set_style_text_font(title, font_lg(), 0);
    lv_obj_set_style_text_letter_space(title, 3, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);
    paper_title_chip(title);

    /* Scrollable list container; ui_set_devices() fills it with one row per
     * device. Starts empty (on_open_devices shows a placeholder). */
    s_dev_list = lv_obj_create(s_screen_devices);
    lv_obj_set_size(s_dev_list, 760, 384);
    lv_obj_align(s_dev_list, LV_ALIGN_TOP_MID, 0, 66);
    lv_obj_set_style_bg_opa(s_dev_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_dev_list, 0, 0);
    lv_obj_set_style_pad_all(s_dev_list, 4, 0);
    lv_obj_set_style_pad_row(s_dev_list, 8, 0);
    lv_obj_set_flex_flow(s_dev_list, LV_FLEX_FLOW_COLUMN);
}

static void on_open_devices(lv_event_t *e)
{
    (void)e;
    if (!s_screen_devices) return;
    /* Placeholder while the blocking device fetch runs on the Spotify task. */
    if (s_dev_list) {
        lv_obj_clean(s_dev_list);
        s_dev_entry_count = 0;
        lv_obj_t *lbl = lv_label_create(s_dev_list);
        lv_label_set_text(lbl, "Scanning...");
        lv_obj_set_style_text_color(lbl, lv_color_hex(s_th->text2), 0);
        lv_obj_set_style_text_font(lbl, font_md(), 0);
    }
    lv_screen_load(s_screen_devices);   /* instant, like settings */
    ui_request_get_devices();
}

static void on_devices_back(lv_event_t *e)
{
    (void)e;
    if (s_screen_browser) lv_screen_load(s_screen_browser);
}

static void on_device_tap(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_dev_entry_count) return;
    const ui_device_t *d = &s_dev_entries[i];
    if (d->is_sonos) ui_request_select_sonos(d->id);
    else             ui_request_transfer(d->id);
    /* Jump to now-playing; the settle re-poll populates it. */
    if (s_screen_np) lv_screen_load(s_screen_np);
}

static void on_transition_option(lv_event_t *e)
{
    ui_transition_t style = (ui_transition_t)(uintptr_t)lv_event_get_user_data(e);
    ui_set_transition_style(style);
    save_transition(s_transition);
    refresh_settings_selection();
    audio_play(AUDIO_SFX_SELECT);
    ESP_LOGI(TAG, "transition style -> %s", k_transition_names[s_transition]);
}

static void on_theme_option(lv_event_t *e)
{
    uint8_t idx = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    if (idx >= MODE_COUNT || idx == s_mode) return;
    s_mode = idx;
    apply_palette();
    save_theme(idx);
    apply_audio_theme();   /* switch the UI-sound palette to match the new MODE */
    audio_play(AUDIO_SFX_SELECT);   /* previews the new MODE's sound palette */
    ESP_LOGI(TAG, "mode -> %s", k_mode_names[idx]);
    /* Re-skin by rebuilding all three screens, but defer it: deleting the
     * active settings screen from inside its own button handler is unsafe. */
    lv_async_call(apply_theme_cb, NULL);
}

static void on_darklight_option(lv_event_t *e)
{
    bool dark = ((uintptr_t)lv_event_get_user_data(e) == 0);
    if (dark == s_dark) return;
    s_dark = dark;
    apply_palette();
    save_dark(s_dark ? 1 : 0);
    audio_play(AUDIO_SFX_SELECT);
    ESP_LOGI(TAG, "appearance -> %s", s_dark ? "DARK" : "LIGHT");
    lv_async_call(apply_theme_cb, NULL);   /* same full re-skin as a MODE change */
}

static void on_art_toggle(lv_event_t *e)
{
    (void)e;
    s_theme_art = !s_theme_art;
    save_theme_art(s_theme_art ? 1 : 0);
    audio_play(AUDIO_SFX_TICK);
    ESP_LOGI(TAG, "theme album art -> %s", s_theme_art ? "ON" : "OFF");
    /* Pools, card sources and now-playing art all re-derive from s_theme_art
     * during a rebuild -- reuse the theme path. */
    lv_async_call(apply_theme_cb, NULL);
}

static void on_accent_option(lv_event_t *e)
{
    uint8_t idx = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    if (idx >= ACCENT_COUNT || idx == s_accent) return;
    s_accent = idx;
    save_accent(idx);
    audio_play(AUDIO_SFX_SELECT);
    ESP_LOGI(TAG, "accent -> #%06X (swatch %d)", (unsigned)k_accents[idx], (int)idx);
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
    bool      was_devices = (active == s_screen_devices);
    int       saved_card  = s_centered_card;   /* preserve carousel position */

    lv_obj_t *old_browser  = s_screen_browser;
    lv_obj_t *old_np       = s_screen_np;
    lv_obj_t *old_settings = s_screen_settings;
    lv_obj_t *old_devices  = s_screen_devices;

    /* Stop all animated features before deleting old screens. */
    cf_deinit();   /* free CF canvas PSRAM; lv_image is a child of old_browser */
    prog_particles_stop();
    wifi_dots_stop();
    memset(s_wifi_bars, 0, sizeof s_wifi_bars);
    memset(s_prog_objs, 0, sizeof s_prog_objs);
    memset(s_wifi_dots, 0, sizeof s_wifi_dots);
    memset(s_dissolve_dots, 0, sizeof s_dissolve_dots);
    s_paper_cursor = NULL;   /* children of the screens about to be deleted; */
    s_br_index_lbl = NULL;   /* the builders recreate them when PAPER is on  */

    lv_obj_t *old_volume = s_screen_volume;
    s_screen_volume  = NULL;
    s_vol_page_label = NULL;
    memset(s_vol_page_dots, 0, sizeof s_vol_page_dots);

    /* Free the theme-look thumbnail pools; build_browser_screen() reallocates
     * whichever the new theme needs (PIXEL / PAPER). */
    if (s_pix_thumbs)   { heap_caps_free(s_pix_thumbs);   s_pix_thumbs = NULL; }
    if (s_paper_thumbs) { heap_caps_free(s_paper_thumbs); s_paper_thumbs = NULL; }

    build_browser_screen();
    build_np_screen();
    build_settings_screen();
    build_devices_screen();
    if (is_glyph_theme()) build_volume_screen();

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
        if (s_br_index_lbl) {
            char ib[20];
            snprintf(ib, sizeof ib, "%02d / %02d", saved_card + 1, (int)s_card_count);
            lv_label_set_text(s_br_index_lbl, ib);
        }
        apply_card_transforms();
    }

    /* Restore now-playing labels from cached track state. */
    if (s_track.title[0]  && s_np_title)  lv_label_set_text(s_np_title,  s_track.title);
    if (s_track.artist[0] && s_np_artist) lv_label_set_text(s_np_artist, s_track.artist);
    if (s_np_device) lv_label_set_text(s_np_device, s_track.device_name[0] ? s_track.device_name : "");
    if (s_track.volume_pct >= 0 && s_np_volume) {
        lv_slider_set_value(s_np_volume, s_track.volume_pct, LV_ANIM_OFF);
        if (is_glyph_theme() && s_screen_volume)
            vol_page_dots_update(s_track.volume_pct);
    }
    update_progress_bar();

    /* If entering or leaving PIXEL/PAPER, update the now-playing art image
     * immediately from the cached raw art pointer (no need to wait for the
     * next poll). */
    if (s_last_raw_art && s_last_raw_w > 0 && s_np_art) {
        if (is_pixel_theme() && s_theme_art) {
            if (!s_pix_art_buf)
                s_pix_art_buf = heap_caps_malloc(
                    PIX_ART_RES * PIX_ART_RES * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
            if (s_pix_art_buf) {
                pixelate_rgb565((const uint16_t *)s_last_raw_art,
                                s_last_raw_w, s_last_raw_h,
                                s_pix_art_buf, PIX_ART_RES, PIX_ART_RES);
                s_pix_art_dsc.header.cf  = LV_COLOR_FORMAT_RGB565;
                s_pix_art_dsc.header.w   = PIX_ART_RES;
                s_pix_art_dsc.header.h   = PIX_ART_RES;
                s_pix_art_dsc.data       = (const uint8_t *)s_pix_art_buf;
                s_pix_art_dsc.data_size  = PIX_ART_RES * PIX_ART_RES * sizeof(uint16_t);
                lv_image_set_src(s_np_art, &s_pix_art_dsc);
                lv_image_set_antialias(s_np_art, false);
                lv_obj_invalidate(s_np_art);
            }
        } else if (is_paper_theme() && s_theme_art) {
            if (!s_paper_art_buf)
                s_paper_art_buf = heap_caps_malloc(
                    PAPER_ART_RES * PAPER_ART_RES * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
            if (s_paper_art_buf) {
                paperize_rgb565((const uint16_t *)s_last_raw_art,
                                s_last_raw_w, s_last_raw_h,
                                s_paper_art_buf, PAPER_ART_RES, PAPER_ART_RES);
                s_paper_art_dsc.header.cf  = LV_COLOR_FORMAT_RGB565;
                s_paper_art_dsc.header.w   = PAPER_ART_RES;
                s_paper_art_dsc.header.h   = PAPER_ART_RES;
                s_paper_art_dsc.data       = (const uint8_t *)s_paper_art_buf;
                s_paper_art_dsc.data_size  = PAPER_ART_RES * PAPER_ART_RES * sizeof(uint16_t);
                lv_image_set_src(s_np_art, &s_paper_art_dsc);
                lv_image_set_antialias(s_np_art, false);
                lv_obj_invalidate(s_np_art);
            }
        } else {
            /* Leaving PIXEL/PAPER: restore full-resolution art. */
            s_art_dsc->header.cf  = LV_COLOR_FORMAT_RGB565;
            s_art_dsc->header.w   = s_last_raw_w;
            s_art_dsc->header.h   = s_last_raw_h;
            s_art_dsc->data       = s_last_raw_art;
            s_art_dsc->data_size  = (uint32_t)s_last_raw_w * s_last_raw_h * 2;
            lv_image_set_src(s_np_art, s_art_dsc);
            lv_image_set_antialias(s_np_art, true);
            lv_obj_invalidate(s_np_art);
        }
    }

    /* Start the Glyph dot-chrome features for the new screens. */
    if (is_glyph_theme()) {
        prog_particles_start(s_screen_np);
        wifi_dots_start(s_screen_browser);
        wifi_dots_update_count(s_wifi_dot_count);   /* restore last-known signal strength */
        if (s_screen_volume)
            vol_page_dots_update(s_np_volume ? lv_slider_get_value(s_np_volume) : 50);
    }

    /* Activate the equivalent new screen first -- the active screen can't be
     * deleted -- then drop the old ones. */
    bool was_volume = (active == old_volume);
    lv_screen_load(was_np ? s_screen_np :
                   was_setting ? s_screen_settings :
                   was_devices ? s_screen_devices :
                   was_volume  ? (s_screen_volume ? s_screen_volume : s_screen_browser) :
                   s_screen_browser);
    /* Clear any input-device reference to a widget on the screens about to be
     * deleted. Without this, rapidly switching font/theme (which deletes the
     * settings screen the user is still touching) leaves the indev pointing at
     * a freed object -> use-after-free crash. */
    lv_indev_reset(NULL, NULL);
    lv_obj_delete(old_browser);
    lv_obj_delete(old_np);
    lv_obj_delete(old_settings);
    if (old_devices) lv_obj_delete(old_devices);
    if (old_volume)  lv_obj_delete(old_volume);
}

static void load_settings(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_SETTINGS_NS, NVS_READONLY, &h) != ESP_OK) return;  /* unset -> defaults */
    uint8_t v = UI_TRANSITION_NONE;
    if (nvs_get_u8(h, NVS_KEY_TRANSITION, &v) == ESP_OK && v < UI_TRANSITION_COUNT) {
        s_transition = (ui_transition_t)v;
    }
    uint8_t t = MODE_BASIC;
    if (nvs_get_u8(h, NVS_KEY_MODE, &t) == ESP_OK && t < MODE_COUNT) {
        s_mode = t;
    }
    uint8_t dk = 1;
    if (nvs_get_u8(h, NVS_KEY_DARK, &dk) == ESP_OK) s_dark = (dk != 0);
    apply_palette();
    uint8_t ta = 1;
    if (nvs_get_u8(h, NVS_KEY_THEME_ART, &ta) == ESP_OK) s_theme_art = (ta != 0);
    uint8_t ac = 0;
    if (nvs_get_u8(h, NVS_KEY_ACCENT, &ac) == ESP_OK && ac < ACCENT_COUNT) {
        s_accent = ac;
    }
    uint8_t bs = BROWSER_CAROUSEL;
    if (nvs_get_u8(h, NVS_KEY_BROWSER_STYLE, &bs) == ESP_OK && bs < BROWSER_STYLE_COUNT) {
        s_browser_style = bs;
    }
    uint8_t sl = 1;
    if (nvs_get_u8(h, NVS_KEY_SEL_LINE, &sl) == ESP_OK) s_show_sel_line = (sl != 0);
    uint8_t br = BRIGHTNESS_DEFAULT;
    if (nvs_get_u8(h, NVS_KEY_BRIGHTNESS, &br) == ESP_OK) {
        if (br < BRIGHTNESS_MIN) br = BRIGHTNESS_MIN;
        if (br > BRIGHTNESS_MAX) br = BRIGHTNESS_MAX;
        s_brightness = br;
    }
    uint8_t fn = FONT_SANS;
    if (nvs_get_u8(h, NVS_KEY_FONT, &fn) == ESP_OK && fn <= FONT_SLAB) {
        s_font_choice = fn;
    }
    uint8_t fps = 0;
    if (nvs_get_u8(h, NVS_KEY_FPS, &fps) == ESP_OK) s_fps_enabled = (fps != 0);
    uint8_t snd = 1;
    if (nvs_get_u8(h, NVS_KEY_SOUND, &snd) == ESP_OK) audio_set_enabled(snd != 0);
    uint8_t vol = 0;
    if (nvs_get_u8(h, NVS_KEY_VOLUME, &vol) == ESP_OK) audio_set_volume(vol);
    uint8_t ss = 0;        /* 0 = AUTO, else named-set index + 1 */
    if (nvs_get_u8(h, NVS_KEY_SOUND_SET, &ss) == ESP_OK)
        audio_set_set(ss == 0 ? -1 : (int)ss - 1);
    nvs_close(h);
    apply_audio_theme();   /* AUTO target follows the restored MODE */
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
    nvs_set_u8(h, NVS_KEY_MODE, idx);
    nvs_commit(h);
    nvs_close(h);
}

static void save_dark(uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_SETTINGS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY_DARK, v);
    nvs_commit(h);
    nvs_close(h);
}

static void save_theme_art(uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_SETTINGS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY_THEME_ART, v);
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

static void save_sel_line(uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_SETTINGS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY_SEL_LINE, v);
    nvs_commit(h);
    nvs_close(h);
}

static void save_brightness(uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_SETTINGS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY_BRIGHTNESS, v);
    nvs_commit(h);
    nvs_close(h);
}

static void save_font(uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_SETTINGS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY_FONT, v);
    nvs_commit(h);
    nvs_close(h);
}

static void save_fps(uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_SETTINGS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY_FPS, v);
    nvs_commit(h);
    nvs_close(h);
}

static void save_sound(uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_SETTINGS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY_SOUND, v);
    nvs_commit(h);
    nvs_close(h);
}

static void save_volume(uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_SETTINGS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY_VOLUME, v);
    nvs_commit(h);
    nvs_close(h);
}

/* Map the active visual MODE to a UI-sound palette so the sounds match the look:
 * PIXEL -> chiptune squares, GLYPH -> ambient, PAPER -> typewriter clicks, the
 * rest -> the clean modern set. */
static void apply_audio_theme(void)
{
    audio_theme_t at = AUDIO_THEME_MODERN;
    if (is_pixel_theme())                  at = AUDIO_THEME_PIXEL;
    else if (is_glyph_theme())             at = AUDIO_THEME_AMBIENT;
    else if (is_paper_theme())             at = AUDIO_THEME_TELEX;
    audio_set_theme(at);
}

void ui_init(lv_image_dsc_t *art_dsc)
{
    s_art_dsc = art_dsc;

    /* Runtime tiny_ttf fonts are DISABLED on this target. LVGL's bundled
     * stb_truetype rasterizer asserts (stb_truetype_htcw.h:3673,
     * "z->ey >= scan_y_top") while rasterizing Montserrat at 24/28 px on the
     * ESP32-P4, crashing on the first text render -- and its glyph cache also
     * fails first ("cache not allocated"). This reproduces on both LVGL 9.4.0
     * and 9.5.0 and is independent of the glyph-cache count or LV_CACHE_DEF_SIZE.
     * Until tiny_ttf is fixed, fall back to the compiled lv_font_montserrat_*
     * bitmap fonts -- the same approach the hardware-verified CYD build uses.
     * The font_lg/md/sm accessors already return the compiled fonts when the
     * s_font_* pointers are NULL, so leaving them unset (their static default)
     * selects bitmap rendering everywhere. Caveats while disabled: glyphs the
     * compiled Montserrat lacks (most accented/non-Latin chars) won't render,
     * and the Settings font choice (Arvo/Slab) is inert. The embedded
     * Montserrat/DejaVu/Arvo TTF blobs and the EMBED_TXTFILES entries are kept
     * so re-enabling is a one-spot change once the rasterizer is sorted. */

    bsp_display_lock(-1);

    load_settings();   /* restore saved transition style (default NONE if unset) */
    bsp_display_brightness_set(s_brightness);   /* restore saved backlight level */
    build_browser_screen();
    build_np_screen();
    build_settings_screen();
    build_devices_screen();
    if (is_glyph_theme()) build_volume_screen();
    if (is_glyph_theme()) {
        prog_particles_start(s_screen_np);
        wifi_dots_start(s_screen_browser);
    }
    lv_screen_load(s_screen_browser);

    /* Local-progress simulation -- ticks 200 ms of progress every 200 ms
     * so the bar advances smoothly between Spotify polls. */
    lv_timer_create(progress_timer_cb, 200, NULL);

    /* WiFi-strength indicator: poll RSSI every 5 s. */
    lv_timer_create(wifi_timer_cb, 5000, NULL);

    /* Auto-dim: 1 s tick gives a snappy "wake on touch" while the dim/restore
     * itself is cheap (one LEDC duty update on state change only). */
    lv_timer_create(idle_timer_cb, 1000, NULL);

    /* FPS counter: count presented frames (RENDER_READY) on the default display
     * and snapshot the achieved rate once per second. Always measuring; the
     * label is only shown when s_fps_enabled is set via the Settings toggle. */
    lv_display_t *fps_disp = lv_display_get_default();
    if (fps_disp) {
        lv_display_add_event_cb(fps_disp, fps_render_ready_cb, LV_EVENT_RENDER_READY, NULL);
    }
    lv_timer_create(fps_timer_cb, 1000, NULL);

    bsp_display_unlock();
}

void ui_set_track_info(const spotify_track_t *info)
{
    if (bsp_display_lock(1000) != ESP_OK) {
        ESP_LOGW(TAG, "ui_set_track_info: display lock timeout, skipping");
        return;
    }
    if (!info) {
        /* No active playback (HTTP 204) or transient error -- stop the
         * progress simulation but keep the last track title/artist visible
         * rather than blanking them. Labels only clear on the first boot
         * before any track has been seen (title still empty). */
        s_track.is_playing = false;
    } else {
        bool same_track = (strncmp(info->title, s_track.title, sizeof s_track.title) == 0);
        uint32_t keep_progress = info->progress_ms;
        if (s_seek_guard_until && same_track) {
            uint32_t now = lv_tick_get();
            uint32_t expected = s_seek_anchor_ms +
                (s_track.is_playing ? (now - s_seek_anchor_tick) : 0);
            uint32_t diff = (info->progress_ms > expected)
                          ? info->progress_ms - expected
                          : expected - info->progress_ms;
            if ((int32_t)(now - s_seek_guard_until) < 0 && diff > SEEK_GUARD_TOL_MS) {
                keep_progress = s_track.progress_ms;   /* server still stale: hold local */
            } else {
                s_seek_guard_until = 0;                /* caught up or expired */
            }
        } else {
            s_seek_guard_until = 0;
        }
        s_track = *info;
        s_track.progress_ms = keep_progress;
        s_last_progress_tick = lv_tick_get();
        if (s_np_title)  lv_label_set_text(s_np_title, info->title);
        if (s_np_artist) lv_label_set_text(s_np_artist, info->artist);
        if (s_np_device) lv_label_set_text(s_np_device, info->device_name[0] ? info->device_name : "");

        /* 3C: auto-scroll the browser carousel to the playing album and accent
         * its border. Match by album URI (track.album.uri from /me/player).
         * Snap is safe even when the browser isn't visible -- it just updates
         * the scroller's internal position, ready for the next browser open. */
        if (info->album_uri[0]) {
            int new_idx = -1;
            for (size_t i = 0; i < s_card_count; i++) {
                const album_entry_t *a = albums_get(i);
                if (a && strcmp(a->uri, info->album_uri) == 0) {
                    new_idx = (int)i;
                    break;
                }
            }
            if (new_idx != s_playing_card_idx) {
                int old_idx = s_playing_card_idx;
                s_playing_card_idx = new_idx;
                if (s_card_pool) {
                    /* Frames are baked into the pool pixels (so they scale in
                     * Focus): re-bake just the two affected tiles. */
                    if (old_idx >= 0) rebake_card_tile((size_t)old_idx);
                    if (new_idx >= 0) rebake_card_tile((size_t)new_idx);
                } else {
                    /* Fallback (no pool): object border, full-size in Focus. */
                    for (size_t i = 0; i < s_card_count; i++) {
                        if (!s_cards[i]) continue;
                        if ((int)i == s_playing_card_idx) {
                            lv_obj_set_style_border_color(s_cards[i],
                                                          lv_color_hex(accent_color()), 0);
                            lv_obj_set_style_border_width(s_cards[i], 3, 0);
                        } else if (is_paper_theme()) {
                            lv_obj_set_style_border_color(s_cards[i],
                                                          lv_color_hex(s_th->text), 0);
                            lv_obj_set_style_border_width(s_cards[i], 2, 0);
                        } else {
                            lv_obj_set_style_border_width(s_cards[i], 0, 0);
                        }
                    }
                }
                if (new_idx >= 0 && s_browser_scroller) {
                    s_target_card = new_idx;
                    lv_obj_scroll_to_x(s_browser_scroller,
                                       (int32_t)new_idx * (cs() + cg()),
                                       LV_ANIM_ON);
                }
            }
        }
    }
    update_progress_bar();
    refresh_play_icon();
    /* Reflect the device's real level, but not while the user is dragging the
     * fader (the hold window) -- programmatic set doesn't fire VALUE_CHANGED. */
    if (info && info->volume_pct >= 0 && s_np_volume &&
        (int32_t)(lv_tick_get() - s_vol_hold_until) >= 0) {
        lv_slider_set_value(s_np_volume, info->volume_pct, LV_ANIM_OFF);
        if (is_glyph_theme()) vol_page_dots_update(info->volume_pct);
    }
    bsp_display_unlock();
}

void ui_art_refresh(const uint8_t *rgb_data, uint16_t w, uint16_t h)
{
    if (!s_art_dsc || !rgb_data || w == 0 || h == 0) return;
    if (bsp_display_lock(1000) != ESP_OK) {
        ESP_LOGW(TAG, "ui_art_refresh: display lock timeout, skipping");
        return;
    }

    /* Cache raw art pointer/dims for re-pixelation when switching into PIXEL
     * mid-session (apply_theme_cb calls pix_art_update directly). */
    s_last_raw_art = rgb_data;
    s_last_raw_w   = w;
    s_last_raw_h   = h;

    if (is_pixel_theme() && s_theme_art) {
        /* Allocate the 8 KB PSRAM art scratch on first use. */
        if (!s_pix_art_buf)
            s_pix_art_buf = heap_caps_malloc(
                PIX_ART_RES * PIX_ART_RES * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        if (s_pix_art_buf) {
            pixelate_rgb565((const uint16_t *)rgb_data, w, h,
                            s_pix_art_buf, PIX_ART_RES, PIX_ART_RES);
            s_pix_art_dsc.header.cf  = LV_COLOR_FORMAT_RGB565;
            s_pix_art_dsc.header.w   = PIX_ART_RES;
            s_pix_art_dsc.header.h   = PIX_ART_RES;
            s_pix_art_dsc.data       = (const uint8_t *)s_pix_art_buf;
            s_pix_art_dsc.data_size  = PIX_ART_RES * PIX_ART_RES * sizeof(uint16_t);
            if (s_np_art) {
                lv_image_set_src(s_np_art, &s_pix_art_dsc);
                lv_image_set_antialias(s_np_art, false);
                lv_obj_invalidate(s_np_art);
            }
        }
    } else if (is_paper_theme() && s_theme_art) {
        /* 128 KB PSRAM scratch, allocated on first use and kept (same policy
         * as the PIXEL buffer): dither at exactly ART_W so the halftone grain
         * lands 1:1 on the panel. */
        if (!s_paper_art_buf)
            s_paper_art_buf = heap_caps_malloc(
                PAPER_ART_RES * PAPER_ART_RES * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        if (s_paper_art_buf) {
            paperize_rgb565((const uint16_t *)rgb_data, w, h,
                            s_paper_art_buf, PAPER_ART_RES, PAPER_ART_RES);
            s_paper_art_dsc.header.cf  = LV_COLOR_FORMAT_RGB565;
            s_paper_art_dsc.header.w   = PAPER_ART_RES;
            s_paper_art_dsc.header.h   = PAPER_ART_RES;
            s_paper_art_dsc.data       = (const uint8_t *)s_paper_art_buf;
            s_paper_art_dsc.data_size  = PAPER_ART_RES * PAPER_ART_RES * sizeof(uint16_t);
            if (s_np_art) {
                lv_image_set_src(s_np_art, &s_paper_art_dsc);
                lv_image_set_antialias(s_np_art, false);
                lv_obj_invalidate(s_np_art);
            }
        }
    } else {
        s_art_dsc->header.cf  = LV_COLOR_FORMAT_RGB565;
        s_art_dsc->header.w   = w;
        s_art_dsc->header.h   = h;
        s_art_dsc->data       = rgb_data;
        s_art_dsc->data_size  = (uint32_t)w * h * 2;
        if (s_np_art) {
            lv_image_set_src(s_np_art, s_art_dsc);
            lv_image_set_antialias(s_np_art, true);
            lv_obj_invalidate(s_np_art);
        }
    }
    bsp_display_unlock();
}

void ui_set_devices(const ui_device_t *list, int count)
{
    if (count > MAX_DEVICES) count = MAX_DEVICES;
    if (count < 0)  count = 0;
    if (bsp_display_lock(1000) != ESP_OK) {
        ESP_LOGW(TAG, "ui_set_devices: display lock timeout, skipping");
        return;
    }
    s_dev_entry_count = 0;
    if (s_dev_list) {
        lv_obj_clean(s_dev_list);
        if (count == 0) {
            lv_obj_t *lbl = lv_label_create(s_dev_list);
            lv_label_set_text(lbl, "No devices found");
            lv_obj_set_style_text_color(lbl, lv_color_hex(s_th->text2), 0);
            lv_obj_set_style_text_font(lbl, font_md(), 0);
        }
        for (int i = 0; i < count; i++) {
            s_dev_entries[i] = list[i];

            lv_obj_t *row = lv_button_create(s_dev_list);
            lv_obj_set_size(row, lv_pct(100), 64);
            style_key_btn(row);
            lv_obj_set_style_bg_color(row, lv_color_hex(s_th->surface), 0);
            if (list[i].is_active) {
                /* PAPER keeps the full ruled frame and recolours it accent;
                 * other themes use the accent left-edge tab. */
                if (!is_paper_theme())
                    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
                lv_obj_set_style_border_width(row, is_paper_theme() ? 3 : 4, 0);
                lv_obj_set_style_border_color(row, lv_color_hex(accent_color()), 0);
            } else if (!is_paper_theme()) {
                lv_obj_set_style_border_width(row, 0, 0);
            }
            lv_obj_add_event_cb(row, on_device_tap, LV_EVENT_CLICKED,
                                (void *)(intptr_t)i);

            lv_obj_t *nm = lv_label_create(row);
            lv_label_set_text(nm, list[i].name);
            lv_obj_set_style_text_color(nm,
                lv_color_hex(list[i].is_active ? accent_color() : s_th->text), 0);
            lv_obj_set_style_text_font(nm, font_md(), 0);
            lv_obj_align(nm, LV_ALIGN_LEFT_MID, 12, -10);

            lv_obj_t *dt = lv_label_create(row);
            lv_label_set_text(dt, list[i].detail);
            lv_obj_set_style_text_color(dt, lv_color_hex(s_th->text2), 0);
            lv_obj_set_style_text_font(dt, font_sm(), 0);
            lv_obj_align(dt, LV_ALIGN_LEFT_MID, 12, 14);
        }
        s_dev_entry_count = count;
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

/* Hint-pill taps -- same destinations as the swipe gestures. */
static void on_hint_to_np(lv_event_t *e)      { (void)e; load_screen(s_screen_np, true); }
static void on_hint_to_browser(lv_event_t *e) { (void)e; load_screen(s_screen_browser, false); }

static void on_seek_start(lv_event_t *e)
{
    (void)e;
    s_seeking = true;
    /* PAPER: the ruler's ink block is the scrub indicator; keep the round
     * accent thumb hidden so the playhead stays a single printed mark. */
    if (s_seek_thumb && !is_paper_theme()) {
        int32_t pct = (s_track.duration_ms > 0)
            ? (int32_t)((uint64_t)s_track.progress_ms * 1000 / s_track.duration_ms) : 0;
        position_seek_thumb(pct);
        lv_obj_remove_flag(s_seek_thumb, LV_OBJ_FLAG_HIDDEN);
    }
}
static void on_seek_click_absorb(lv_event_t *e) { lv_event_stop_bubbling(e); }

/* Tap the right-hand timecode to flip remaining <-> total (Spotify-style). */
static void on_remain_tap(lv_event_t *e)
{
    lv_event_stop_bubbling(e);   /* don't reach the screen play/pause handler */
    s_remain_show_total = !s_remain_show_total;
    audio_play(AUDIO_SFX_TICK);
    update_progress_bar();       /* repaint the label immediately */
}

/* Arm the seek guard so the next few polls don't snap the bar back to the
 * stale server position. Call right after issuing a seek for `target_ms`. */
static void arm_seek_guard(uint32_t target_ms)
{
    s_seek_anchor_ms   = target_ms;
    s_seek_anchor_tick = lv_tick_get();
    s_seek_guard_until = s_seek_anchor_tick + SEEK_GUARD_MAX_MS;
}

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
    position_seek_thumb(pct);
    update_progress_bar();   /* refresh timestamps; bar value is frozen while seeking */
}

static void on_seek_released(lv_event_t *e)
{
    (void)e;
    if (!s_seeking) return;          /* RELEASED + PRESS_LOST can both fire */
    s_seeking = false;
    if (s_seek_thumb) lv_obj_add_flag(s_seek_thumb, LV_OBJ_FLAG_HIDDEN);
    if (s_track.duration_ms == 0) return;
    /* progress_ms was kept in sync by on_seek_pressing; send the final value. */
    ui_request_seek(s_track.progress_ms);
    arm_seek_guard(s_track.progress_ms);
}

static void format_mmss(char *buf, size_t n, uint32_t ms)
{
    uint32_t total = ms / 1000;
    snprintf(buf, n, "%u:%02u", (unsigned)(total / 60), (unsigned)(total % 60));
}

static void position_seek_thumb(int32_t pct)
{
    if (!s_seek_thumb) return;
    if (pct < 0)    pct = 0;
    if (pct > 1000) pct = 1000;
    int32_t cx = PROG_X + (int32_t)((int64_t)pct * PROG_W / 1000);
    lv_obj_set_pos(s_seek_thumb, cx - THUMB_W / 2, PROG_Y + PROG_H / 2 - THUMB_H / 2);
}

static void refresh_play_icon(void)
{
    if (s_np_play_lbl)
        lv_label_set_text(s_np_play_lbl,
                          s_track.is_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

/* Spotify-style previous: restart the track if more than 3 s in, else go to the
 * previous track. Shared by the prev key and the swipe-right gesture. */
static void prev_or_restart(void)
{
    if (s_track.progress_ms > 3000) {
        ui_request_seek(0);
        s_track.progress_ms = 0;
        arm_seek_guard(0);
        update_progress_bar();
    } else {
        ui_request_prev();
    }
}

static void on_transport_prev(lv_event_t *e)   { (void)e; prev_or_restart(); }
static void on_transport_toggle(lv_event_t *e) { (void)e; ui_request_toggle_play(); }
static void on_transport_next(lv_event_t *e)   { (void)e; ui_request_next(); }

static void on_vol_press(lv_event_t *e)
{
    (void)e;
    s_vol_dragging = true;
}

static void on_vol_press_lost(lv_event_t *e)
{
    (void)e;
    s_vol_dragging = false;
}

static void on_vol_changed(lv_event_t *e)
{
    (void)e;
    if (!s_np_volume) return;
    s_vol_dragging = true;  /* ensure flag stays set during dragging */
    vol_hud_show(lv_slider_get_value(s_np_volume), false);  /* live feedback */
    s_vol_hold_until = lv_tick_get() + 4000;   /* keep polls from snapping it back */
}

static void on_vol_released(lv_event_t *e)
{
    (void)e;
    s_vol_dragging = false;
    if (!s_np_volume) return;
    ui_request_volume(lv_slider_get_value(s_np_volume));          /* apply once, on release */
    s_vol_hold_until = lv_tick_get() + 4000;
}

static void on_gesture(lv_event_t *e)
{
    /* A drag that began on the progress bar or the volume slider must not be
     * reclassified as a screen swipe. */
    if (s_seeking || s_vol_dragging) return;
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
        prev_or_restart();
    }
    (void)e;
}

/* How close to the screen centre (px) a tap must land to count as "play the
 * centred album" rather than "scroll to a side cover". Sized to the centre
 * cover's half-width so taps anywhere on it play; taps clearly on a side cover
 * scroll. */
#define CENTRE_TAP_TOL  140

static void on_card_clicked(lv_event_t *e)
{
    size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);

    /* Decide from the touch X, NOT which hidden card object was hit. In Cover
     * Flow the fanned covers don't line up with the (linear) clickable card
     * slots, so a tap on the big centre cover can land on a neighbour's hitbox
     * and scroll instead of play. A tap near the screen centre plays the
     * centred album; a tap clearly to one side scrolls that card in. */
    int  centred = (s_centered_card >= 0) ? s_centered_card : (int)idx;
    bool play    = ((int)idx == centred);   /* fallback if no pointer info */

    lv_indev_t *indev = lv_indev_active();
    if (indev) {
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        int dx = (int)p.x - SCREEN_W / 2;
        if (dx < 0) dx = -dx;
        play = (dx <= CENTRE_TAP_TOL);
    }

    if (!play) {
        s_target_card = (int)idx;
        lv_obj_scroll_to_x(s_browser_scroller,
                           (int32_t)idx * (cs() + cg()), LV_ANIM_ON);
        return;
    }

    const album_entry_t *a = albums_get((size_t)centred);
    if (!a) return;
    ESP_LOGI(TAG, "play album: %s -- %s", a->artist, a->title);
    audio_play(AUDIO_SFX_SELECT);
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
    audio_play(AUDIO_SFX_TICK);   /* detent tick as the centred album changes */
    if (s_br_index_lbl) {
        char ib[20];
        snprintf(ib, sizeof ib, "%02d / %02d", idx + 1, (int)s_card_count);
        lv_label_set_text(s_br_index_lbl, ib);
    }
    const album_entry_t *a = albums_get((size_t)idx);
    if (!a) return;
    if (s_browser_title)  lv_label_set_text(s_browser_title, a->title);
    if (s_browser_artist) lv_label_set_text(s_browser_artist, a->artist);
}

static void update_progress_bar(void)
{
    if (!s_np_progress) return;
    uint32_t p = s_track.progress_ms;
    if (s_track.duration_ms > 0 && p > s_track.duration_ms) p = s_track.duration_ms;
    int32_t pct = (s_track.duration_ms > 0)
                ? (int32_t)((uint64_t)p * 1000 / s_track.duration_ms) : 0;

    /* Timestamps track progress even mid-scrub (on_seek_pressing keeps
     * progress_ms current); only the bar value is frozen during a drag. */
    if (s_np_elapsed) {
        char b[12];
        format_mmss(b, sizeof b, p);
        lv_label_set_text(s_np_elapsed, b);
    }
    if (s_np_remain) {
        char t[12], b[14];
        if (s_remain_show_total) {
            format_mmss(t, sizeof t, s_track.duration_ms);
            snprintf(b, sizeof b, "%s", t);            /* total track length */
        } else {
            uint32_t rem = (s_track.duration_ms > p) ? (s_track.duration_ms - p) : 0;
            format_mmss(t, sizeof t, rem);
            snprintf(b, sizeof b, "-%s", t);           /* time remaining */
        }
        lv_label_set_text(s_np_remain, b);
    }

    /* PAPER: ride the ink block along the ruler. While scrubbing, the bar holds
     * the drag value (on_seek_pressing wrote it just before calling here), so
     * the block tracks the finger; otherwise it tracks playback. */
    if (s_paper_cursor) {
        int32_t cpct = s_seeking ? lv_bar_get_value(s_np_progress) : pct;
        lv_obj_set_pos(s_paper_cursor,
                       PROG_X + (int32_t)((int64_t)cpct * PROG_W / 1000) - PAPER_CUR_W / 2,
                       PROG_Y + PROG_H / 2 - PAPER_CUR_H / 2);
    }

    if (s_seeking) return;
    lv_bar_set_value(s_np_progress, pct, LV_ANIM_OFF);
}

static void progress_timer_cb(lv_timer_t *t)
{
    (void)t;
    /* Always advance the tick reference so a long pause (seeking / not playing)
     * doesn't cause a sudden jump when playback resumes. */
    uint32_t now = lv_tick_get();
    uint32_t delta = now - s_last_progress_tick;
    s_last_progress_tick = now;
    if (s_seeking || !s_track.is_playing || s_track.duration_ms == 0) return;
    /* Advance using wall-clock delta so the bar doesn't drift under UI load.
     * The next Spotify poll overwrites this with the server's authoritative value. */
    s_track.progress_ms += delta;
    if (s_track.progress_ms > s_track.duration_ms) {
        s_track.progress_ms = s_track.duration_ms;
    }
    update_progress_bar();
}

/* Apply per-card transforms based on distance from the viewport centre.
 *
 *  - FOCUS:     scale + dim the child lv_image (safe image-draw path; no layer).
 *               IMPORTANT: transforms are on lv_image, NOT the card object.
 *               Object-level transform_scale/opa forces a per-frame snapshot
 *               that this board's DIRECT-mode rotated DSI flush mis-composited.
 *  - COVERFLOW: delegates entirely to cf_render() — LVGL card images are hidden;
 *               the PSRAM column rasteriser draws the perspective trapezoids.
 */
static void apply_card_transforms(void)
{
    if (!BROWSER_STYLE_TRANSFORMS(s_browser_style) || !s_browser_scroller) return;

    if (s_browser_style == BROWSER_COVERFLOW) {
        /* CF uses its own PSRAM rasteriser; LVGL card images are hidden. */
        cf_render();
        return;
    }

    /* Focus mode: scale each card image by distance from centre, dim gently. */
    int32_t scroll_x   = lv_obj_get_scroll_left(s_browser_scroller);
    int32_t pad_left   = (SCREEN_W - cs()) / 2;
    int32_t step       = cs() + cg();
    int32_t scr_center = SCREEN_W / 2;
    for (size_t i = 0; i < s_card_count; i++) {
        if (!s_card_imgs[i] || s_card_dscs[i].header.w == 0) continue;
        int32_t card_cx = pad_left + (int32_t)i * step + cs() / 2 - scroll_x;
        int32_t dist = card_cx - scr_center;
        if (dist < 0) dist = -dist;

        /* Base scale fills the cs() (286px) card from the dsc's source pixels:
         * the 286px card pool normally (base == LV_SCALE_NONE, so the centred
         * card is an untransformed 1:1 blit), or the 220px / 64px fallback
         * sources when the pool is absent. Neighbours scale down from there. */
        uint32_t base = (uint32_t)cs() * LV_SCALE_NONE / s_card_dscs[i].header.w;
        int32_t scale = LV_SCALE_NONE - dist * 76 / step;
        if (scale < 150) scale = 150;
        int32_t dim = dist * 95 / step;
        if (dim > 110) dim = 110;

        /* Skip the LVGL writes (each one invalidates the card's region) when
         * the computed values match what was last applied -- true for every
         * card outside the few near the centre, where both values saturate. */
        if ((int16_t)scale != s_card_scale_last[i]) {
            s_card_scale_last[i] = (int16_t)scale;
            lv_image_set_scale(s_card_imgs[i], (uint32_t)((int64_t)scale * base / LV_SCALE_NONE));
        }
        if ((int16_t)dim != s_card_dim_last[i]) {
            s_card_dim_last[i] = (int16_t)dim;
            lv_obj_set_style_image_recolor(s_card_imgs[i], lv_color_black(), 0);
            lv_obj_set_style_image_recolor_opa(s_card_imgs[i], (lv_opa_t)dim, 0);
        }
    }
}

static void refresh_browser_style_selection(void)
{
    for (int i = 0; i < BROWSER_STYLE_COUNT; i++) {
        if (!s_brstyle_btns[i] || !s_brstyle_labels[i]) continue;
        bool sel = (i == (int)s_browser_style);
        lv_obj_set_style_bg_color(s_brstyle_btns[i],
            sel ? opt_sel_bg() : lv_color_hex(s_th->surface), 0);
        lv_obj_set_style_text_color(s_brstyle_labels[i],
            sel ? opt_sel_fg() : lv_color_hex(s_th->text2), 0);
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
    audio_play(AUDIO_SFX_SELECT);
    ESP_LOGI(TAG, "browser style -> %s", k_browser_style_names[idx]);
    /* Rebuild the browser screen in the background -- user stays in settings. */
    lv_async_call(rebuild_browser_cb, NULL);
}

static void refresh_line_selection(void)
{
    if (!s_line_toggle_btn || !s_line_toggle_lbl) return;
    lv_obj_set_style_bg_color(s_line_toggle_btn,
        s_show_sel_line ? opt_sel_bg() : lv_color_hex(s_th->surface), 0);
    lv_obj_set_style_text_color(s_line_toggle_lbl,
        s_show_sel_line ? opt_sel_fg() : lv_color_hex(s_th->text2), 0);
    lv_label_set_text(s_line_toggle_lbl, s_show_sel_line ? "ON" : "OFF");
}

static void on_line_toggle(lv_event_t *e)
{
    (void)e;
    s_show_sel_line = !s_show_sel_line;
    save_sel_line(s_show_sel_line ? 1 : 0);
    /* No rebuild needed -- the browser screen still exists; just toggle the
     * underline's visibility directly. */
    if (s_sel_line) {
        if (s_show_sel_line) lv_obj_remove_flag(s_sel_line, LV_OBJ_FLAG_HIDDEN);
        else                 lv_obj_add_flag(s_sel_line, LV_OBJ_FLAG_HIDDEN);
    }
    refresh_line_selection();
    audio_play(AUDIO_SFX_TICK);
    ESP_LOGI(TAG, "selection line -> %s", s_show_sel_line ? "ON" : "OFF");
}

static void on_brightness_changed(lv_event_t *e)
{
    (void)e;
    if (!s_brightness_slider) return;
    s_brightness = (uint8_t)lv_slider_get_value(s_brightness_slider);
    bsp_display_brightness_set(s_brightness);   /* live dim while dragging */
    if (s_brightness_val) {
        char b[8];
        snprintf(b, sizeof b, "%d%%", s_brightness);
        lv_label_set_text(s_brightness_val, b);
    }
}

static void on_brightness_released(lv_event_t *e)
{
    (void)e;
    save_brightness(s_brightness);              /* persist once, on release */
    ESP_LOGI(TAG, "brightness -> %d%%", s_brightness);
}

static void refresh_font_selection(void)
{
    for (int i = 0; i < 2; i++) {
        if (!s_font_btns[i]) continue;
        bool sel = ((uint8_t)i == s_font_choice);
        lv_obj_set_style_bg_color(s_font_btns[i],
            sel ? opt_sel_bg() : lv_color_hex(s_th->surface), 0);
        lv_obj_set_style_bg_opa(s_font_btns[i], LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(s_font_labels[i],
            sel ? lv_color_white() : lv_color_hex(s_th->text), 0);
    }
}

static void on_font_option(lv_event_t *e)
{
    uint8_t idx = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    if (idx == s_font_choice) return;
    s_font_choice = idx;
    save_font(idx);
    refresh_font_selection();
    audio_play(AUDIO_SFX_SELECT);
    /* Trigger a full theme rebuild so all screens pick up the new font. */
    lv_async_call(apply_theme_cb, NULL);
    ESP_LOGI(TAG, "font -> %s", idx == FONT_SLAB ? "SLAB (Arvo)" : "SANS (Montserrat)");
}

static void refresh_fps_selection(void)
{
    if (!s_fps_toggle_btn || !s_fps_toggle_lbl) return;
    lv_obj_set_style_bg_color(s_fps_toggle_btn,
        s_fps_enabled ? opt_sel_bg() : lv_color_hex(s_th->surface), 0);
    lv_obj_set_style_text_color(s_fps_toggle_lbl,
        s_fps_enabled ? opt_sel_fg() : lv_color_hex(s_th->text2), 0);
    lv_label_set_text(s_fps_toggle_lbl, s_fps_enabled ? "ON" : "OFF");
}

static void on_fps_toggle(lv_event_t *e)
{
    (void)e;
    s_fps_enabled = !s_fps_enabled;
    save_fps(s_fps_enabled ? 1 : 0);
    if (s_fps_label) {
        if (s_fps_enabled) lv_obj_remove_flag(s_fps_label, LV_OBJ_FLAG_HIDDEN);
        else               lv_obj_add_flag(s_fps_label, LV_OBJ_FLAG_HIDDEN);
    }
    refresh_fps_selection();
    audio_play(AUDIO_SFX_TICK);
    ESP_LOGI(TAG, "fps display -> %s", s_fps_enabled ? "ON" : "OFF");
}

static void refresh_sound_selection(void)
{
    if (!s_sound_toggle_btn || !s_sound_toggle_lbl) return;
    bool on = audio_is_enabled();
    lv_obj_set_style_bg_color(s_sound_toggle_btn,
        on ? opt_sel_bg() : lv_color_hex(s_th->surface), 0);
    lv_obj_set_style_text_color(s_sound_toggle_lbl,
        on ? opt_sel_fg() : lv_color_hex(s_th->text2), 0);
    lv_label_set_text(s_sound_toggle_lbl, on ? "ON" : "OFF");
}

static void on_sound_toggle(lv_event_t *e)
{
    (void)e;
    bool on = !audio_is_enabled();
    audio_set_enabled(on);
    save_sound(on ? 1 : 0);
    refresh_sound_selection();
    if (on) audio_play(AUDIO_SFX_SELECT);   /* audible confirmation when enabling */
    ESP_LOGI(TAG, "ui sound -> %s", on ? "ON" : "OFF");
}

static void on_volume_changed(lv_event_t *e)
{
    (void)e;
    if (!s_volume_slider) return;
    int v = lv_slider_get_value(s_volume_slider);
    audio_set_volume(v);                        /* live -- next blip uses new gain */
    if (s_volume_val) {
        char b[8];
        snprintf(b, sizeof b, "%d%%", v);
        lv_label_set_text(s_volume_val, b);
    }
}

static void on_volume_released(lv_event_t *e)
{
    (void)e;
    int v = audio_get_volume();
    save_volume((uint8_t)v);                    /* persist once, on release */
    audio_play(AUDIO_SFX_TICK);                 /* preview the new level */
    ESP_LOGI(TAG, "ui volume -> %d%%", v);
}

static void refresh_settings_tabs(void)
{
    for (int i = 0; i < SET_TAB_COUNT; i++) {
        if (!s_set_tabs[i] || !s_set_tab_lbls[i]) continue;
        bool sel = (i == (int)s_set_tab);
        lv_obj_set_style_bg_color(s_set_tabs[i],
            sel ? opt_sel_bg() : lv_color_hex(s_th->surface), 0);
        lv_obj_set_style_text_color(s_set_tab_lbls[i],
            sel ? opt_sel_fg() : lv_color_hex(s_th->text2), 0);
    }
}

static void on_settings_tab(lv_event_t *e)
{
    uint8_t idx = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    if (idx >= SET_TAB_COUNT || idx == s_set_tab) return;
    s_set_tab = idx;
    for (int i = 0; i < SET_TAB_COUNT; i++) {
        if (!s_set_pages[i]) continue;
        if (i == (int)idx) lv_obj_remove_flag(s_set_pages[i], LV_OBJ_FLAG_HIDDEN);
        else               lv_obj_add_flag(s_set_pages[i], LV_OBJ_FLAG_HIDDEN);
    }
    refresh_settings_tabs();
    audio_play(AUDIO_SFX_BACK);   /* soft page-change blip */
}

static void save_sound_set(int8_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_SETTINGS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY_SOUND_SET, (uint8_t)(v < 0 ? 0 : v + 1));   /* 0 = AUTO */
    nvs_commit(h);
    nvs_close(h);
}

static void refresh_sound_set_selection(void)
{
    int sel     = audio_get_set();               /* -1 = AUTO */
    int sel_opt = (sel < 0) ? 0 : sel + 1;       /* option 0 = AUTO */
    for (int i = 0; i < s_sndset_opt_count; i++) {
        if (!s_sndset_btns[i] || !s_sndset_lbls[i]) continue;
        bool on = (i == sel_opt);
        lv_obj_set_style_bg_color(s_sndset_btns[i],
            on ? opt_sel_bg() : lv_color_hex(s_th->surface), 0);
        lv_obj_set_style_text_color(s_sndset_lbls[i],
            on ? opt_sel_fg() : lv_color_hex(s_th->text2), 0);
    }
}

static void on_sound_set_option(lv_event_t *e)
{
    uint8_t opt = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    int set = (opt == 0) ? -1 : (int)opt - 1;
    audio_set_set(set);
    save_sound_set((int8_t)set);
    refresh_sound_set_selection();
    audio_play(AUDIO_SFX_SELECT);   /* preview the chosen design */
    ESP_LOGI(TAG, "sound set -> %s", (opt == 0) ? "AUTO" : audio_set_name(set));
}

/* Presented-frame accounting for the FPS readout. Fires on the LVGL task after
 * each refresh; we only read the clock -- NO lv_*_invalidate() here (that
 * asserts during rendering and wedges the task). */
static void fps_render_ready_cb(lv_event_t *e)
{
    (void)e;
    int64_t now = esp_timer_get_time();
    if (s_fps_prev_ready_us != 0 &&
        now - s_fps_prev_ready_us < FPS_BURST_GAP_US) {
        s_fps_burst_frames++;
    } else {
        /* Gap: close the previous burst into the window accumulators. A burst
         * of N frames contributes N-1 intervals over (last - first) time. */
        if (s_fps_burst_frames >= 2) {
            s_fps_acc_intervals += s_fps_burst_frames - 1;
            s_fps_acc_span_us   += (uint32_t)(s_fps_prev_ready_us - s_fps_burst_start_us);
        }
        s_fps_burst_start_us = now;
        s_fps_burst_frames   = 1;
    }
    s_fps_prev_ready_us = now;
}

/* 1-second tick: fold the running burst into the window, compute the achieved
 * rate, and update the browser label. */
static void fps_timer_cb(lv_timer_t *t)
{
    (void)t;
    /* Fold the still-running burst into this window, then re-anchor it at its
     * last frame so a burst spanning windows keeps counting seamlessly. */
    if (s_fps_burst_frames >= 2) {
        s_fps_acc_intervals += s_fps_burst_frames - 1;
        s_fps_acc_span_us   += (uint32_t)(s_fps_prev_ready_us - s_fps_burst_start_us);
        s_fps_burst_start_us = s_fps_prev_ready_us;
        s_fps_burst_frames   = 1;
    }
    /* Need a few intervals to call it a rate -- isolated one-off redraws
     * (this label update, a toast) hold the previous reading instead. */
    if (s_fps_acc_intervals >= 3 && s_fps_acc_span_us > 0) {
        uint32_t fps = (uint32_t)(((uint64_t)s_fps_acc_intervals * 1000000u)
                                  / s_fps_acc_span_us);
        if (fps > 999) fps = 999;
        s_fps_last_rate = fps;
    }
    s_fps_acc_intervals = 0;
    s_fps_acc_span_us   = 0;
    if (!s_fps_enabled || !s_fps_label) return;
    char buf[12];
    snprintf(buf, sizeof buf, "%lu FPS", (unsigned long)s_fps_last_rate);
    lv_label_set_text(s_fps_label, buf);
}

static void rebuild_browser_cb(void *unused)
{
    (void)unused;
    int     saved_card  = s_centered_card;
    lv_obj_t *old_browser = s_screen_browser;

    /* Free any existing pixel thumbnail pool; build_browser_screen() will
     * reallocate it if PIXEL is still active. */
    if (s_pix_thumbs) { heap_caps_free(s_pix_thumbs); s_pix_thumbs = NULL; }
    /* Free the CF canvas PSRAM buffer; the lv_image widget is a child of
     * old_browser and is freed when that screen is deleted below. */
    cf_deinit();
    /* The WiFi dot meter lives on the browser screen; it must be torn down and
     * rebuilt on the new screen, else s_wifi_dots dangles into the freed old
     * screen and the WiFi timer writes to freed objects (crash). */
    wifi_dots_stop();
    build_browser_screen();
    if (is_glyph_theme()) wifi_dots_start(s_screen_browser);

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

    if (is_glyph_theme()) {
        /* Glyph: update the dot strength meter instead of the bar indicators. */
        wifi_dots_update_count(bars);
    } else {
        for (int i = 0; i < 4; i++) {
            if (!s_wifi_bars[i]) continue;
            lv_color_t c = (i < bars)
                ? lv_color_hex(s_th->text)
                : lv_color_hex(s_th->track);
            lv_obj_set_style_bg_color(s_wifi_bars[i], c, 0);
        }
    }

    /* Don't lie to the user with a stale track when WiFi is gone. On the
     * transition into offline, replace the title with "OFFLINE"; on the way
     * back, restore from the cached track (next successful poll overwrites it). */
    bool now_offline = (bars == 0);
    if (now_offline != s_offline) {
        s_offline = now_offline;
        if (s_np_title) {
            if (is_glyph_theme()) {
                /* Glyph: animated dissolve/reform instead of instant label swap. */
                if (s_offline) title_dissolve();
                else           title_reform();
            } else {
                if (s_offline) lv_label_set_text(s_np_title, "OFFLINE");
                else           lv_label_set_text(s_np_title, s_track.title);
            }
        }
    }
}

bool ui_is_now_playing(void)
{
    /* Conservative on lock timeout: report "not on now-playing" rather than
     * stall the caller -- a wrong false only re-routes a context-dependent
     * control to its browser action. */
    if (bsp_display_lock(1000) != ESP_OK) {
        ESP_LOGW(TAG, "ui_is_now_playing: display lock timeout");
        return false;
    }
    bool on_np = (lv_screen_active() == s_screen_np);
    bsp_display_unlock();
    return on_np;
}

void ui_toggle_view(void)
{
    if (bsp_display_lock(1000) != ESP_OK) {
        ESP_LOGW(TAG, "ui_toggle_view: display lock timeout, skipping");
        return;
    }
    lv_obj_t *active = lv_screen_active();
    if (active == s_screen_browser) {
        load_screen(s_screen_np, true);
    } else {
        load_screen(s_screen_browser, false);
    }
    bsp_display_unlock();
}

void ui_play_centered_album(void)
{
    if (bsp_display_lock(1000) != ESP_OK) {
        ESP_LOGW(TAG, "ui_play_centered_album: display lock timeout, skipping");
        return;
    }
    int idx = find_centered_card();
    const album_entry_t *a = (idx >= 0) ? albums_get((size_t)idx) : NULL;
    if (a) {
        ESP_LOGI(TAG, "play album (encoder): %s -- %s", a->artist, a->title);
        audio_play(AUDIO_SFX_SELECT);
        ui_request_play(a->uri);
        load_screen(s_screen_np, true);
    }
    bsp_display_unlock();
}

void ui_scroll_browser(int32_t delta)
{
    if (bsp_display_lock(1000) != ESP_OK) {
        ESP_LOGW(TAG, "ui_scroll_browser: display lock timeout, skipping");
        return;
    }
    if (s_browser_scroller && delta != 0 && s_card_count != 0) {
        int t = s_target_card + (int)delta;
        if (t < 0) t = 0;
        if (t >= (int)s_card_count) t = (int)s_card_count - 1;
        s_target_card = t;
        if (s_cards[t]) {
            /* Scroll straight to the target card's layout slot (t*step),
             * matching the snap convention in rebuild_browser_cb/
             * find_centered_card: card i is centred when scroll_left == i*step. */
            lv_obj_scroll_to_x(s_browser_scroller, (int32_t)t * (cs() + cg()),
                               LV_ANIM_ON);
        }
    }
    bsp_display_unlock();
}

uint32_t ui_get_progress_ms(void)
{
    /* On lock timeout fall back to a direct read: progress_ms is an aligned
     * 32-bit field so the read is atomic; a momentarily stale value beats
     * returning 0 (which would snap a caller's seek/progress view to zero). */
    if (bsp_display_lock(1000) != ESP_OK) {
        return s_track.progress_ms;
    }
    uint32_t ms = s_track.progress_ms;
    bsp_display_unlock();
    return ms;
}

static void vol_hud_hide_cb(lv_timer_t *t)
{
    (void)t;
    if (s_vol_hud) lv_obj_add_flag(s_vol_hud, LV_OBJ_FLAG_HIDDEN);
    s_vol_hud_timer = NULL;
}

static void toast_hide_cb(lv_timer_t *t)
{
    (void)t;
    if (s_toast) lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    s_toast_timer = NULL;
}

static void idle_timer_cb(lv_timer_t *t)
{
    (void)t;
    /* lv_display_get_inactive_time() resets to 0 on any touch / button event,
     * so this also serves as the "wake" trigger -- next tick after touch,
     * inactive < AUTO_DIM_AFTER_MS and we restore s_brightness. */
    uint32_t inactive = lv_display_get_inactive_time(NULL);
    uint8_t want = 0;
    if      (inactive >= AUTO_DIM_DEEP_AFTER_MS) want = 2;
    else if (inactive >= AUTO_DIM_AFTER_MS)      want = 1;

    if (want == s_dim_state) return;   /* no transition, skip the LEDC write */
    s_dim_state = want;

    int level = s_brightness;
    if      (want == 1) level = (s_brightness * 30) / 100;
    else if (want == 2) level = (s_brightness * 10) / 100;
    if (level < 2) level = 2;   /* keep a faint glow so the device doesn't look dead */
    bsp_display_brightness_set(level);
}

void ui_show_toast(const char *msg, uint32_t ms_dur)
{
    if (!msg) return;
    /* Called from spotify_task (no LVGL lock held), so we must take it. */
    if (bsp_display_lock(1000) != ESP_OK) {
        ESP_LOGW(TAG, "ui_show_toast: display lock timeout, skipping");
        return;
    }
    if (s_toast) {
        lv_label_set_text(s_toast, msg);
        lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
        if (s_toast_timer) {
            lv_timer_reset(s_toast_timer);
        } else {
            s_toast_timer = lv_timer_create(toast_hide_cb, ms_dur, NULL);
            lv_timer_set_repeat_count(s_toast_timer, 1);
        }
    }
    bsp_display_unlock();
}

/* In-context worker: caller must hold the LVGL lock (the slider handler runs
 * in the LVGL task). Other tasks go through ui_show_volume_hud below. */
static void vol_hud_show(int pct, bool muted)
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

void ui_show_volume_hud(int pct, bool muted)
{
    if (bsp_display_lock(1000) != ESP_OK) {
        ESP_LOGW(TAG, "ui_show_volume_hud: display lock timeout, skipping");
        return;
    }
    vol_hud_show(pct, muted);
    bsp_display_unlock();
}
