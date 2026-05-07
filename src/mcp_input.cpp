#include "mcp_input.h"
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_MCP23X17.h>

// ── Hardware constants ─────────────────────────────────────────────────────
#define MCP_ADDR      0x20   // A0=A1=A2=GND
#define MCP_SDA_PIN   27
#define MCP_SCL_PIN   22
#define MCP_INTB_PIN  35     // INTB → GPIO35 (input-only, push-pull driver on MCP)
#define I2C_FREQ      400000

// Port A (pins 0-7) — push buttons (active LOW)
#define PIN_SW1  0
#define PIN_SW2  1
#define PIN_SW3  2
#define PIN_SW4  3

// Port B (pins 8-15) — encoders + encoder switches (active LOW)
#define PIN_RE2_CLK  8   // GPB0
#define PIN_RE2_DT   9   // GPB1
#define PIN_RE2_SW   10  // GPB2
#define PIN_RE1_SW   11  // GPB3
#define PIN_RE1_DT   12  // GPB4
#define PIN_RE1_CLK  13  // GPB5

// Button debounce
#define BTN_DEBOUNCE_MS 30

// ── MCP instance ───────────────────────────────────────────────────────────
static Adafruit_MCP23X17 mcp;
static bool mcp_ok = false;

// ── Encoder state ──────────────────────────────────────────────────────────
struct EncState {
    uint8_t last_ab;  // previous {CLK, DT} 2-bit value
    int32_t count;
};
static EncState re1 = {0, 0};
static EncState re2 = {0, 0};

// Gray-code quadrature table: index = (last_ab << 2) | new_ab → delta
// Full-step: only ±1 per complete 4-state cycle; glitches produce 0
static const int8_t enc_table[16] = {
    0, -1,  1,  0,
    1,  0,  0, -1,
   -1,  0,  0,  1,
    0,  1, -1,  0
};

// ── Button / switch state ──────────────────────────────────────────────────
struct BtnState {
    bool last_raw;
    bool pressed;
    bool event_pending;
    unsigned long last_change_ms;
};

static BtnState btns[4];     // SW1-SW4
static BtnState re1_sw;
static BtnState re2_sw;

// ── Internal helpers ───────────────────────────────────────────────────────
static void _update_encoder(EncState &enc, uint8_t portb_byte,
                             uint8_t clk_bit, uint8_t dt_bit)
{
    uint8_t clk = (portb_byte >> clk_bit) & 1;
    uint8_t dt  = (portb_byte >> dt_bit)  & 1;
    uint8_t new_ab = (clk << 1) | dt;
    enc.count += enc_table[(enc.last_ab << 2) | new_ab];
    enc.last_ab = new_ab;
}

static void _update_btn(BtnState &b, bool raw_active)
{
    b.event_pending = false;
    if (raw_active != b.last_raw) {
        b.last_raw = raw_active;
        b.last_change_ms = millis();
    }
    if (millis() - b.last_change_ms > BTN_DEBOUNCE_MS) {
        if (raw_active != b.pressed) {
            b.pressed = raw_active;
            if (raw_active) b.event_pending = true;  // confirmed press edge
        }
    }
}

static void _process_buttons(uint8_t porta)
{
    for (uint8_t i = 0; i < 4; i++) {
        _update_btn(btns[i], !((porta >> i) & 1));  // active LOW → invert
    }
}

static void _process_encoders(uint8_t portb)
{
    // RE1 — CLK=bit5, DT=bit4 (Adafruit pins 13, 12 → GPIOB bits 5, 4)
    _update_encoder(re1, portb, 5, 4);
    // RE2 — CLK=bit0, DT=bit1 (Adafruit pins 8, 9 → GPIOB bits 0, 1)
    _update_encoder(re2, portb, 0, 1);

    // Encoder push-switches (active LOW)
    _update_btn(re1_sw, !((portb >> 3) & 1));  // GPB3 = bit3
    _update_btn(re2_sw, !((portb >> 2) & 1));  // GPB2 = bit2
}

// ── Public API ─────────────────────────────────────────────────────────────
void mcp_input_init()
{
    Wire.begin(MCP_SDA_PIN, MCP_SCL_PIN);
    Wire.setClock(I2C_FREQ);

    if (!mcp.begin_I2C(MCP_ADDR, &Wire)) {
        Serial.println("ERROR: MCP23017 not found — check wiring and I2C pull-ups");
        mcp_ok = false;
        return;
    }
    mcp_ok = true;

    // Port A: push buttons (pins 0-3) — input with pull-ups
    for (uint8_t p = PIN_SW1; p <= PIN_SW4; p++) {
        mcp.pinMode(p, INPUT_PULLUP);
    }

    // Port B: encoders + switches (pins 8-13) — input with pull-ups
    for (uint8_t p = PIN_RE2_CLK; p <= PIN_RE1_CLK; p++) {
        mcp.pinMode(p, INPUT_PULLUP);
    }

    // Configure INTB for Port B: separate ports, push-pull output, active LOW
    mcp.setupInterrupts(false, false, LOW);
    for (uint8_t p = PIN_RE2_CLK; p <= PIN_RE1_CLK; p++) {
        mcp.setupInterruptPin(p, CHANGE);
    }

    // Clear any pending interrupt at startup
    mcp.readGPIOAB();
    if (digitalRead(MCP_INTB_PIN) == LOW) {
        mcp.readGPIOAB();
    }

    // GPIO35: input-only, no pull-up needed (INTB is push-pull)
    pinMode(MCP_INTB_PIN, INPUT);

    // Seed encoder last-state from actual pin levels to avoid a false edge
    uint16_t ab = mcp.readGPIOAB();
    uint8_t portb = (ab >> 8) & 0xFF;
    re1.last_ab = (((portb >> 5) & 1) << 1) | ((portb >> 4) & 1);
    re2.last_ab = (((portb >> 0) & 1) << 1) | ((portb >> 1) & 1);

    Serial.println("MCP23017 ready (SDA=27, SCL=22, INTB=35)");
}

void mcp_input_update()
{
    if (!mcp_ok) return;

    if (digitalRead(MCP_INTB_PIN) == LOW) {
        // Port B changed — read both ports in one I2C transaction
        uint16_t ab = mcp.readGPIOAB();
        _process_encoders((ab >> 8) & 0xFF);
        _process_buttons(ab & 0xFF);
    } else {
        // Nothing changed on Port B; still poll buttons (Port A has no interrupt)
        _process_buttons(mcp.readGPIO(0) & 0xFF);
    }
}

int32_t re1_get_delta()
{
    int32_t v = re1.count;
    re1.count = 0;
    return v;
}

int32_t re2_get_delta()
{
    int32_t v = re2.count;
    re2.count = 0;
    return v;
}

bool btn_get_event(uint8_t i)
{
    if (i >= 4) return false;
    return btns[i].event_pending;
}

bool btn_is_held(uint8_t i)
{
    if (i >= 4) return false;
    return btns[i].pressed;
}

bool re1_sw_get_event() { return re1_sw.event_pending; }
bool re2_sw_get_event() { return re2_sw.event_pending; }
