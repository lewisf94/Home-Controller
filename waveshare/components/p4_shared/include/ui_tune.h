/*
 * ui_tune.h -- USER-TWEAKABLE layout + colour knobs for the Waveshare UI.
 *
 * Everything here is safe to edit by eye: change a number, rebuild + flash
 * (idf.py build flash), look at the result. Nothing in ui.c needs touching.
 *
 * Most knobs are per-MODE arrays in this order:
 *
 *        { BASIC, GLYPH, PIXEL, PAPER, BOLD }
 *
 * so each theme can have its own spacing without affecting the others. BOLD
 * reuses BASIC's geometry throughout (it's a font/palette/radius redesign of
 * BASIC, not a new layout) except TUNE_TITLE_LETTER_SP, where it adds a touch
 * of tracking for the geometric-poster feel.
 * Y values are pixels from the top of the 800x480 landscape panel; X from the
 * left. The album strip is TUNE_SCROLLER_Y tall SCROLLER_H (292) px in every
 * browser style -- BASIC/GLYPH/PIXEL start at y30 (bottom y322); PAPER starts
 * at y54 to clear its header rule (bottom y346).
 */
#pragma once

/* =========================================================================
 * ACCENT COLOURS -- the COLOUR grid in Settings (8 hues x 3 variants).
 * Order: rows top->bottom = VIVID / DEEP / SOFT; columns left->right walk the
 * hue wheel: ORANGE / AMBER / GREEN / TEAL / BLUE / PURPLE / MAGENTA / RED.
 * 0xRRGGBB. The DEEP row (the favourite) is rich + darkened (~33% lum, high
 * sat); VIVID is pure/bright; SOFT is pastel. Tweak hue/sat/lum freely here --
 * the selected swatch INDEX is what's saved, so reordering changes what an
 * existing save points at.
 * ========================================================================= */
#define TUNE_ACCENTS { \
    /* vivid */ 0xFF4E00, 0xFFAA00, 0x00B355, 0x00D6C4, 0x1A79FF, 0x8B5CF6, 0xFF1A9F, 0xE62E21, \
    /* deep  */ 0xC23B00, 0xB27700, 0x00753A, 0x008F83, 0x0F58BD, 0x5B3FA8, 0xC11579, 0xA61E14, \
    /* soft  */ 0xFF8A4D, 0xF3C568, 0x4DC98A, 0x64D8CF, 0x679EE9, 0xA98BE0, 0xE774B7, 0xE8675C, \
}
#define TUNE_ACCENT_COUNT 24
/* Number of hue columns in the grid (rows = TUNE_ACCENT_COUNT / this). */
#define TUNE_ACCENT_COLS  8

/* =========================================================================
 * LIVE KNOBS -- every macro from here to the end of the file is ALSO
 * adjustable on the device under Settings > DEVELOPER (grouped into the TYPE /
 * SHAPE / LAYOUT / BROWSER / ART sub-pages). On-device edits are stored as NVS
 * overrides for the mode you edited; the values here stay the compiled
 * defaults and are what a RESET returns to.
 *
 * ROUND TRIP: tune on device -> tap EXPORT -> the serial monitor prints these
 * exact #define lines with your values -> paste them back here -> RESET MODE on
 * the device so the compiled defaults are live again -> commit. The printed
 * format is byte-identical to the lines below, so paste-over is safe.
 *
 * FORMATTING: the columns are 6 wide because the same formatter emits both
 * these lines and the device's EXPORT. Keep that width if you hand-edit, or
 * the round trip stops being a clean diff.
 *
 * COMPILE-TIME BOUNDS: TUNE_PROG_PARTS, TUNE_CF_MAX_SIDE and TUNE_CF_SCALE are
 * capped by arrays sized at build time (PROG_PART_COUNT, CF_CARDS_MAX,
 * CF_COL_MAX in ui.c). Their on-device sliders only ever REDUCE work; raising
 * them past the compiled bound needs a rebuild, because the scratch lives in
 * internal SRAM, which is the scarce resource on this board.
 * ========================================================================= */
/* Corner radius (px) of every boxy button/key. -1 means a full pill
 * (LV_RADIUS_CIRCLE). GLYPH's pill and PAPER's hard square are part of those
 * themes' identities -- BOLD's 14 is its rounded-geometric signature. */
#define TUNE_KEY_RADIUS        {     3,    -1,     3,     0,    14 }
/* Corner radius (px) of album cards + the now-playing cover. 0 = hard square
 * (every theme's original look); BOLD softens them to match its buttons. */
#define TUNE_ART_RADIUS        {     0,     0,     0,     0,     7 }
/* Progress-bar thickness (px). BOLD's chunky bar is a deliberate flat block
 * rather than the hairline the other themes use. */
#define TUNE_PROG_H            {     6,     6,     6,     6,    12 }
/* Selection underline under the centred album: width then thickness (px). */
#define TUNE_SEL_W             {    88,    88,    88,    88,   120 }
#define TUNE_SEL_H             {     3,     3,     3,     3,     8 }
/* 1 = transport keys (prev/play/next) are drawn as true circles regardless of
 * TUNE_KEY_RADIUS. Futura's defining shape is the perfect circle, so BOLD
 * takes it; the others keep their square-ish keys. */
#define TUNE_TKEY_CIRCLE       {     0,     0,     0,     0,     1 }
/* 1 = force album/track TITLES to uppercase (artist stays as supplied -- the
 * caps/mixed pairing is the point). LVGL has no text-transform, so ui.c
 * upcases the string itself; ASCII a-z only, so accented metadata survives. */
#define TUNE_TITLE_UPPER       {     0,     0,     0,     0,     1 }

/* ---- TYPE ---------------------------------------------------------------
 * Letter spacing is per ROLE, so a tracked title can sit over an untracked
 * artist line. Uppercase is likewise per role. */
#define TUNE_ARTIST_LSP        {     0,     0,     0,     0,     1 }
#define TUNE_HEADER_LSP        {     2,     2,     2,     2,     3 }
#define TUNE_ARTIST_UPPER      {     0,     0,     0,     0,     0 }
/* Extra px between wrapped lines (LVGL text_line_space). */
#define TUNE_LINE_SPACE        {     0,     0,     0,     0,     0 }
/* Marquee: full scroll cycle in ms, and 1 = only scroll when the text is
 * actually too wide (0 = always scroll, LVGL's default circular behaviour). */
#define TUNE_MARQUEE_MS        { 30000, 30000, 30000, 30000, 30000 }
#define TUNE_MARQUEE_FIT_ONLY  {     1,     1,     1,     1,     1 }
/* Title block alignment: 0 = centred, 1 = left, 2 = right. Left-aligned with
 * tracking is the classic geometric-poster setting. */
#define TUNE_TITLE_ALIGN       {     0,     0,     0,     0,     0 }
/* Overflow handling for titles: 0 = marquee, 1 = ellipsis, 2 = hard clip. */
#define TUNE_TITLE_LONG        {     0,     0,     0,     0,     0 }

/* ---- SHAPE --------------------------------------------------------------- */
/* Gap between the three transport keys (px). */
#define TUNE_TKEY_GAP          {    28,    28,    28,    28,    28 }
/* Border thickness (px) on album cards and on buttons. These carry theme
 * identity -- PAPER's printed 2 px rule, GLYPH's 1 px hairline -- so changing
 * them changes how "drawn" the UI reads. 0 = borderless. */
#define TUNE_CARD_BORDER       {     0,     0,     0,     2,     0 }
#define TUNE_BTN_BORDER        {     0,     1,     0,     2,     0 }
/* Drop shadow depth (px) under the centred album card. 0 = flat (the default
 * everywhere -- flat is a deliberate choice, not an omission). */
#define TUNE_CARD_SHADOW       {     0,     0,     0,     0,     0 }
/* Progress bar end caps: 0 = rounded, 1 = square. */
#define TUNE_PROG_CAP          {     0,     0,     0,     1,     0 }
/* Screen edge inset (px) for text that can scroll to the edges. */
#define TUNE_PAD               {     6,     6,     6,     6,     6 }

/* ---- LAYOUT (now-playing album art) -------------------------------------- */
#define TUNE_ART_W             {   256,   256,   256,   256,   256 }
#define TUNE_ART_Y             {    44,    44,    44,    44,    44 }

/* ---- BROWSER + MOTION ---------------------------------------------------- */
/* Carousel/Focus card slot size and gap (px). Cover Flow uses its own step. */
#define TUNE_CARD_SIZE         {   286,   286,   286,   286,   286 }
#define TUNE_CARD_GAP          {    28,    28,    28,    28,    28 }
/* Focus mode falloff: how fast side cards shrink and dim per slot away from
 * centre. Larger = more dramatic. */
#define TUNE_FOCUS_SCALE       {    76,    76,    76,    76,    76 }
#define TUNE_FOCUS_DIM         {    95,    95,    95,    95,    95 }
/* Cover Flow. SCALE is x100 (130 = 1.30). MAX_SIDE caps how many covers are
 * rasterised per side -- RAISING IT COSTS REAL FRAME TIME, the whole fan is
 * redrawn on every scroll event. LEAN_FLIP mirrors the tilt direction. */
#define TUNE_CF_SCALE          {   130,   130,   130,   130,   130 }
#define TUNE_CF_MAX_SIDE       {     3,     3,     3,     3,     3 }
#define TUNE_CF_LEAN_FLIP      {     0,     0,     0,     0,     0 }
/* Screen transition duration (ms) when the style is not NONE. */
#define TUNE_TRANS_MS          {   300,   300,   300,   300,   300 }
/* Auto-dim: seconds of inactivity before each stage, then the two backlight
 * levels (% of the user's brightness). */
#define TUNE_DIM_AFTER_S       {    60,    60,    60,    60,    60 }
#define TUNE_DIM_DEEP_S        {   300,   300,   300,   300,   300 }
#define TUNE_DIM_LEVEL         {    30,    30,    30,    30,    30 }
#define TUNE_DIM_DEEP_LEVEL    {    10,    10,    10,    10,    10 }
/* Volume step (%) for the +/- keys and the knob. */
#define TUNE_VOL_STEP          {     5,     5,     5,     5,     5 }
/* Cover Flow centre-tap tolerance (px) and the optimistic play/pause icon
 * hold (ms) before server state is trusted again. */
#define TUNE_TAP_TOL           {   140,   140,   140,   140,   140 }
#define TUNE_PP_GUARD_MS       {  2000,  2000,  2000,  2000,  2000 }

/* ---- THEME ART ----------------------------------------------------------- */
/* GLYPH dot-matrix pitch (px): smaller = finer, denser dots. */
#define TUNE_GLYPH_CELL        {     5,     5,     5,     5,     5 }
/* GLYPH gas-tank progress: number of drifting ink particles. */
#define TUNE_PROG_PARTS        {    44,    44,    44,    44,    44 }

/* =========================================================================
 * BROWSER (album list) -- text + furniture          { BASIC, GLYPH, PIXEL, PAPER }
 * ========================================================================= */
/* Cover-strip top Y and transport-key size. BASIC and PAPER start the strip at
 * y54; GLYPH/PIXEL stay at their original hardware-verified y30. BASIC was
 * lowered from y30 so the covers clear the top button row (ADD/DEVICES/etc):
 * at y30 the strip crowded the buttons; y54 leaves a ~20px gap. PAPER's y54
 * comes from its header rule (TUNE_PAPER_RULE_Y). TKEY_SZ: PAPER's transport
 * keys were shrunk to clear the art's lower rule; the others are untouched.
 * Strip height (SCROLLER_H, ui.c) is a flat 292 for every theme. */
#define TUNE_SCROLLER_Y        {    54,    30,    30,    54,    54 }
#define TUNE_TKEY_SZ           {    56,    56,    56,    48,    56 }
/* Album title / artist baseline Y. Tracks TUNE_SCROLLER_Y per mode: GLYPH/PIXEL
 * keep the 16px gap below their y322 strip bottom (342/384); BASIC and PAPER
 * start lower (strip bottom y346) so their title/artist follow to 362/404. */
#define TUNE_BR_TITLE_Y        {   362,   342,   342,   362,   362 }
#define TUNE_BR_ARTIST_Y       {   404,   384,   384,   404,   404 }
/* Extra letter spacing (px) on the browser + now-playing title text. BOLD
 * gets real tracking on its Jost Bold headings -- the geometric-poster
 * feel Futura/Bauhaus titling leans on. */
#define TUNE_TITLE_LETTER_SP   {     0,     0,     0,     0,     3 }
/* Selection underline: px below the strip bottom (TUNE_SCROLLER_Y + 292). */
#define TUNE_SEL_LINE_DY       {     4,     4,     4,     4,     4 }
/* FPS readout position (top-left). GLYPH starts further right so it clears
 * the 4-dot WiFi meter. PAPER: Y=17 centres mono_16 on the ~y25 header row;
 * X=64 clears the WiFi cluster now that it's inset further right (x0=20). */
#define TUNE_FPS_X             {    44,    64,    44,    64,    44 }
#define TUNE_FPS_Y             {     6,     6,     6,    17,     6 }
/* WiFi bar cluster: X of the leftmost bar, Y of the bars' shared bottom edge.
 * GLYPH's slot is unused (it shows an orbiting dot cluster instead of bars). */
#define TUNE_WIFI_X0           {     6,     6,     6,    20,     6 }
#define TUNE_WIFI_BOT          {    22,    22,    22,    34,    22 }
/* Settings cog / devices buttons (top-right corner): Y of both buttons,
 * and the X inset of each (negative = from the right edge). PAPER's Y=8
 * puts the TOPBTN_H-tall (34px, ui.c) button at y8..42, clearing the header
 * rule at y46 (icon centred ~y25, no longer cut off at the bottom). */
#define TUNE_TOPBTN_Y          {     0,     0,     0,     8,     0 }
#define TUNE_GEAR_X            (-6)
#define TUNE_DEVBTN_X          (-56)
#define TUNE_LIGHTSBTN_X       (-106)   /* one more 50px slot left of DEVBTN */
/* Add-albums "+" button: next 50px slot left of LIGHTS on the HA build. On the
 * non-HA build there is no lights button (P4_HAS_HA_LIGHTS unset) and the "+"
 * takes the TUNE_LIGHTSBTN_X slot instead -- see browser_build_top_buttons. */
#define TUNE_ADDBTN_X          (-156)
/* PAPER only: Y of the "NN / NN" album index counter, and of the printed
 * header rule. Counter Y=17 shares the buttons'/FPS's ~y25 header-row centre
 * (mono_16 line height 16). Rule Y=46 clears the button bottom (y42) and
 * still leaves a gap before the covers (TUNE_SCROLLER_Y[PAPER] = 54). */
#define TUNE_BR_INDEX_Y        17
#define TUNE_PAPER_RULE_Y      46

/* Devices-selector icon. The compiled Montserrat fonts carry these LVGL
 * symbols -- swap in whichever reads best to you:
 *   LV_SYMBOL_VOLUME_MID  small speaker (current -- works in ALL themes incl PIXEL)
 *   LV_SYMBOL_LIST        three stacked rows (U+F03C, NOT in the pixel font)
 *   LV_SYMBOL_WIFI        wireless fan
 *   LV_SYMBOL_BLUETOOTH   BT rune
 *   LV_SYMBOL_USB         USB trident
 *   LV_SYMBOL_GPS         location arrow
 *   LV_SYMBOL_AUDIO       music note
 * NOTE: the PIXEL font bakes a limited FA5 subset. Stick to symbols whose
 * codepoints are in: 0xF001, 0xF00C, 0xF011, 0xF013, 0xF021, 0xF026-0xF028,
 * 0xF048, 0xF04B, 0xF04C, 0xF051, 0xF053, 0xF054, 0xF077-0xF079.
 */
#define TUNE_DEVICES_ICON      LV_SYMBOL_VOLUME_MID

/* Lights-selector icon (HA build; harmless on non-HA, see ui_request_get_lights).
 * LVGL has no lightbulb glyph in this FA5 subset, so POWER (a toggle/on-off
 * rune) is the closest fit that's still in the PIXEL-safe codepoint list above
 * (0xF011). */
#define TUNE_LIGHTS_ICON       LV_SYMBOL_POWER

/* =========================================================================
 * NOW PLAYING                                       { BASIC, GLYPH, PIXEL, PAPER }
 * ========================================================================= */
/* Track title / artist Y (art occupies y44..300). */
#define TUNE_NP_TITLE_Y        {   308,   308,   308,   308,   308 }
#define TUNE_NP_ARTIST_Y       {   348,   348,   348,   348,   348 }
/* Progress bar Y. PAPER sits higher: its tick ruler needs room above the
 * transport keys. Timestamps, seek overlay and the GLYPH gas-tank all derive
 * from this value automatically. */
#define TUNE_PROG_Y            {   392,   392,   392,   378,   392 }
/* Width of each timestamp label. Wide enough that "-12:34" stays on ONE line:
 * Montserrat needs ~64, the chunky PIXEL / PAPER mono digits need ~104. */
#define TUNE_TS_W              {    64,    64,   104,   104,    64 }
/* Transport keys (prev/play/next) Y. */
#define TUNE_TKEY_Y            {   414,   414,   414,   414,   414 }
/* Vertical volume fader: X, top Y, track height (width is FADER_W, ui.c).
 * The square knob overhangs the track ends by ~26px at 0%/100% -- PAPER's
 * shorter, lower-started track keeps the knob clear of the LEVEL label
 * above (bottom y74) at 100% (knob top = fader_y - 26 = 80) and the printed
 * rule below. Bottom stays at y266 (106+160), same as the original 86+180. */
/* Volume column lives on the LEFT of the album art (OUTPUT moved to the
 * right column); the +/- buttons sit at fader_x + FADER_W + gap, and the
 * whole column stays clear of the art at ART_X=272. X=80 centres the fader +
 * button cluster (fader_x .. fader_x+108) in the 0..272 left zone -- was 36,
 * which hugged the left edge and left a lopsided ~128px gap before the art. */
#define TUNE_FADER_X           {    80,    80,    80,    80,    80 }
#define TUNE_FADER_Y           {    66,    66,    66,   106,    66 }
#define TUNE_FADER_H           {   236,   236,   236,   160,   236 }
/* PAPER only: "VOLUME" fader corner-label geometry, centred on the fader's
 * mid-x (FADER_W/2, ui.c). Y=58 matches OUTPUT's clearance below the y42
 * top rule. W=132: the mono font is ~16px/char, so "VOLUME" + letter-spacing
 * needs ~112px. */
#define TUNE_LEVEL_Y           58
#define TUNE_LEVEL_W           132
