#include "app.h"
#include "input.h"
#include "mcp_input.h"
#include "spotify.h"
#include "ui.h"
#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

// ============================================================
// USER CREDENTIALS - Fill in your details below before building
// ============================================================
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#define SPOTIFY_CLIENT_ID "YOUR_SPOTIFY_CLIENT_ID"
#define SPOTIFY_CLIENT_SECRET "YOUR_SPOTIFY_CLIENT_SECRET"
#define SPOTIFY_REFRESH_TOKEN "YOUR_SPOTIFY_REFRESH_TOKEN"
// ============================================================

TFT_eSPI tft = TFT_eSPI();
bool sd_ok = false;

// RE1 delta consumed by ui.cpp via this wrapper (keeps ui.cpp unchanged)
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

bool get_touch_coords(int16_t *x, int16_t *y) {
  if (!touch.tirqTouched() || !touch.touched()) {
    smooth_x = -1;
    smooth_y = -1;
    return false;
  }

  TS_Point p = touch.getPoint();
  touch_pressure = p.z;

  int16_t mapped_x = map(p.x, 200, 3800, 0, 319);
  int16_t mapped_y = map(p.y, 200, 3800, 0, 239);
  mapped_x = constrain(mapped_x, 0, 319);
  mapped_y = constrain(mapped_y, 0, 239);

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

void setup() {
  Serial.begin(115200);

  tft.begin();
  tft.setRotation(3);
  tft.fillScreen(TFT_BLACK);

  touchSpi.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  touch.begin(touchSpi);
  touch.setRotation(3);

  if (SD.begin(5)) {
    sd_ok = true;
    Serial.println("SD card mounted OK");
  } else {
    Serial.println("SD card mount FAILED (continuing without)");
  }

  mcp_input_init();

  ui_init();
  app_init();

  if (String(WIFI_SSID) != "YOUR_WIFI_SSID") {
    spotify_init(WIFI_SSID, WIFI_PASSWORD, SPOTIFY_CLIENT_ID,
                 SPOTIFY_CLIENT_SECRET, SPOTIFY_REFRESH_TOKEN);
  } else {
    Serial.println("WiFi skipped (placeholder credentials)");
  }
}

void loop() {
  mcp_input_update();
  input_update();
  spotify_update();
  ui_update();

  static unsigned long last_debug = 0;
  if (millis() - last_debug > 500) {
    last_debug = millis();
    int16_t dummy_x, dummy_y;
    bool t = get_touch_coords(&dummy_x, &dummy_y);
    Serial.print("vol=");
    Serial.print(current_volume_pct);
    Serial.print(" T=");
    Serial.println(t);
  }
}
