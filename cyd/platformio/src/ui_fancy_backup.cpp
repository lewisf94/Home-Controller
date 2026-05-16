#include "ui.h"
#include "spotify.h"
#include <Arduino.h>
#include <JPEGDEC.h>
#include <SD.h>
#include <TFT_eSPI.h>


extern TFT_eSPI tft;
extern bool get_touch_coords(int16_t *x, int16_t *y);

// Sprite for double buffering
TFT_eSprite spr = TFT_eSprite(&tft);
static bool sprite_created = false;

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
#define NP_SQUARE_ART_SIZE 120 // Dimensions of the static square art (WxH)
#define NP_VINYL_OUTER_RADIUS 60 // Outer radius of the spinning vinyl record
#define NP_VINYL_INNER_RADIUS 8 // Inner 'hole' radius of the spinning vinyl record

// --- Tonearm Layout ---
// --- Tonearm Layout ---
#define NP_TONEARM_PIVOT_X 260     // X coordinate of the tonearm pivot point (top right)
#define NP_TONEARM_PIVOT_Y 65      // Y coordinate of the tonearm pivot point (moved down so base doesn't clip)
#define NP_TONEARM_LENGTH 112      // Length of the tonearm from pivot to needle
#define NP_TONEARM_START_ANGLE 123 // Starting angle (degrees) resting at the outer edge (180 = left, 90 = down)
#define NP_TONEARM_END_ANGLE 147   // Ending angle (degrees) resting near the center hole
#define NP_TONEARM_THICKNESS 5     // Thickness of the tonearm rod (odd numbers recommended, e.g. 3, 5, 7)
#define NP_TONEARM_COLOR 0xC618            // Silver color for the arm (RGB565 space, e.g., tft.color565(200,200,200))
#define NP_TONEARM_BASE_COLOR 0x4A69       // Dark grey for the outer circle of the pivot base
#define NP_TONEARM_BASE_CENTER_COLOR 0x0000 // Black for the inner dot of the pivot base
#define NP_TONEARM_DOT_COLOR 0xF800        // Red for the stylus needle dot

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

    spr.setSwapBytes(true);
    // 1.5x scaling: Map destination (0-119) to source (0-79)
    for (int dst_y = 0; dst_y < ALBUM_SIZE; dst_y++) {
      int src_y = (dst_y * 2) / 3;
      int src_idx = src_y * IMG_SRC_SIZE;

      for (int dst_x = 0; dst_x < ALBUM_SIZE; dst_x++) {
        int src_x = (dst_x * 2) / 3;
        line_buf[dst_x] = img[src_idx + src_x];
      }
      spr.pushImage(x, y + dst_y, ALBUM_SIZE, 1, line_buf);
    }
    spr.setSwapBytes(false);
  } else {
    spr.fillRoundRect(x, y, ALBUM_SIZE, ALBUM_SIZE, 4, fallback_color);
  }
}

// ============================================================
// Drawing
// ============================================================

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

  if (!sprite_created)
    return; // Sprite created in ui_init

  spr.fillSprite(TFT_BLACK);

  // Simple horizontal slide: calculate pixel offset from scroll position
  // scroll_pos is in fixed-point (x100), so album i is at pixel center:
  //   cx = SCREEN_W/2 + (i * ALBUM_SPACING) - scroll_pos * ALBUM_SPACING /
  //   SCROLL_SCALE
  int32_t scroll_px = scroll_pos * ALBUM_SPACING / SCROLL_SCALE;
  int album_y = (SPRITE_H - ALBUM_SIZE) / 2;

  for (int i = 0; i < album_count; i++) {
    int cx = SCREEN_W / 2 + i * ALBUM_SPACING - scroll_px;
    int ax = cx - ALBUM_SIZE / 2;

    // Skip if off-screen
    if (ax + ALBUM_SIZE < 0 || ax >= SCREEN_W)
      continue;

    drawAlbumArt(ax, album_y, i);
  }

  // Push sprite shifted slightly higher to balance screen
  int y_offset = (SCREEN_H - SPRITE_H) / 2 - 15;
  spr.pushSprite(0, y_offset);

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

JPEGDEC jpeg_np;
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
  uint16_t *pixels = pDraw->pPixels;
  int center_x = 160;
  int center_y = 120;      // Centered nicely between top/bottom text
  int radius_sq = 45 * 45; // Smaller circle (radius 45 instead of 60)

  // pDraw->x and pDraw->y are the offset of the current block WITHIN the image.
  // np_img_x and np_img_y are where we placed the top-left of the image ON
  // SCREEN. So absolute screen coordinates are: np_img_x + pDraw->x + x

  for (int y = 0; y < pDraw->iHeight; y++) {
    for (int x = 0; x < pDraw->iWidth; x++) {
      int screen_x = np_img_x + pDraw->x + x;
      int screen_y = np_img_y + pDraw->y + y;
      int dx = screen_x - center_x;
      int dy = screen_y - center_y;

      // Only draw if within our nice 90x90 circle mask on screen
      if ((dx * dx) + (dy * dy) <= radius_sq) {
        tft.drawPixel(screen_x, screen_y, pixels[y * pDraw->iWidth + x]);
      }
    }
  }
  return 1;
}

// Precomputed sine/cosine helpers for fast rotation
static float fsin(float angle) { return sin(angle); }
static float fcos(float angle) { return cos(angle); }

static bool np_show_square_art = false;
static float current_rotation_angle = 0.0f;

static void drawLocalAlbumArt(int center_x, int center_y, int index, bool to_sprite = false, int sprite_y_offset = 0) {
  int slot = loadAlbumImage(index);
  if (slot < 0) {
    // Fallback drawing if local cache fails
    int size = np_show_square_art ? 140 : 120;
    tft.fillRoundRect(center_x - size / 2, center_y - size / 2, size, size, 4,
                      fallback_color);
    return;
  }

  uint16_t *img = sd_img_cache[slot];

  if (np_show_square_art) {
    int TARGET_SIZE = NP_SQUARE_ART_SIZE; // Larger static square
    int img_x = center_x - TARGET_SIZE / 2;
    int img_y = center_y - TARGET_SIZE / 2;
    // Use straight TFT push for now playing (no full screen sprite used here)
    uint16_t line_buf[NP_SQUARE_ART_SIZE];
    tft.setSwapBytes(true);
    for (int dy = 0; dy < TARGET_SIZE; dy++) {
      int src_y = (dy * IMG_SRC_SIZE) / TARGET_SIZE;
      int src_idx = src_y * IMG_SRC_SIZE;
      for (int dx = 0; dx < TARGET_SIZE; dx++) {
        int src_x = (dx * IMG_SRC_SIZE) / TARGET_SIZE;
        line_buf[dx] = img[src_idx + src_x];
      }
      tft.pushImage(img_x, img_y + dy, TARGET_SIZE, 1, line_buf);
    }
    tft.setSwapBytes(false);
  } else {
    // Vinyl Mode
    int TARGET_SIZE = NP_VINYL_OUTER_RADIUS * 2;
    int radius_sq = NP_VINYL_OUTER_RADIUS * NP_VINYL_OUTER_RADIUS;
    int inner_sq = NP_VINYL_INNER_RADIUS * NP_VINYL_INNER_RADIUS;
    int img_x = center_x - TARGET_SIZE / 2;
    int img_y = center_y - TARGET_SIZE / 2;

    float s = fsin(current_rotation_angle);
    float c = fcos(current_rotation_angle);

    // Draw into screen. Since it animates every frame, drawing individual
    // pixels can be slow. We'll buffer line by line.
    for (int dy = 0; dy < TARGET_SIZE; dy++) {
      int cy = dy - TARGET_SIZE / 2;
      for (int dx = 0; dx < TARGET_SIZE; dx++) {
        int cx = dx - TARGET_SIZE / 2;
        int dist_sq = cx * cx + cy * cy;

        if (dist_sq <= radius_sq && dist_sq >= inner_sq) {
          // Inverse rotation applied to destination (cx, cy) to find source
          // pixel sx = cx * cos - cy * sin sy = cx * sin + cy * cos
          float src_cx = cx * c - cy * s;
          float src_cy = cx * s + cy * c;

          // Map back to 0..TARGET_SIZE bounds
          int idx_x = (int)(src_cx + TARGET_SIZE / 2.0f);
          int idx_y = (int)(src_cy + TARGET_SIZE / 2.0f);

          // Constrain bounds to avoid artifacts at edges
          if (idx_x < 0)
            idx_x = 0;
          if (idx_x >= TARGET_SIZE)
            idx_x = TARGET_SIZE - 1;
          if (idx_y < 0)
            idx_y = 0;
          if (idx_y >= TARGET_SIZE)
            idx_y = TARGET_SIZE - 1;

          int real_src_x = (idx_x * IMG_SRC_SIZE) / TARGET_SIZE;
          int real_src_y = (idx_y * IMG_SRC_SIZE) / TARGET_SIZE;

          if (to_sprite) {
            spr.drawPixel(img_x + dx, img_y + dy - sprite_y_offset,
                          img[real_src_y * IMG_SRC_SIZE + real_src_x]);
          } else {
            tft.drawPixel(img_x + dx, img_y + dy,
                          img[real_src_y * IMG_SRC_SIZE + real_src_x]);
          }
        } else if (dist_sq < inner_sq) {
          // Force the inner hole to black
          if (to_sprite) spr.drawPixel(img_x + dx, img_y + dy - sprite_y_offset, TFT_BLACK);
          else tft.drawPixel(img_x + dx, img_y + dy, TFT_BLACK);
        }
      }
    }
  }
}

static void draw_tonearm(bool to_sprite = false, int sprite_y_offset = 0) {
  if (np_show_square_art || current_track_info.duration_ms == 0)
    return;

  // Calculate angle based on track progress
  float progress_ratio =
      (float)current_track_info.progress_ms / current_track_info.duration_ms;
  if (progress_ratio > 1.0f)
    progress_ratio = 1.0f;

  // Linearly interpolate angle between start and end
  float current_angle_deg =
      NP_TONEARM_START_ANGLE +
      (NP_TONEARM_END_ANGLE - NP_TONEARM_START_ANGLE) * progress_ratio;

  // Convert angle to radians for math
  float angle_rad = current_angle_deg * 3.14159f / 180.0f;

  // Calculate needle endpoint from pivot (Standard polar mapping)
  int needle_x = NP_TONEARM_PIVOT_X + NP_TONEARM_LENGTH * cos(angle_rad);
  int needle_y = NP_TONEARM_PIVOT_Y + NP_TONEARM_LENGTH * sin(angle_rad);

  int p_x = NP_TONEARM_PIVOT_X;
  int p_y = NP_TONEARM_PIVOT_Y - sprite_y_offset;
  int n_x = needle_x;
  int n_y = needle_y - sprite_y_offset;

  if (!to_sprite) {
    // Partially clear the area where the tonearm sweeps that is OUTSIDE the vinyl
    tft.fillRect(160 + NP_VINYL_OUTER_RADIUS, NP_TONEARM_PIVOT_Y + 12, 100, 100, TFT_BLACK);
  }

  // Draw thick Tonearm rod dynamically based on thickness
  uint16_t arm_color = NP_TONEARM_COLOR;
  
  if (to_sprite) {
      for (int t = -(NP_TONEARM_THICKNESS / 2); t <= (NP_TONEARM_THICKNESS / 2); t++) {
          spr.drawLine(p_x + t, p_y, n_x + t, n_y, arm_color);
          spr.drawLine(p_x, p_y + t, n_x, n_y + t, arm_color);
      }
      spr.fillCircle(p_x, p_y, 11 + (NP_TONEARM_THICKNESS / 2), NP_TONEARM_BASE_COLOR);
      spr.fillCircle(p_x, p_y, 6 + (NP_TONEARM_THICKNESS / 2), arm_color);
      spr.fillCircle(p_x, p_y, 3, NP_TONEARM_BASE_CENTER_COLOR);
      
      // Draw tiny dot to signify the stylus
      spr.fillCircle(n_x, n_y, 3, NP_TONEARM_DOT_COLOR);
  } else {
      for (int t = -(NP_TONEARM_THICKNESS / 2); t <= (NP_TONEARM_THICKNESS / 2); t++) {
          tft.drawLine(p_x + t, p_y, n_x + t, n_y, arm_color);
          tft.drawLine(p_x, p_y + t, n_x, n_y + t, arm_color);
      }
      tft.fillCircle(p_x, p_y, 11 + (NP_TONEARM_THICKNESS / 2), NP_TONEARM_BASE_COLOR);
      tft.fillCircle(p_x, p_y, 6 + (NP_TONEARM_THICKNESS / 2), arm_color);
      tft.fillCircle(p_x, p_y, 3, NP_TONEARM_BASE_CENTER_COLOR);
      
      // Draw tiny dot to signify the stylus
      tft.fillCircle(n_x, n_y, 3, NP_TONEARM_DOT_COLOR);
  }
}

static bool np_needs_full_redraw = true;

static void draw_now_playing() {
  bool initial_draw = np_needs_full_redraw || track_info_updated;

  if (initial_draw) {
    tft.fillScreen(TFT_BLACK);

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

    np_needs_full_redraw = false;
    track_info_updated = false; // consume the flag
  }

  // We draw the artwork every frame during playback IF it's in vinyl mode
  // (If it's square mode, we only need to draw it once, but we'll draw it here
  // on initial toggle)
  static bool last_square_state = !np_show_square_art;
  static unsigned long last_vinyl_draw_time = 0;
  bool vinyl_draw_time = (millis() - last_vinyl_draw_time >= 33); // ~30fps cap

  if ((current_track_info.is_playing && !np_show_square_art && vinyl_draw_time) ||
      last_square_state != np_show_square_art || initial_draw) {
      
    if (current_track_info.is_playing && !np_show_square_art) {
        last_vinyl_draw_time = millis();
    }

    if (!np_show_square_art && current_track_info.local_album_idx >= 0) {
      // VINYL BUFFERED MODE: Fixes flicker and dramatically speeds up rendering
      spr.fillSprite(TFT_BLACK);
      
      // Draw white outline in sprite context
      spr.drawCircle(SCREEN_W / 2, NP_ART_CENTER_Y - 50, NP_VINYL_OUTER_RADIUS + 1, TFT_WHITE);
      
      drawLocalAlbumArt(SCREEN_W / 2, NP_ART_CENTER_Y,
                        current_track_info.local_album_idx, true, 50);
      draw_tonearm(true, 50);
      spr.pushSprite(0, 50);
    } else {
      if (current_track_info.local_album_idx >= 0) {
        drawLocalAlbumArt(SCREEN_W / 2, NP_ART_CENTER_Y,
                          current_track_info.local_album_idx, false, 0);
      } else {
        // TODO: In the future we need to implement rotation for JPEG decoder,
        // For now, if we don't have local cache, just draw it standard using JPEG
        jpeg_np.open("/sd_card_albums/nowplaying.jpg", npOpen, npClose, npRead,
                     npSeek, JPEGDraw_NowPlaying);
        // Just raw scale down, no rotation supported yet natively in jpeglib
        int w = jpeg_np.getWidth();
        int scale =
            (w >= 400) ? JPEG_SCALE_QUARTER : ((w >= 200) ? JPEG_SCALE_HALF : 0);
        np_img_x = (SCREEN_W / 2) - (w / 2);
        np_img_y = NP_ART_CENTER_Y - (jpeg_np.getHeight() / 2);
        jpeg_np.decode(0, 0, scale);
        jpeg_np.close();
      }
      // Draw Tonearm AFTER the vinyl image is fully committed to the screen
      draw_tonearm(false, 0);
    }
    
    last_square_state = np_show_square_art;
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
  // CRITICAL: Create sprite FIRST — it's the largest allocation (89.6KB)
  // and must succeed for any display to work
  if (!sprite_created) {
    spr.setColorDepth(16);
    if (spr.createSprite(SCREEN_W, SPRITE_H) == nullptr) {
      Serial.println("SPRITE ALLOCATION FAILED");
    } else {
      sprite_created = true;
      Serial.println("Sprite created OK");
    }
  }

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
  if (sprite_created) {
    spr.deleteSprite();
    sprite_created = false;
  }
}

void ui_resume_sprite() {
  if (!sprite_created) {
    if (spr.createSprite(SCREEN_W, SPRITE_H) != nullptr) {
      sprite_created = true;
      // We don't draw immediately; the next loop cycle calling ui_update() will handle redrawing
    } else {
      Serial.println("FAILED TO RESUME SPRITE! OUT OF RAM!");
    }
  }
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
    // Animate vinyl spinning approx 10 seconds per rotation
    current_rotation_angle += (dt * 0.000628f);
    if (current_rotation_angle >= 6.28318f)
      current_rotation_angle -= 6.28318f;

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
      } else if (current_view == VIEW_NOW_PLAYING) {
        if (totalDrag < 6 && duration < 300) {
          // Check if tap was strictly inside the album art approx box
          if (tx >= 90 && tx <= 230 && ty >= 45 && ty <= 185) {
            np_show_square_art = !np_show_square_art;
            np_needs_full_redraw = true; // Clear the old art mask
          }
        }
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
    draw_album_browser(); // Force an immediate redraw of the browser
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