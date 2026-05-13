#include "mcp_input.h"
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_MCP23X17.h>

// ── Hardware constants ─────────────────────────────────────────────────────
#define MCP_ADDR      0x20   // A0=A1=A2=GND
#define MCP_SDA_PIN   27
#define MCP_SCL_PIN   22
#define MCP_INTA_PIN  35     // INTA → GPIO35 (input-only, push-pull driver on MCP)
#define I2C_FREQ      100000

// All inputs on Port A (pins 0-6), active LOW with internal pull-ups.
// Buttons: SW1=GPA0, SW2=GPA1, SW3=GPA2, SW4=GPA3
// RE1 encoder: CLK=GPA4, DT=GPA5, SW=GPA6
#define PIN_SW1      0
#define PIN_SW2      1
#define PIN_SW3      2
#define PIN_SW4      3
#define PIN_RE1_CLK  4   // GPA4
#define PIN_RE1_DT   5   // GPA5
#define PIN_RE1_SW   6   // GPA6

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

static BtnState btns[4];  // SW1-SW4
static BtnState re1_sw;

// ── Internal helpers ───────────────────────────────────────────────────────
static void _update_encoder(EncState &enc, uint8_t porta,
                             uint8_t clk_bit, uint8_t dt_bit)
{
    uint8_t clk = (porta >> clk_bit) & 1;
    uint8_t dt  = (porta >> dt_bit)  & 1;
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
        _update_btn(btns[i], !((porta >> i) & 1));  // active LOW, SW1=bit0..SW4=bit3
    }
}

static void _process_encoders(uint8_t porta)
{
    // RE1 — CLK=GPA4 (bit4), DT=GPA5 (bit5)
    _update_encoder(re1, porta, 4, 5);

    // Encoder push-switch (active LOW).
    // Guard: only register SW press when CLK and DT are both HIGH (encoder at rest).
    // When the shared GND contact is active during rotation it drags SW LOW too —
    // this rejects those false triggers.
    bool re1_still = ((porta >> 4) & 1) && ((porta >> 5) & 1);
    _update_btn(re1_sw, !((porta >> 6) & 1) && re1_still);
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

    // All active pins (PA0-PA6): INPUT_PULLUP
    for (uint8_t p = PIN_SW1; p <= PIN_RE1_SW; p++) {
        mcp.pinMode(p, INPUT_PULLUP);
    }

    // INTA for Port A: no mirroring (all inputs on one port), push-pull, active LOW.
    // Interrupt-on-change for all active pins so any press or encoder step fires INTA.
    mcp.setupInterrupts(false, false, LOW);
    for (uint8_t p = PIN_SW1; p <= PIN_RE1_SW; p++) {
        mcp.setupInterruptPin(p, CHANGE);
    }

    // Clear any pending interrupt at startup
    mcp.readGPIOAB();
    if (digitalRead(MCP_INTA_PIN) == LOW) {
        mcp.readGPIOAB();
    }

    // GPIO35: input-only, no pull-up needed (INTA is push-pull)
    pinMode(MCP_INTA_PIN, INPUT);

    // Seed encoder last-state from actual pin levels to avoid a false edge on boot
    uint8_t porta = mcp.readGPIO(0);
    re1.last_ab = (((porta >> 4) & 1) << 1) | ((porta >> 5) & 1);

    Serial.println("MCP23017 ready (SDA=27, SCL=22, INTA=35)");
}

// ── Debug helpers ──────────────────────────────────────────────────────────
// Define MCP_DEBUG to re-enable verbose serial output during hardware bring-up.
// #define MCP_DEBUG

#ifdef MCP_DEBUG
static void _dbg_print_bits(const char* label, uint8_t val) {
    Serial.printf("%s=0x%02X (", label, val);
    for (int i = 7; i >= 0; i--) Serial.print((val >> i) & 1);
    Serial.print(") ");
}
#endif

void mcp_input_update()
{
    if (!mcp_ok) {
        static bool once = true;
        if (once) { Serial.println("[MCP] ERROR: mcp_ok=false, all input disabled"); once = false; }
        return;
    }

    static uint8_t last_porta = 0xFF;

    bool inta_low = (digitalRead(MCP_INTA_PIN) == LOW);

    if (inta_low) {
        // Interrupt fired — read and clear immediately. Handles both encoder edges
        // and button presses since all PA0-PA6 have interrupt-on-change enabled.
        uint8_t porta = mcp.readGPIO(0);

#ifdef MCP_DEBUG
        Serial.print("[INTA] ");
        _dbg_print_bits("PA", porta);
        Serial.printf("SW1=%d SW2=%d SW3=%d SW4=%d RE1_CLK=%d RE1_DT=%d RE1_SW=%d\n",
                      !((porta >> 0) & 1), !((porta >> 1) & 1),
                      !((porta >> 2) & 1), !((porta >> 3) & 1),
                      (porta >> 4) & 1, (porta >> 5) & 1, (porta >> 6) & 1);
#endif

        if (porta != last_porta) last_porta = porta;

        _process_encoders(porta);
        _process_buttons(porta);
    } else {
        // No interrupt — poll Port A to keep debounce timers ticking and catch
        // any button state that didn't produce a clean interrupt edge.
        uint8_t porta = mcp.readGPIO(0);
        if (porta != last_porta) {
#ifdef MCP_DEBUG
            Serial.printf("[POLL] PA changed: SW1=%d SW2=%d SW3=%d SW4=%d (raw=0x%02X)\n",
                          !((porta >> 0) & 1), !((porta >> 1) & 1),
                          !((porta >> 2) & 1), !((porta >> 3) & 1), porta);
#endif
            last_porta = porta;
        }
        _process_buttons(porta);
    }

    // Periodic heartbeat every 2 s
    static unsigned long _hb_ms = 0;
    if (millis() - _hb_ms > 2000) {
        _hb_ms = millis();
#ifdef MCP_DEBUG
        uint8_t snap = mcp.readGPIO(0);
        Serial.printf("[HB  ] PA=0x%02X INTA=%d RE1cnt=%d\n",
                      snap, digitalRead(MCP_INTA_PIN), re1.count);
#endif
        _hb_ms = millis();
    }

#ifdef MCP_DEBUG
    if (re1_sw.event_pending) Serial.println("[EVT ] RE1-SW pressed (view toggle)");
    for (uint8_t i = 0; i < 4; i++)
        if (btns[i].event_pending) Serial.printf("[EVT ] SW%d pressed\n", i + 1);
    if (re1.count != 0) Serial.printf("[ENC ] RE1 accumulated delta=%d\n", re1.count);
#endif
}

int32_t re1_get_delta()
{
    int32_t v = re1.count;
    re1.count = 0;
    return v;
}

int32_t re2_get_delta()   { return 0; }  // RE2 not fitted

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
bool re2_sw_get_event() { return false; }  // RE2 not fitted
