/*
 * ui_tune.h -- USER-TWEAKABLE layout + colour knobs for the Waveshare UI.
 *
 * Everything here is safe to edit by eye: change a number, rebuild + flash
 * (idf.py build flash), look at the result. Nothing in ui.c needs touching.
 *
 * Most knobs are per-MODE arrays in this order:
 *
 *        { BASIC, GLYPH, PIXEL, PAPER }
 *
 * so each theme can have its own spacing without affecting the others.
 * Y values are pixels from the top of the 800x480 landscape panel; X from the
 * left. The album strip occupies y30..326 in every browser style.
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
/* Album title / artist baseline Y (strip ends at 326). */
#define TUNE_BR_TITLE_Y        { 342, 342, 342, 342 }
#define TUNE_BR_ARTIST_Y       { 384, 384, 384, 384 }
/* Extra letter spacing (px) on the browser + now-playing title text. */
#define TUNE_TITLE_LETTER_SP   {   0,   0,   0,   0 }
/* Selection underline: px below the strip bottom (y326). */
#define TUNE_SEL_LINE_DY       {   4,   4,   4,   4 }
/* FPS readout position (top-left). GLYPH starts further right so it clears
 * the 4-dot WiFi meter. */
#define TUNE_FPS_X             {  44,  64,  44,  44 }
#define TUNE_FPS_Y             {   6,   6,   6,   8 }
/* Settings cog / devices buttons (top-right corner): Y of both buttons,
 * and the X inset of each (negative = from the right edge). */
#define TUNE_TOPBTN_Y          {   0,   0,   0,   8 }
#define TUNE_GEAR_X            (-6)
#define TUNE_DEVBTN_X          (-56)
/* PAPER only: Y of the printed header rule on the browser. */
#define TUNE_PAPER_RULE_Y      40

/* Devices-selector icon. The compiled Montserrat fonts carry these LVGL
 * symbols -- swap in whichever reads best to you:
 *   LV_SYMBOL_LIST        three stacked rows (a device list)
 *   LV_SYMBOL_WIFI        wireless fan
 *   LV_SYMBOL_BLUETOOTH   BT rune
 *   LV_SYMBOL_USB         USB trident
 *   LV_SYMBOL_GPS         location arrow
 *   LV_SYMBOL_VOLUME_MID  small speaker
 *   LV_SYMBOL_AUDIO       music note (the original)
 */
#define TUNE_DEVICES_ICON      LV_SYMBOL_LIST

/* =========================================================================
 * NOW PLAYING                                       { BASIC, GLYPH, PIXEL, PAPER }
 * ========================================================================= */
/* Track title / artist Y (art occupies y44..300). */
#define TUNE_NP_TITLE_Y        { 308, 308, 308, 308 }
#define TUNE_NP_ARTIST_Y       { 348, 348, 348, 348 }
/* Progress bar Y. PAPER sits higher: its tick ruler needs room above the
 * transport keys. Timestamps, seek overlay and the GLYPH gas-tank all derive
 * from this value automatically. */
#define TUNE_PROG_Y            { 392, 392, 392, 378 }
/* Width of each timestamp label. Wide enough that "-12:34" stays on ONE line:
 * Montserrat needs ~64, the chunky PIXEL / PAPER mono digits need ~104. */
#define TUNE_TS_W              {  64,  64, 104, 104 }
/* Transport keys (prev/play/next) Y. */
#define TUNE_TKEY_Y            { 414, 414, 414, 414 }
/* Vertical volume fader: X, top Y, track height. The square knob overhangs
 * the track ends by ~26px at 0%/100% -- PAPER's shorter track keeps it clear
 * of the LEVEL label above and the printed rule below. */
#define TUNE_FADER_X           { 708, 708, 708, 708 }
#define TUNE_FADER_Y           {  66,  66,  66,  86 }
#define TUNE_FADER_H           { 236, 236, 236, 180 }
