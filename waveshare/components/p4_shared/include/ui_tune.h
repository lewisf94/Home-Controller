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
 * BROWSER (album list) -- text + furniture          { BASIC, GLYPH, PIXEL, PAPER }
 * ========================================================================= */
/* Cover-strip top Y and transport-key size. BASIC and PAPER start the strip at
 * y54; GLYPH/PIXEL stay at their original hardware-verified y30. BASIC was
 * lowered from y30 so the covers clear the top button row (ADD/DEVICES/etc):
 * at y30 the strip crowded the buttons; y54 leaves a ~20px gap. PAPER's y54
 * comes from its header rule (TUNE_PAPER_RULE_Y). TKEY_SZ: PAPER's transport
 * keys were shrunk to clear the art's lower rule; the others are untouched.
 * Strip height (SCROLLER_H, ui.c) is a flat 292 for every theme. */
#define TUNE_SCROLLER_Y        {  54,  30,  30,  54,  54 }
#define TUNE_TKEY_SZ           {  56,  56,  56,  48,  56 }
/* Album title / artist baseline Y. Tracks TUNE_SCROLLER_Y per mode: GLYPH/PIXEL
 * keep the 16px gap below their y322 strip bottom (342/384); BASIC and PAPER
 * start lower (strip bottom y346) so their title/artist follow to 362/404. */
#define TUNE_BR_TITLE_Y        { 362, 342, 342, 362, 362 }
#define TUNE_BR_ARTIST_Y       { 404, 384, 384, 404, 404 }
/* Extra letter spacing (px) on the browser + now-playing title text. BOLD
 * gets a touch of tracking on its Jost Bold headings -- the geometric-poster
 * feel Futura/Bauhaus titling leans on. */
#define TUNE_TITLE_LETTER_SP   {   0,   0,   0,   0,   1 }
/* Selection underline: px below the strip bottom (TUNE_SCROLLER_Y + 292). */
#define TUNE_SEL_LINE_DY       {   4,   4,   4,   4,   4 }
/* FPS readout position (top-left). GLYPH starts further right so it clears
 * the 4-dot WiFi meter. PAPER: Y=17 centres mono_16 on the ~y25 header row;
 * X=64 clears the WiFi cluster now that it's inset further right (x0=20). */
#define TUNE_FPS_X             {  44,  64,  44,  64,  44 }
#define TUNE_FPS_Y             {   6,   6,   6,  17,   6 }
/* WiFi bar cluster: X of the leftmost bar, Y of the bars' shared bottom edge.
 * GLYPH's slot is unused (it shows an orbiting dot cluster instead of bars). */
#define TUNE_WIFI_X0           {   6,   6,   6,  20,   6 }
#define TUNE_WIFI_BOT          {  22,  22,  22,  34,  22 }
/* Settings cog / devices buttons (top-right corner): Y of both buttons,
 * and the X inset of each (negative = from the right edge). PAPER's Y=8
 * puts the TOPBTN_H-tall (34px, ui.c) button at y8..42, clearing the header
 * rule at y46 (icon centred ~y25, no longer cut off at the bottom). */
#define TUNE_TOPBTN_Y          {   0,   0,   0,   8,   0 }
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
#define TUNE_NP_TITLE_Y        { 308, 308, 308, 308, 308 }
#define TUNE_NP_ARTIST_Y       { 348, 348, 348, 348, 348 }
/* Progress bar Y. PAPER sits higher: its tick ruler needs room above the
 * transport keys. Timestamps, seek overlay and the GLYPH gas-tank all derive
 * from this value automatically. */
#define TUNE_PROG_Y            { 392, 392, 392, 378, 392 }
/* Width of each timestamp label. Wide enough that "-12:34" stays on ONE line:
 * Montserrat needs ~64, the chunky PIXEL / PAPER mono digits need ~104. */
#define TUNE_TS_W              {  64,  64, 104, 104,  64 }
/* Transport keys (prev/play/next) Y. */
#define TUNE_TKEY_Y            { 414, 414, 414, 414, 414 }
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
#define TUNE_FADER_X           {  80,  80,  80,  80,  80 }
#define TUNE_FADER_Y           {  66,  66,  66, 106,  66 }
#define TUNE_FADER_H           { 236, 236, 236, 160, 236 }
/* PAPER only: "VOLUME" fader corner-label geometry, centred on the fader's
 * mid-x (FADER_W/2, ui.c). Y=58 matches OUTPUT's clearance below the y42
 * top rule. W=132: the mono font is ~16px/char, so "VOLUME" + letter-spacing
 * needs ~112px. */
#define TUNE_LEVEL_Y           58
#define TUNE_LEVEL_W           132
