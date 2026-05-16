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
// State machine decoder. A physical click traverses the gray-code cycle
//   rest(11) → 01 → 00 → 10 → rest(11)   (CW)
//   rest(11) → 10 → 00 → 01 → rest(11)   (CCW)
// We emit ±1 only on completing the full traversal back to rest. Any deviation
// (skipped state, bounce back toward rest, invalid 11→00 jump) sends us back
// to S_REST without emitting. This is far more noise-tolerant than counting
// each sub-step, which a flaky encoder can over-emit by ramping subcount past
// the threshold via clean-but-spurious transitions.
enum {
    S_REST       = 0,  // resting at AB=11
    S_CW1,             // saw rest→01
    S_CW2,             // saw 01→00
    S_CW3,             // saw 00→10 — next AB=11 emits +1
    S_CCW1,            // saw rest→10
    S_CCW2,            // saw 10→00
    S_CCW3,            // saw 00→01 — next AB=11 emits -1
};

struct EncState {
    uint8_t       state;          // S_REST / S_CWx / S_CCWx
    int32_t       count;          // accumulated detents (callers see ±1 per click)
    unsigned long last_change_ms; // last time AB observably changed
};
static EncState re1 = {S_REST, 0, 0};

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
    uint8_t ab = (((porta >> clk_bit) & 1) << 1) | ((porta >> dt_bit) & 1);

    uint8_t next = enc.state;
    int8_t  emit = 0;

    switch (enc.state) {
        case S_REST:
            if      (ab == 0b01) next = S_CW1;
            else if (ab == 0b10) next = S_CCW1;
            // ab == 11 or 00: stay at rest
            break;
        case S_CW1:
            if      (ab == 0b00) next = S_CW2;
            else if (ab == 0b11) next = S_REST;   // backed out
            break;
        case S_CW2:
            if      (ab == 0b10) next = S_CW3;
            else if (ab == 0b01) next = S_CW1;    // proper step-by-step backout
            // ab == 0b11 here would be an impossible 00→11 jump — almost
            // certainly a contact glitch. Stay in place rather than aborting
            // the click. Only a real backout (00→01→11) reaches REST.
            break;
        case S_CW3:
            if      (ab == 0b11) { emit = +1; next = S_REST; }
            else if (ab == 0b00) next = S_CW2;    // proper step-by-step backout
            // ab == 0b01 here is a 10→01 jump — glitch, ignore.
            break;
        case S_CCW1:
            if      (ab == 0b00) next = S_CCW2;
            else if (ab == 0b11) next = S_REST;
            break;
        case S_CCW2:
            if      (ab == 0b01) next = S_CCW3;
            else if (ab == 0b10) next = S_CCW1;
            // ab == 0b11 here would be an impossible 00→11 jump — glitch,
            // ignore (same reasoning as S_CW2).
            break;
        case S_CCW3:
            if      (ab == 0b11) { emit = -1; next = S_REST; }
            else if (ab == 0b00) next = S_CCW2;
            break;
    }

    if (next != enc.state) {
        enc.state = next;
        enc.last_change_ms = millis();
    }
    enc.count += emit;
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
    // RE1 push-switch (GPA6, active LOW). Treated as a regular button now —
    // the previous "encoder must be quiet" guard rejected presses whenever the
    // encoder was noisy (which on a flaky module is most of the time). The
    // 30 ms button debounce in _update_btn handles ordinary contact bounce.
    _update_btn(re1_sw, !((porta >> 6) & 1));
}

static void _process_encoders(uint8_t porta)
{
    // RE1 — CLK=GPA4 (bit4), DT=GPA5 (bit5)
    _update_encoder(re1, porta, 4, 5);

    // Watchdog: if the state machine has been parked in a non-rest state for
    // over a second with no transitions, the encoder is probably just sitting
    // at AB=11 while we mistakenly think we're mid-click. Silently reset so
    // the next click starts fresh instead of being lost.
    if (re1.state != S_REST && (millis() - re1.last_change_ms) > 1000) {
        re1.state = S_REST;
    }
}

// Direct GPIOA read that returns -1 on I2C failure, so callers can skip
// processing instead of treating a failed read as "all pins HIGH" (which the
// Adafruit lib does — and which masquerades as "all buttons released" with
// our active-LOW wiring, silently breaking debounce).
// GPIOA register is 0x12 in IOCON.BANK=0 (the Adafruit default).
static int _read_porta_safe()
{
    Wire.beginTransmission(MCP_ADDR);
    Wire.write(0x12);
    if (Wire.endTransmission(false) != 0) return -1;
    if (Wire.requestFrom((uint8_t)MCP_ADDR, (uint8_t)1) != 1) return -1;
    return Wire.read();
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

    // The state machine starts at S_REST; whatever AB the encoder is actually
    // at on boot will resolve naturally once rotation begins (S_REST ignores
    // AB=00 / AB=11 and only transitions on the first valid 01 or 10).

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
    static unsigned long last_read_ms = 0;

    // Throttle ALL I2C traffic — both INTA-driven and polled — to one read per
    // 2 ms. Originally added at 5 ms while the MCP solder joints were cold and
    // the bus was hammered → flood. With the chip resoldered (bus healthy), we
    // can poll faster so the encoder state machine catches every quadrature
    // transition; the previous 5 ms window could miss the brief 01/10 phases
    // of a fast click and look like a state jump. 2 ms = 500 reads/s, well
    // within bus capacity and still imperceptible compared to 30 ms button
    // debounce. Must apply to BOTH paths: when a read fails the MCP interrupt
    // isn't cleared, INTA stays LOW, and an INTA-only rate-limit gets bypassed
    // every iteration.
    if (millis() - last_read_ms < 2) return;

    bool inta_low = (digitalRead(MCP_INTA_PIN) == LOW);
    int p = _read_porta_safe();
    last_read_ms = millis();
    if (p < 0) return;  // skip on I2C failure — preserve debounce state
    uint8_t porta = (uint8_t)p;

    if (inta_low) {
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
    bool e = btns[i].event_pending;
    btns[i].event_pending = false;  // consume on read
    return e;
}

bool btn_is_held(uint8_t i)
{
    if (i >= 4) return false;
    return btns[i].pressed;
}

bool re1_sw_get_event()
{
    bool e = re1_sw.event_pending;
    re1_sw.event_pending = false;  // consume on read
    return e;
}
bool re2_sw_get_event() { return false; }  // RE2 not fitted
