#include "ui.h"
#include "spotify.h"
#include <Arduino.h>
#include <JPEGDEC.h>
#include <SD.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <XPT2046_Touchscreen.h>


extern TFT_eSPI tft;
extern bool get_touch_coords(int16_t *x, int16_t *y);

// Main display and touch objects
extern TFT_eSPI tft;
extern XPT2046_Touchscreen ts;

// View state
enum ViewMode { VIEW_BROWSER, VIEW_NOW_PLAYING };
static ViewMode current_view = VIEW_BROWSER;

// --- Album Capacity ---
#define MAX_ALBUMS 100
static int album_count = 0; // Actual number loaded from SD

#define SCREEN_W 320
#define SCREEN_H 240

// ============================================================
// UI LAYOUT CONFIGURATION (Easy Tweaks)
// ============================================================

// --- Text Sizes (Hardcoded Pixel Heights) ---
// Note: To save RAM, the ESP32 graphics library uses static bitmap fonts.
// You MUST choose one of the following exact pixel heights for every text
// element: 8, 16, 26, or 48
#define BROWSER_ALBUM_TEXT_SIZE 16
#define BROWSER_ARTIST_TEXT_SIZE 8
#define NP_ALBUM_TEXT_SIZE 8
#define NP_TITLE_TEXT_SIZE 16
#define NP_ARTIST_TEXT_SIZE 8

// Internal macro to convert pixel heights to TFT_eSPI font IDs
// Do not modify this macro.
#define GET_FONT_ID(px)                                                        \
  ((px) == 48 ? 6 : ((px) == 26 ? 4 : ((px) == 16 ? 2 : 1)))

// --- Album Browser Layout ---
#define BROWSER_ALBUM_GAP_BELOW_ART                                            \
  4 // Gap between bottom of album art and title text
#define BROWSER_ARTIST_GAP_BELOW_TITLE                                         \
  16 // Gap between title text and artist text

// --- Now Playing Layout ---
#define NP_TOP_TEXT_Y 40 // Standard Y position for the top Album text
#define NP_ART_CENTER_Y 115 // Vertical center coordinate for the rotating vinyl/square art
#define NP_BOTTOM_TITLE_Y 190  // Standard Y position for the bottom Title text
#define NP_BOTTOM_ARTIST_Y 210 // Standard Y position for the bottom Artist text
#define NP_SQUARE_ART_SIZE 120

// ============================================================

// Simple horizontal slide layout — drawn at 1.5x scale
#define ALBUM_SIZE 120
#define ALBUM_SPACING 140 // Center-to-center distance between albums
#define SPRITE_H 130
#define IMG_SRC_SIZE 80
#define IMG_PIXELS (IMG_SRC_SIZE * IMG_SRC_SIZE)

// --- Per-Album Metadata (loaded from SD metadata.csv) ---
static char album_filenames[MAX_ALBUMS][128];
static char album_titles[MAX_ALBUMS][32];
static char album_artists[MAX_ALBUMS][24];
static char album_uris[MAX_ALBUMS][48];

// --- 1-Slot SD Image Cache (heap-allocated) ---
extern bool sd_ok;
#define CACHE_SLOTS 1
static uint16_t *sd_img_cache[CACHE_SLOTS] = {nullptr};
static int cache_album_idx[CACHE_SLOTS] = {-1};
static unsigned long cache_access_time[CACHE_SLOTS] = {0};

static uint16_t fallback_color = 0x4208;

static void initCache() {
  for (int s = 0; s < CACHE_SLOTS; s++) {
    if (!sd_img_cache[s]) {
      if (ESP.getFreeHeap() < 20000) {
        Serial.print("Not enough heap for cache slot ");
        Serial.println(s);
        break;
      }
      sd_img_cache[s] = (uint16_t *)malloc(IMG_PIXELS * 2);
      if (sd_img_cache[s]) {
        Serial.print("Cache slot ");
        Serial.print(s);
        Serial.println(" OK");
      } else {
        Serial.print("Cache slot ");
        Serial.print(s);
        Serial.println(" malloc failed");
      }
    }
  }
}

// --- Scroll State (fixed-point, x100) ---
// ============================================================
// TUNING — Change these values to adjust feel
// ============================================================

// --- Encoder tuning ---
// Easing speed when scrolling via encoder.
// Lower = faster snap. 5 = near-instant, 12 = smooth, 20 = slow.
#define ENCODER_EASE_SPEED 5
// Min step per frame for encoder (prevents slow crawl at end).
#define ENCODER_MIN_STEP 8

// --- Touch tuning ---
// Easing speed when snapping after touch release.
// Lower = faster snap. 5 = near-instant, 12 = smooth, 20 = slow.
#define TOUCH_EASE_SPEED 10
// Min step per frame for touch snap.
#define TOUCH_MIN_STEP 5
// How many pixels of finger movement = 1 album scroll.
// Lower = more sensitive. 30 = very fast, 50 = moderate, 100 = slow.
#define TOUCH_DRAG_DIVISOR 40
// How far you can scroll past the first/last album.
#define OVERSCROLL_LIMIT 30

// --- Scroll State (fixed-point, x100) ---
static int32_t scroll_pos = 0;
static int32_t target_scroll = 0;
#define SCROLL_SCALE 140

// ── Overlay / HUD state ────────────────────────────────────────────────────
#include "input.h"

#define HUD_DURATION_MS   2000  // volume HUD auto-hides after 2 s
#define HUD_H             26    // pixel height of the HUD strip

static int           hud_vol_pct   = -1;
static bool          hud_muted_    = false;
static unsigned long hud_show_ms   = 0;
static bool          hud_was_on    = false; // tracks expiry to trigger redraw

static void _draw_volume_hud(int pct, bool muted) {
    tft.fillRect(0, 0, SCREEN_W, HUD_H, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    if (muted) {
        tft.setTextColor(tft.color565(220, 80, 80));
        tft.drawString("MUTED", SCREEN_W / 2, HUD_H / 2, GET_FONT_ID(16));
    } else {
        int bar_x = 16, bar_w = 200, bar_h = 6, bar_y = (HUD_H - bar_h) / 2;
        int fill_w = pct * bar_w / 100;
        tft.drawRect(bar_x, bar_y, bar_w, bar_h, tft.color565(80, 80, 80));
        if (fill_w > 0) tft.fillRect(bar_x, bar_y, fill_w, bar_h, TFT_WHITE);
        char buf[10];
        snprintf(buf, sizeof(buf), "%d%%", pct);
        tft.setTextColor(tft.color565(180, 180, 180));
        tft.drawString(buf, bar_x + bar_w + 20, HUD_H / 2, GET_FONT_ID(8));
    }
}

void ui_show_volume_hud(int pct, bool muted) {
    hud_vol_pct  = pct;
    hud_muted_   = muted;
    hud_show_ms  = millis();
    _draw_volume_hud(pct, muted);
}

// ── WiFi signal indicator ──────────────────────────────────────────────────
#define WIFI_CHECK_INTERVAL_MS 5000

static int           last_wifi_bars     = -1;  // -1 = never drawn
static unsigned long last_wifi_check_ms = 0;

static int _wifi_bars() {
    if (WiFi.status() != WL_CONNECTED) return 0;
    int rssi = WiFi.RSSI();
    if (rssi >= -55) return 4;
    if (rssi >= -65) return 3;
    if (rssi >= -75) return 2;
    if (rssi >= -85) return 1;
    return 0;
}

static void _draw_wifi_indicator(int bars) {
    // 4-bar icon, bottom-aligned, top-left corner
    // Each bar: 2px wide, 2px gap, heights 4/6/8/10 px, bottom at y=13
    tft.fillRect(2, 2, 18, 13, TFT_BLACK);
    for (int i = 0; i < 4; i++) {
        int bh    = 4 + i * 2;
        int bx    = 3 + i * 4;
        int by    = 14 - bh;
        uint16_t c = (i < bars) ? TFT_WHITE : tft.color565(55, 55, 55);
        tft.fillRect(bx, by, 2, bh, c);
    }
}

// ── Play/Pause flash ───────────────────────────────────────────────────────
#define PLAY_FLASH_MS 1500

static unsigned long play_flash_ms        = 0;
static bool          play_flash_is_play   = true;  // true=play icon, false=pause

static void _draw_play_pause_icon(bool is_play, int cx, int cy) {
    int r = 22;
    tft.fillCircle(cx, cy, r + 5, tft.color565(0, 0, 0));
    tft.drawCircle(cx, cy, r + 5, tft.color565(70, 70, 70));
    if (is_play) {
        tft.fillTriangle(cx - r / 2, cy - r, cx - r / 2, cy + r, cx + r, cy, TFT_WHITE);
    } else {
        int bw = r / 2 - 2, bh = r * 2;
        tft.fillRect(cx - r + 2, cy - r, bw, bh, TFT_WHITE);
        tft.fillRect(cx + 4,     cy - r, bw, bh, TFT_WHITE);
    }
}

// --- Touch State ---
static bool is_dragging = false;
static int16_t touch_start_x = 0;
static int16_t touch_start_y = 0;
static int32_t scroll_start = 0;
static unsigned long touch_start_time = 0;
static int32_t momentum_velocity = 0;
static bool ease_from_encoder = false; // Which input triggered current easing

// ============================================================
// SD Card Album Loading
// ============================================================

// Parse one CSV line into fields (handles quoted fields with commas)
static int parseCsvLine(char *line, char *fields[], int maxFields) {
  int count = 0;
  char *p = line;
  while (*p && count < maxFields) {
    if (*p == '"') {
      p++; // Skip the opening quote
      fields[count] = p;
      // Read until we hit a quote followed by a comma, or end of string
      while (*p && !(*p == '"' && (*(p + 1) == ',' || *(p + 1) == '\0' ||
                                   *(p + 1) == '\r' || *(p + 1) == '\n'))) {
        p++;
      }
      if (*p == '"') {
        *p = '\0'; // Replace closing quote with null terminator
        p++;       // Move past the old quote position
      }
      if (*p == ',') {
        p++;       // Move past the comma
      }
    } else {
      fields[count] = p;
      while (*p && *p != ',' && *p != '\r' && *p != '\n') {
        p++;
      }
      if (*p == ',') {
        *p = '\0';
        p++;
      } else if (*p) {
        *p = '\0';
        p++;
      }
    }
    count++;
  }
  return count;
}

static void loadAlbumsFromSD() {
  if (!sd_ok) {
    Serial.println("SD not available, no albums loaded");
    return;
  }

  Serial.println("SD card mounted. Scanning /sd_card_albums/...");

  File dir = SD.open("/sd_card_albums");
  if (!dir) {
    Serial.println("ERROR: /sd_card_albums folder not found!");
    return;
  }
  int fileCount = 0;
  while (File entry = dir.openNextFile()) {
    Serial.print("  Found: ");
    Serial.println(entry.name());
    fileCount++;
    entry.close();
  }
  dir.close();
  Serial.print("Total files in folder: ");
  Serial.println(fileCount);

  File f = SD.open("/sd_card_albums/metadata.csv", FILE_READ);
  if (!f) {
    Serial.println("ERROR: metadata.csv not found!");
    return;
  }

  char lineBuf[512];
  album_count = 0;

  while (f.available() && album_count < MAX_ALBUMS) {
    int len = 0;
    while (f.available() && len < 511) {
      char c = f.read();
      if (c == '\n')
        break;
      if (c != '\r')
        lineBuf[len++] = c;
    }
    lineBuf[len] = '\0';
    if (len == 0)
      continue;

    char *fields[4] = {nullptr, nullptr, nullptr, nullptr};
    int fieldCount = parseCsvLine(lineBuf, fields, 4);
    if (fieldCount < 3)
      continue;

    int i = album_count;
    strncpy(album_filenames[i], fields[0], 127);
    album_filenames[i][127] = '\0';
    strncpy(album_titles[i], fields[1], 31);
    album_titles[i][31] = '\0';
    strncpy(album_artists[i], fields[2], 23);
    album_artists[i][23] = '\0';
    if (fieldCount >= 4 && fields[3]) {
      strncpy(album_uris[i], fields[3], 47);
      album_uris[i][47] = '\0';
    } else {
      album_uris[i][0] = '\0';
    }

    // Verify this file actually exists on SD
    char path[128];
    snprintf(path, sizeof(path), "/sd_card_albums/%s", album_filenames[i]);
    File test = SD.open(path, FILE_READ);
    if (test) {
      test.close();
      album_count++;
    } else {
      Serial.print("SKIP (file not found): ");
      Serial.println(path);
    }
  }
  f.close();

  Serial.print("Loaded ");
  Serial.print(album_count);
  Serial.println(" albums from SD card");
}

// ============================================================
// Image Loading & Drawing
// ============================================================

// Find a cache slot for an album (returns slot index, or -1 on failure)
static int loadAlbumImage(int index) {
  if (!sd_ok || index < 0 || index >= album_count)
    return -1;

  // Check if already cached
  for (int s = 0; s < CACHE_SLOTS; s++) {
    if (cache_album_idx[s] == index && sd_img_cache[s]) {
      cache_access_time[s] = millis();
      return s;
    }
  }

  // Find LRU slot
  int lru = -1;
  for (int s = 0; s < CACHE_SLOTS; s++) {
    if (!sd_img_cache[s])
      continue;
    if (lru == -1 || cache_access_time[s] < cache_access_time[lru])
      lru = s;
  }
  if (lru == -1)
    return -1;

  char path[128];
  snprintf(path, sizeof(path), "/sd_card_albums/%s", album_filenames[index]);

  File f = SD.open(path, FILE_READ);
  if (!f)
    return -1;

  size_t bytesRead = f.read((uint8_t *)sd_img_cache[lru], IMG_PIXELS * 2);
  f.close();

  if (bytesRead != (size_t)(IMG_PIXELS * 2))
    return -1;

  for (int i = 0; i < IMG_PIXELS; i++) {
    uint16_t v = sd_img_cache[lru][i];
    sd_img_cache[lru][i] = (v >> 8) | (v << 8);
  }

  cache_album_idx[lru] = index;
  cache_access_time[lru] = millis();
  return lru;
}

static void drawAlbumArt(int x, int y, int index) {
  int slot = loadAlbumImage(index);
  if (slot >= 0) {
    uint16_t *img = sd_img_cache[slot];
    uint16_t line_buf[ALBUM_SIZE];

    tft.setSwapBytes(true);
    // 1.5x scaling: Map destination (0-119) to source (0-79)
    for (int dst_y = 0; dst_y < ALBUM_SIZE; dst_y++) {
      int src_y = (dst_y * 2) / 3;
      int src_idx = src_y * IMG_SRC_SIZE;

      for (int dst_x = 0; dst_x < ALBUM_SIZE; dst_x++) {
        int src_x = (dst_x * 2) / 3;
        line_buf[dst_x] = img[src_idx + src_x];
      }
      tft.pushImage(x, y + dst_y, ALBUM_SIZE, 1, line_buf);
    }
    tft.setSwapBytes(false);
  } else {
    tft.fillRoundRect(x, y, ALBUM_SIZE, ALBUM_SIZE, 4, fallback_color);
  }
}

// ============================================================
// Drawing
// ============================================================

// View redraw flags (forward-used by both draw_album_browser and draw_now_playing)
static bool np_needs_full_redraw = true;
static bool browser_needs_redraw = true;

static void draw_album_browser() {
  if (album_count == 0) {
    // Show error message instead of blank screen
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("No albums found", SCREEN_W / 2, SCREEN_H / 2 - 20, 2);
    tft.setTextColor(tft.color565(160, 160, 160));
    tft.setTextDatum(MC_DATUM);
    if (!sd_ok) {
      tft.drawString("SD card not detected", SCREEN_W / 2, SCREEN_H / 2 + 10,
                     1);
    } else {
      tft.drawString("Copy metadata.csv to SD card", SCREEN_W / 2,
                     SCREEN_H / 2 + 10, 1);
    }
    return;
  }

  int32_t scroll_px = scroll_pos * ALBUM_SPACING / SCROLL_SCALE;
  int y_offset = (SCREEN_H - SPRITE_H) / 2 - 15;
  int album_y = y_offset + (SPRITE_H - ALBUM_SIZE) / 2;

  // Optimize: Avoid redrawing if nothing has changed.
  // browser_needs_redraw is set true by ui_show_album_browser() so that
  // returning from the now-playing view always forces a fresh paint,
  // otherwise the static guard below would short-circuit and leave a black
  // screen (ui_show_album_browser blanks the screen before calling us).
  static int32_t last_drawn_scroll = -999;
  static ViewMode last_drawn_view = (ViewMode)-1;

  if (!browser_needs_redraw &&
      last_drawn_scroll == scroll_pos && last_drawn_view == current_view) {
    return;
  }
  browser_needs_redraw = false;

  // Clear background area for albums
  tft.fillRect(0, y_offset, SCREEN_W, SPRITE_H, TFT_BLACK);
  last_drawn_scroll = scroll_pos;
  last_drawn_view = current_view;

  for (int i = 0; i < album_count; i++) {
    int cx = SCREEN_W / 2 + i * ALBUM_SPACING - scroll_px;
    int ax = cx - ALBUM_SIZE / 2;

    // Skip if off-screen
    if (ax + ALBUM_SIZE < 0 || ax >= SCREEN_W)
      continue;

    drawAlbumArt(ax, album_y, i);
  }

  // Album title + artist text (center only)
  tft.fillRect(0, y_offset + SPRITE_H, SCREEN_W, 30, TFT_BLACK);
  int centerIndex = (scroll_pos + SCROLL_SCALE / 2) / SCROLL_SCALE;
  centerIndex = constrain(centerIndex, 0, album_count - 1);

  int32_t snapDist = abs(scroll_pos - (int32_t)centerIndex * SCROLL_SCALE);
  if (snapDist < 35) {
    tft.setTextColor(TFT_WHITE);
    tft.setTextDatum(TC_DATUM);
    tft.drawString(album_titles[centerIndex], SCREEN_W / 2,
                   y_offset + SPRITE_H + BROWSER_ALBUM_GAP_BELOW_ART,
                   GET_FONT_ID(BROWSER_ALBUM_TEXT_SIZE));
    tft.setTextColor(tft.color565(160, 160, 160));
    tft.drawString(album_artists[centerIndex], SCREEN_W / 2,
                   y_offset + SPRITE_H + BROWSER_ALBUM_GAP_BELOW_ART +
                       BROWSER_ARTIST_GAP_BELOW_TITLE,
                   GET_FONT_ID(BROWSER_ARTIST_TEXT_SIZE));
  }

  // Clear top
  tft.fillRect(0, 0, SCREEN_W, y_offset, TFT_BLACK);

  // Up chevron signifier for "Now Playing" at the bottom of the screen (^)
  tft.drawLine(SCREEN_W / 2 - 10, SCREEN_H - 10, SCREEN_W / 2, SCREEN_H - 20,
               tft.color565(180, 180, 180));
  tft.drawLine(SCREEN_W / 2, SCREEN_H - 20, SCREEN_W / 2 + 10, SCREEN_H - 10,
               tft.color565(180, 180, 180));
}

File npFile;

void *npOpen(const char *filename, int32_t *size) {
  npFile = SD.open(filename);
  if (!npFile)
    return nullptr;
  *size = npFile.size();
  return &npFile;
}
void npClose(void *handle) {
  if (npFile)
    npFile.close();
}
int32_t npRead(JPEGFILE *handle, uint8_t *buffer, int32_t length) {
  if (!npFile)
    return 0;
  return npFile.read(buffer, length);
}
int32_t npSeek(JPEGFILE *handle, int32_t position) {
  if (!npFile)
    return 0;
  return npFile.seek(position) ? position : -1;
}

int np_img_x = 0;
int np_img_y = 0;
static int JPEGDraw_NowPlaying(JPEGDRAW *pDraw) {
  tft.pushImage(np_img_x + pDraw->x, np_img_y + pDraw->y,
                pDraw->iWidth, pDraw->iHeight, pDraw->pPixels);
  return 1;
}

static void drawLocalAlbumArt(int center_x, int center_y, int index) {
  int slot = loadAlbumImage(index);
  if (slot < 0) {
    tft.fillRoundRect(center_x - NP_SQUARE_ART_SIZE / 2, center_y - NP_SQUARE_ART_SIZE / 2,
                      NP_SQUARE_ART_SIZE, NP_SQUARE_ART_SIZE, 4, fallback_color);
    return;
  }
  uint16_t *img = sd_img_cache[slot];
  int img_x = center_x - NP_SQUARE_ART_SIZE / 2;
  int img_y = center_y - NP_SQUARE_ART_SIZE / 2;
  uint16_t line_buf[NP_SQUARE_ART_SIZE];
  tft.setSwapBytes(true);
  for (int dy = 0; dy < NP_SQUARE_ART_SIZE; dy++) {
    int src_y = (dy * IMG_SRC_SIZE) / NP_SQUARE_ART_SIZE;
    int src_idx = src_y * IMG_SRC_SIZE;
    for (int dx = 0; dx < NP_SQUARE_ART_SIZE; dx++) {
      int src_x = (dx * IMG_SRC_SIZE) / NP_SQUARE_ART_SIZE;
      line_buf[dx] = img[src_idx + src_x];
    }
    tft.pushImage(img_x, img_y + dy, NP_SQUARE_ART_SIZE, 1, line_buf);
  }
  tft.setSwapBytes(false);
}


// Track-change detection: keep last drawn title/album so we only redo the
// expensive full redraw (and JPEG re-decode) when the track actually changes.
// Spotify polling sets track_info_updated every 2 s for progress; using that
// as the redraw trigger flickered the screen every poll.
static char np_last_title[64] = "";
static char np_last_album[64] = "";

static void draw_now_playing() {
  bool track_changed = strncmp(np_last_title, current_track_info.title, sizeof(np_last_title)) != 0 ||
                        strncmp(np_last_album, current_track_info.album, sizeof(np_last_album)) != 0;
  bool initial_draw = np_needs_full_redraw || track_changed;

  if (initial_draw) {
    tft.fillScreen(TFT_BLACK);
    strncpy(np_last_title, current_track_info.title, sizeof(np_last_title) - 1);
    np_last_title[sizeof(np_last_title) - 1] = '\0';
    strncpy(np_last_album, current_track_info.album, sizeof(np_last_album) - 1);
    np_last_album[sizeof(np_last_album) - 1] = '\0';

    tft.setTextColor(TFT_WHITE);
    tft.setTextDatum(MC_DATUM);

    // Equalize the text margins: Top Album text
    tft.setTextColor(tft.color565(180, 180, 180));
    tft.drawString(current_track_info.album, SCREEN_W / 2, NP_TOP_TEXT_Y,
                   GET_FONT_ID(NP_ALBUM_TEXT_SIZE));

    // Bottom Title and Artist
    tft.setTextColor(TFT_WHITE);
    tft.drawString(current_track_info.title, SCREEN_W / 2, NP_BOTTOM_TITLE_Y,
                   GET_FONT_ID(NP_TITLE_TEXT_SIZE));
    tft.setTextColor(tft.color565(160, 160, 160));
    tft.drawString(current_track_info.artist, SCREEN_W / 2, NP_BOTTOM_ARTIST_Y,
                   GET_FONT_ID(NP_ARTIST_TEXT_SIZE));

    // Down chevron signifier for "Album Browser" at the top of the screen (V)
    tft.drawLine(SCREEN_W / 2 - 10, 5, SCREEN_W / 2, 12,
                 tft.color565(180, 180, 180));
    tft.drawLine(SCREEN_W / 2, 12, SCREEN_W / 2 + 10, 5,
                 tft.color565(180, 180, 180));

    // WiFi indicator (top-left corner)
    _draw_wifi_indicator(last_wifi_bars >= 0 ? last_wifi_bars : _wifi_bars());

    np_needs_full_redraw = false;
    track_info_updated = false; // consume the flag
  }

  if (initial_draw) {
    if (current_track_info.local_album_idx >= 0) {
      drawLocalAlbumArt(SCREEN_W / 2, NP_ART_CENTER_Y, current_track_info.local_album_idx);
    } else {
      JPEGDEC *jpeg_np = new JPEGDEC();
      bool decoded = false;
      if (jpeg_np->open("/sd_card_albums/nowplaying.jpg", npOpen, npClose, npRead,
                       npSeek, JPEGDraw_NowPlaying)) {
        int w = jpeg_np->getWidth();
        int h = jpeg_np->getHeight();
        // Guard against unparseable / progressive JPEGs reporting w==0 or h==0
        // — without this we'd compute np_img_x = SCREEN_W/2 and draw the
        // remaining MCU blocks from there toward the bottom-right of the screen.
        if (w > 0 && h > 0) {
          int scale = 0;
          if (w >= 480) scale = JPEG_SCALE_QUARTER;
          else if (w >= 240) scale = JPEG_SCALE_HALF;
          int dw = w >> scale;
          int dh = h >> scale;
          np_img_x = (SCREEN_W / 2) - (dw / 2);
          np_img_y = NP_ART_CENTER_Y - (dh / 2);
          // Clamp so a too-large image still has its top-left on-screen
          if (np_img_x < 0) np_img_x = 0;
          if (np_img_y < HUD_H) np_img_y = HUD_H;
          // JPEGDEC outputs little-endian RGB565; ILI9341 wants big-endian.
          // Without this byte-swap the colours come out inverted (R/B swapped).
          tft.setSwapBytes(true);
          decoded = (jpeg_np->decode(0, 0, scale) == 1);
          tft.setSwapBytes(false);
        }
        jpeg_np->close();
      }
      if (!decoded) {
        tft.fillRect((SCREEN_W - NP_SQUARE_ART_SIZE) / 2,
                     NP_ART_CENTER_Y - NP_SQUARE_ART_SIZE / 2,
                     NP_SQUARE_ART_SIZE, NP_SQUARE_ART_SIZE, fallback_color);
      }
      delete jpeg_np;
    }
  }

  // ── Play/Pause flash (drawn on top of art for 1.5 s after state change) ─
  if (play_flash_ms > 0 && millis() - play_flash_ms < PLAY_FLASH_MS) {
      _draw_play_pause_icon(play_flash_is_play, SCREEN_W / 2, NP_ART_CENTER_Y);
  }

  // ── Mute badge (persistent small indicator top-right) ─────────────────
  static bool last_muted_badge = false;
  bool cur_muted = input_is_muted();
  if (cur_muted != last_muted_badge || initial_draw) {
      // Clear the badge area (top-right corner, 60x14 px)
      tft.fillRect(SCREEN_W - 62, 2, 60, 14, TFT_BLACK);
      if (cur_muted) {
          tft.setTextDatum(MR_DATUM);
          tft.setTextColor(tft.color565(220, 80, 80));
          tft.drawString("MUTED", SCREEN_W - 4, 9, GET_FONT_ID(8));
      }
      last_muted_badge = cur_muted;
  }

  static uint32_t last_prog = 0xFFFFFFFF; // force first draw
  static int last_fill_w = -1;

  int prog_w = 200;
  int prog_x = (SCREEN_W - prog_w) / 2;
  int prog_y = 225; // Move progress bar to the very bottom
  
  int current_fill_w = 0;
  if (current_track_info.duration_ms > 0) {
      current_fill_w = (int)(((float)current_track_info.progress_ms /
                          current_track_info.duration_ms) *
                         prog_w);
      if (current_fill_w > prog_w) current_fill_w = prog_w;
  }

  // Determine if we need to redraw progress bar (if jumped, resized, or initialized)
  bool reset_prog = (current_track_info.progress_ms < last_prog) || (last_prog == 0xFFFFFFFF) || initial_draw;
  
  bool redraw_prog = reset_prog || (current_fill_w != last_fill_w);

  if (redraw_prog) {
    // To stop flicker, only draw the fill extending, don't clear the whole rect
    // unless reset
    if (reset_prog) {
      tft.fillRect(prog_x, prog_y - 2, prog_w + 4, 10, TFT_BLACK); // clearing rect safely
      tft.drawRect(prog_x, prog_y, prog_w, 6, tft.color565(100, 100, 100)); // dark grey outline rect
    }

    if (current_track_info.duration_ms > 0) {
      // Draw pure white fill instead of green
      tft.fillRect(prog_x, prog_y, current_fill_w, 6, TFT_WHITE);
    }
    
    last_prog = current_track_info.progress_ms;
    last_fill_w = current_fill_w;
  }
}

static void draw_ui() {
  if (current_view == VIEW_BROWSER) {
    draw_album_browser();
  } else if (current_view == VIEW_NOW_PLAYING) {
    draw_now_playing();
  }
}

// ============================================================
// Public API
// ============================================================

void ui_init() {
  // We no longer allocate the 83.2KB Sprite. This guarantees Spotify TLS connection success.

  // Now allocate cache slots from remaining heap
  Serial.print("Free heap: ");
  Serial.println(ESP.getFreeHeap());
  initCache();

  loadAlbumsFromSD();
  scroll_pos = 0;
  target_scroll = 0;
  draw_ui();
}

void ui_suspend_sprite() {
  // Empty stub: Direct draw uses 0 RAM overhead!
}

void ui_resume_sprite() {
  // Empty stub: Direct draw uses 0 RAM overhead!
}

void ui_update() {
  if (album_count == 0)
    return;

  static int32_t last_scroll_pos = -1;
  static unsigned long last_update_time = 0;
  unsigned long now = millis();
  unsigned long dt = now - last_update_time;
  if (dt > 100)
    dt = 16;
  if (dt > 0)
    last_update_time = now;

  // Simulate progress bar incrementing if we are playing local track
  if (current_track_info.is_playing && current_track_info.duration_ms > 0) {
    current_track_info.progress_ms += dt;
    if (current_track_info.progress_ms >= current_track_info.duration_ms) {
      current_track_info.progress_ms = 0;
      current_track_info.is_playing = false;
    }
  }

  int16_t tx, ty;
  bool touched = get_touch_coords(&tx, &ty);

  // --- Rotary encoder: sets target, cancels touch ---
  extern int32_t get_encoder_delta();

  int32_t enc = -get_encoder_delta();
  if (enc > 1)
    enc = 1;
  if (enc < -1)
    enc = -1;
  bool encoder_active = false;
  if (enc != 0 && current_view == VIEW_BROWSER) {
    encoder_active = true;
    is_dragging = false;
    momentum_velocity = 0;

    int current_album = constrain(
        (target_scroll + SCROLL_SCALE / 2) / SCROLL_SCALE, 0, album_count - 1);
    int next_album = constrain(current_album + enc, 0, album_count - 1);
    target_scroll = (int32_t)next_album * SCROLL_SCALE;
    ease_from_encoder = true;
  }

  // --- Touch input (only if encoder didn't fire this frame) ---
  if (!encoder_active) {
    if (touched) {
      if (!is_dragging) {
        is_dragging = true;
        touch_start_x = tx;
        touch_start_y = ty;
        scroll_start = scroll_pos;
        touch_start_time = now;
        momentum_velocity = 0;
      } else {
        int16_t dx = tx - touch_start_x;
        int16_t dy = ty - touch_start_y;

        // Check for vertical swipe to switch views
        if (abs(dy) > 50 && abs(dy) > abs(dx) * 2) {
          if (dy < -50 && current_view == VIEW_BROWSER) {
            ui_show_now_playing();
          } else if (dy > 50 && current_view == VIEW_NOW_PLAYING) {
            ui_show_album_browser();
          }
        }

        if (current_view == VIEW_BROWSER) {
          scroll_pos =
              scroll_start - (int32_t)dx * SCROLL_SCALE / TOUCH_DRAG_DIVISOR;

          int32_t lo = -OVERSCROLL_LIMIT;
          int32_t hi =
              (int32_t)(album_count - 1) * SCROLL_SCALE + OVERSCROLL_LIMIT;
          scroll_pos = constrain(scroll_pos, lo, hi);
          target_scroll = scroll_pos;
        } else if (current_view == VIEW_NOW_PLAYING) {
          // Ignored dragged in now playing
        }
      }
    } else if (is_dragging) {
      // Touch released — snap to nearest album (no momentum fling)
      is_dragging = false;
      unsigned long duration = now - touch_start_time;
      int32_t totalDrag = abs(scroll_pos - scroll_start);

      if (current_view == VIEW_BROWSER) {
        if (totalDrag < 6 && duration < 300) {
          // Tap — play center album
          int ci = constrain((scroll_pos + SCROLL_SCALE / 2) / SCROLL_SCALE, 0,
                             album_count - 1);
          Serial.print("Tapped: ");
          Serial.println(album_titles[ci]);

          // Send play command to Spotify API
          if (strlen(album_uris[ci]) > 0) {
            spotify_play_album(album_uris[ci]);
          } else {
            Serial.println("Warning: No Spotify URI for this album.");
          }

          // We switch to the Now Playing view immediately. 
          // The background API poller will catch the new track metadata automatically 
          // on its next 2-second checking interval.
          ui_show_now_playing();
        }

        // Snap to nearest album
        int closest = constrain((scroll_pos + SCROLL_SCALE / 2) / SCROLL_SCALE,
                                0, album_count - 1);
        target_scroll = (int32_t)closest * SCROLL_SCALE;
        ease_from_encoder = false;
      }
    }
  }

  // --- Easing toward target (uses encoder or touch speed) ---
  if (dt > 0 && !is_dragging && scroll_pos != target_scroll &&
      current_view == VIEW_BROWSER) {
    int32_t ease_spd =
        ease_from_encoder ? ENCODER_EASE_SPEED : TOUCH_EASE_SPEED;
    int32_t min_stp = ease_from_encoder ? ENCODER_MIN_STEP : TOUCH_MIN_STEP;
    int32_t diff = target_scroll - scroll_pos;
    int32_t step = diff * (int32_t)dt / ease_spd;
    if (step == 0 || (abs(step) < min_stp && abs(diff) > 1))
      step = (diff > 0) ? min_stp : -min_stp;

    if (abs(diff) <= abs(step))
      scroll_pos = target_scroll;
    else
      scroll_pos += step;
  }

  // ── Volume HUD expiry ─────────────────────────────────────────────────
  bool hud_now_on = (hud_show_ms > 0 && now - hud_show_ms < HUD_DURATION_MS);
  if (hud_was_on && !hud_now_on) {
      // HUD just expired — restore background beneath it
      if (current_view == VIEW_NOW_PLAYING) {
          np_needs_full_redraw = true;
      } else {
          // Browser: blank the strip then restore the WiFi indicator
          tft.fillRect(0, 0, SCREEN_W, HUD_H, TFT_BLACK);
          if (last_wifi_bars >= 0) _draw_wifi_indicator(last_wifi_bars);
      }
  }
  hud_was_on = hud_now_on;

  // ── Play/Pause change detection ───────────────────────────────────────
  static bool last_is_playing = false;
  if (current_track_info.is_playing != last_is_playing) {
      play_flash_is_play = current_track_info.is_playing;
      play_flash_ms      = now;
      last_is_playing    = current_track_info.is_playing;
  }

  // ── WiFi signal poll (every 5 s, redraw only on bar change) ──────────
  if (now - last_wifi_check_ms > WIFI_CHECK_INTERVAL_MS) {
      last_wifi_check_ms = now;
      int bars = _wifi_bars();
      if (bars != last_wifi_bars) {
          last_wifi_bars = bars;
          if (!hud_now_on) {
              _draw_wifi_indicator(bars);
          }
          // If HUD is covering it, the expiry redraw above will restore it
      }
  }

  // --- Draw at ~60fps ---
  static unsigned long last_frame = 0;
  if (now - last_frame >= 16) {
    if (scroll_pos != last_scroll_pos || current_view == VIEW_NOW_PLAYING ||
        track_info_updated) {
      draw_ui();
      last_scroll_pos = scroll_pos;
      track_info_updated = false;
    }
    last_frame = now;
  }
}

void ui_show_album_browser() {
  if (current_view != VIEW_BROWSER) {
    current_view = VIEW_BROWSER;
    tft.fillScreen(TFT_BLACK);
    browser_needs_redraw = true; // bypass the static-guard short-circuit
    draw_album_browser();        // Force an immediate redraw of the browser
  }
}

void ui_show_now_playing() {
  if (current_view != VIEW_NOW_PLAYING) {
    current_view = VIEW_NOW_PLAYING;
    np_needs_full_redraw = true; // force the initial draw
    draw_ui();                   // Force an immediate redraw of now playing
  }
}

void ui_toggle_view() {
  if (current_view == VIEW_BROWSER) {
    ui_show_now_playing();
  } else {
    ui_show_album_browser();
  }
}

bool ui_is_now_playing() {
  return current_view == VIEW_NOW_PLAYING;
}

void ui_play_centered_album() {
  int ci = constrain((scroll_pos + SCROLL_SCALE / 2) / SCROLL_SCALE, 0, album_count - 1);
  if (ci < album_count && strlen(album_uris[ci]) > 0) {
    spotify_play_album(album_uris[ci]);
    ui_show_now_playing();
  }
}