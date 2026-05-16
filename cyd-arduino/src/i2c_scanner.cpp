// I2C scanner — diagnostic-only build target.
// Builds standalone via `pio run -e scanner -t upload -t monitor`.
// Excluded from the esp32dev (main project) build via build_src_filter.
//
// Purpose: prove whether the MCP23017 is reachable at all without any of the
// main project's Adafruit / TFT / WiFi / Spotify code in the way.

#include <Arduino.h>
#include <Wire.h>

#define MCP_SDA_PIN 27
#define MCP_SCL_PIN 22
#define I2C_FREQ    100000

static int scan_pass = 0;

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== I2C scanner ===");
    Serial.printf("SDA=GPIO%d  SCL=GPIO%d  clock=%d Hz\n",
                  MCP_SDA_PIN, MCP_SCL_PIN, I2C_FREQ);
    Wire.begin(MCP_SDA_PIN, MCP_SCL_PIN);
    Wire.setClock(I2C_FREQ);
}

void loop() {
    scan_pass++;
    Serial.printf("\n--- scan #%d ---\n", scan_pass);

    int found = 0;
    for (uint8_t addr = 1; addr < 127; addr++) {
        Wire.beginTransmission(addr);
        uint8_t err = Wire.endTransmission();
        if (err == 0) {
            Serial.printf("  found device at 0x%02X%s\n",
                          addr, addr == 0x20 ? "   <-- MCP23017 expected" : "");
            found++;
        }
    }
    if (found == 0) Serial.println("  no devices responded");
    Serial.printf("(total: %d)\n", found);

    delay(2000);
}
