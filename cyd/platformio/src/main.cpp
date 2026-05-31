#include "input.h"
#include "mcp_input.h"
#include "spotify.h"
#include "ui.h"
#include "lvgl_app.h"
#include "secrets.h"

#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <lvgl.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define SCREEN_W 320
#define SCREEN_H 240

// Landscape. NOTE: the ESP-IDF build runs 180-degrees flipped from this
// (USB on the left). To match it, set both this and the touch mapping to the
// flipped orientation -- deferred until the LVGL port is confirmed booting so
// bring-up isn't debugging orientation and the port at the same time.
#define DISPLAY_ROTATION 1

TFT_eSPI tft = TFT_eSPI();

// RE1 delta consumed by legacy callers via this wrapper.
int32_t get_encoder_delta() { return re1_get_delta(); }

// --- CYD Custom Touch Pins ---
#define XPT2046_IRQ  36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33

SPIClass touchSpi = SPIClass(HSPI);
XPT2046_Touchscreen touch(XPT2046_CS, XPT2046_IRQ);

static int16_t smooth_x = -1, smooth_y = -1;
int16_t touch_pressure = 0;

// Raw XPT2046 read -> screen coordinates with a 3:1 IIR smooth. Unchanged from
// the pre-LVGL build so touch stays calibrated; consumed by the LVGL indev cb.
bool get_touch_coords(int16_t *x, int16_t *y) {
  if (!touch.tirqTouched() || !touch.touched()) {
    smooth_x = -1;
    smooth_y = -1;
    return false;
  }

  TS_Point p = touch.getPoint();
  touch_pressure = p.z;

  int16_t mapped_x = map(p.x, 200, 3800, 0, SCREEN_W - 1);
  int16_t mapped_y = map(p.y, 200, 3800, 0, SCREEN_H - 1);
  mapped_x = constrain(mapped_x, 0, SCREEN_W - 1);
  mapped_y = constrain(mapped_y, 0, SCREEN_H - 1);

  if (smooth_x < 0) {
    smooth_x = mapped_x;
    smooth_y = mapped_y;
  } else {
    smooth_x = (smooth_x * 3 + mapped_x) / 4;
    smooth_y = (smooth_y * 3 + mapped_y) / 4;
  }

  *x = smooth_x;
  *y = smooth_y;
  return true;
}

// ── LVGL glue ──────────────────────────────────────────────────────────────
// Single partial draw buffer of 40 lines (320*40*2 = 25.6 KB). Heap-allocated
// rather than static so it doesn't inflate .dram0.bss at link time.
#define LVBUF_LINES 40
#define LVBUF_BYTES (SCREEN_W * LVBUF_LINES * 2)
static uint8_t *s_lvbuf = NULL;
static lv_display_t *s_disp = NULL;

// Recursive so a render-loop callback can re-enter via a ui_* helper.
static SemaphoreHandle_t s_lvgl_mux = NULL;
void lvgl_lock()   { if (s_lvgl_mux) xSemaphoreTakeRecursive(s_lvgl_mux, portMAX_DELAY); }
void lvgl_unlock() { if (s_lvgl_mux) xSemaphoreGiveRecursive(s_lvgl_mux); }

static uint32_t lv_tick_cb(void) { return millis(); }

static void disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;
  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushColors((uint16_t *)px_map, w * h, true);  // swap bytes for RGB565
  tft.endWrite();
  lv_display_flush_ready(disp);
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
  (void)indev;
  int16_t x, y;
  if (get_touch_coords(&x, &y)) {
    data->state   = LV_INDEV_STATE_PRESSED;
    data->point.x = x;
    data->point.y = y;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

static void lvgl_glue_init() {
  lv_init();
  lv_tick_set_cb(lv_tick_cb);

  s_lvbuf = (uint8_t *)malloc(LVBUF_BYTES);
  if (!s_lvbuf) {
    Serial.println("FATAL: LVGL draw buffer alloc failed");
    while (true) delay(1000);  // can't render without it; halt loudly
  }

  s_disp = lv_display_create(SCREEN_W, SCREEN_H);
  lv_display_set_flush_cb(s_disp, disp_flush);
  lv_display_set_buffers(s_disp, s_lvbuf, NULL, LVBUF_BYTES,
                         LV_DISPLAY_RENDER_MODE_PARTIAL);

  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, touch_read_cb);
}

// ── Typed Spotify command queue ──────────────────────────────────────────────
// Mirrors the IDF build: ui_request_*() post here from the render/input context
// and spotify_task drains them, so the blocking HTTPS never stalls rendering.
typedef enum {
  SCMD_PLAY_ALBUM, SCMD_TOGGLE_PLAY,  SCMD_PREV_TRACK,
  SCMD_NEXT_TRACK, SCMD_SEEK_MS,      SCMD_SET_VOLUME,
  SCMD_TOGGLE_SHUFFLE,
} scmd_type_t;

typedef struct {
  scmd_type_t type;
  uint32_t    param;  // seek_ms or volume_pct
  const char *uri;    // SCMD_PLAY_ALBUM only; points into albums.cpp .rodata
} scmd_t;

static QueueHandle_t s_cmd_queue = NULL;

static void post_cmd(scmd_type_t type, uint32_t param, const char *uri) {
  if (!s_cmd_queue) return;
  scmd_t cmd = { type, param, uri };
  xQueueSend(s_cmd_queue, &cmd, 0);
}

void ui_request_play(const char *uri) { post_cmd(SCMD_PLAY_ALBUM,      0,             uri); }
void ui_request_toggle_play()         { post_cmd(SCMD_TOGGLE_PLAY,    0,             nullptr); }
void ui_request_prev()                { post_cmd(SCMD_PREV_TRACK,     0,             nullptr); }
void ui_request_next()                { post_cmd(SCMD_NEXT_TRACK,     0,             nullptr); }
void ui_request_seek(uint32_t ms)     { post_cmd(SCMD_SEEK_MS,        ms,            nullptr); }
void ui_request_volume(int pct)       { post_cmd(SCMD_SET_VOLUME,     (uint32_t)pct, nullptr); }
void ui_request_shuffle()             { post_cmd(SCMD_TOGGLE_SHUFFLE, 0,             nullptr); }

static void dispatch_cmd(const scmd_t &c) {
  switch (c.type) {
    case SCMD_PLAY_ALBUM:      spotify_play_album(c.uri);               break;
    case SCMD_TOGGLE_PLAY:     spotify_toggle_play_pause();             break;
    case SCMD_PREV_TRACK:      spotify_prev_track();                    break;
    case SCMD_NEXT_TRACK:      spotify_next_track();                    break;
    case SCMD_SEEK_MS:         spotify_seek_position((int32_t)c.param); break;
    case SCMD_SET_VOLUME:      spotify_set_volume((int)c.param);        break;
    case SCMD_TOGGLE_SHUFFLE:  spotify_toggle_shuffle();                break;
  }
}

// ── Tasks ────────────────────────────────────────────────────────────────────
static void spotify_task(void *arg) {
  (void)arg;
  spotify_init(WIFI_SSID, WIFI_PASSWORD, SPOTIFY_CLIENT_ID,
               SPOTIFY_CLIENT_SECRET, SPOTIFY_REFRESH_TOKEN);
  for (;;) {
    spotify_update();  // token refresh + player poll (self-paced, blocking)

    if (track_info_updated) {
      lvgl_lock();
      ui_set_track_info(&current_track_info);
      lvgl_unlock();
      track_info_updated = false;
    }

    // Block briefly for a command so presses dispatch promptly; drain the rest.
    scmd_t c;
    if (xQueueReceive(s_cmd_queue, &c, pdMS_TO_TICKS(50)) == pdTRUE) {
      dispatch_cmd(c);
      while (xQueueReceive(s_cmd_queue, &c, 0) == pdTRUE) dispatch_cmd(c);
    }
  }
}

// MCP polling on core 0, decoupled from the render loop (core 1). Touches only
// the MCP driver's own state. mcp_input_update() self-throttles to ~2 ms.
static void mcp_input_task(void *arg) {
  (void)arg;
  for (;;) {
    mcp_input_update();
    vTaskDelay(1);
  }
}

void setup() {
  Serial.begin(115200);

  tft.begin();
  tft.setRotation(DISPLAY_ROTATION);
  tft.fillScreen(TFT_BLACK);

  touchSpi.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  touch.begin(touchSpi);
  touch.setRotation(DISPLAY_ROTATION);

  s_lvgl_mux = xSemaphoreCreateRecursiveMutex();
  lvgl_glue_init();

  mcp_input_init();
  input_init();

  ui_init();

  s_cmd_queue = xQueueCreate(8, sizeof(scmd_t));

  // Input polling on core 0; render loop stays on core 1.
  xTaskCreatePinnedToCore(mcp_input_task, "mcp_input", 4096, NULL, 2, NULL, 0);

  if (String(WIFI_SSID) != "YOUR_WIFI_SSID") {
    // Core 0 (with WiFi/lwip), NOT core 1: the TLS handshake is CPU-bound and
    // runs at higher priority than the render loop, so keeping it off core 1
    // stops it from freezing the UI during connects/reconnects. 16 KB stack --
    // mbedTLS handshake + ArduinoJson parsing are both stack-hungry.
    xTaskCreatePinnedToCore(spotify_task, "spotify", 16384, NULL, 3, NULL, 0);
  } else {
    Serial.println("WiFi skipped (placeholder credentials)");
  }
}

void loop() {
  // Render under the lock (excludes the Spotify task during a flush); input
  // dispatch runs outside it (its ui_* calls take the lock individually and
  // only enqueue commands, so they never block here).
  lvgl_lock();
  lv_timer_handler();
  lvgl_unlock();

  input_update();

  delay(5);
}
