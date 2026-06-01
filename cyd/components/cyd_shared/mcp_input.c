#include "mcp_input.h"

#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

/* ── Hardware constants ───────────────────────────────────────────────── */
#define MCP_ADDR      0x20
#define MCP_SDA       GPIO_NUM_27
#define MCP_SCL       GPIO_NUM_22
#define MCP_INTA_PIN  GPIO_NUM_35
#define I2C_FREQ_HZ   100000

/* MCP23017 registers (IOCON.BANK=0 default) */
#define REG_IODIRA    0x00
#define REG_GPINTENA  0x04
#define REG_IOCON     0x0A
#define REG_GPPUA     0x0C
#define REG_GPIOA     0x12

/* All inputs on Port A (active LOW with internal pull-ups):
 *   SW1=GPA0  SW2=GPA1  SW3=GPA2  SW4=GPA3
 *   RE1_CLK=GPA4  RE1_DT=GPA5  RE1_SW=GPA6 */
#define BTN_DEBOUNCE_MS 30
/* Minimum gap between Port A reads. Faster is wasted I2C bus traffic -- the
 * encoder gray-code state machine only needs one read per real transition
 * (~8 ms even on fast spins). */
#define MCP_MIN_READ_MS 2
/* If the encoder hasn't moved for this long, drop it back to rest so any
 * partial gray-code state can't carry into the next turn as a stale glitch. */
#define ENC_REST_TIMEOUT_MS 1000

static const char *TAG = "mcp";

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;
static bool                    s_ok  = false;

/* ── Gray-code state machine (matches PlatformIO mcp_input.cpp) ─────── */
enum { S_REST=0, S_CW1, S_CW2, S_CW3, S_CCW1, S_CCW2, S_CCW3 };

typedef struct {
    uint8_t  state;
    int32_t  count;
    uint32_t last_change_ms;
} enc_state_t;

typedef struct {
    bool     last_raw;
    bool     pressed;
    bool     event_pending;
    uint32_t last_change_ms;
} btn_state_t;

static enc_state_t s_re1    = {S_REST, 0, 0};
static btn_state_t s_btns[4] = {0};
static btn_state_t s_re1_sw  = {0};

static uint32_t _millis(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static esp_err_t _write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_dev, buf, 2, 10);
}

static int _read_porta(void)
{
    uint8_t reg = REG_GPIOA;
    uint8_t val = 0xFF;
    if (i2c_master_transmit_receive(s_dev, &reg, 1, &val, 1, 10) != ESP_OK)
        return -1;
    return val;
}

static void _update_encoder(enc_state_t *enc, uint8_t porta,
                             uint8_t clk_bit, uint8_t dt_bit)
{
    uint8_t ab = (((porta >> clk_bit) & 1) << 1) | ((porta >> dt_bit) & 1);
    uint8_t next = enc->state;
    int8_t  emit = 0;

    switch (enc->state) {
        case S_REST:
            if      (ab == 0x01) next = S_CW1;
            else if (ab == 0x02) next = S_CCW1;
            break;
        case S_CW1:
            if      (ab == 0x00) next = S_CW2;
            else if (ab == 0x03) next = S_REST;
            break;
        case S_CW2:
            if      (ab == 0x02) next = S_CW3;
            else if (ab == 0x01) next = S_CW1;
            break;
        case S_CW3:
            if      (ab == 0x03) { emit = +1; next = S_REST; }
            else if (ab == 0x00) next = S_CW2;
            break;
        case S_CCW1:
            if      (ab == 0x00) next = S_CCW2;
            else if (ab == 0x03) next = S_REST;
            break;
        case S_CCW2:
            if      (ab == 0x01) next = S_CCW3;
            else if (ab == 0x02) next = S_CCW1;
            break;
        case S_CCW3:
            if      (ab == 0x03) { emit = -1; next = S_REST; }
            else if (ab == 0x00) next = S_CCW2;
            break;
    }

    if (next != enc->state) {
        enc->state = next;
        enc->last_change_ms = _millis();
    }
    enc->count += emit;
}

static void _update_btn(btn_state_t *b, bool raw_active)
{
    /* event_pending is a consume-on-read latch (cleared by btn_get_event /
     * re1_sw_get_event). Do NOT clear it per tick here -- the consumer runs
     * under the LVGL lock (which can time out mid-animation), and clearing
     * here would silently drop a press confirmed on a tick where the consumer
     * didn't get the lock. Matches the Arduino driver's documented semantics. */
    if (raw_active != b->last_raw) {
        b->last_raw = raw_active;
        b->last_change_ms = _millis();
    }
    if (_millis() - b->last_change_ms > BTN_DEBOUNCE_MS) {
        if (raw_active != b->pressed) {
            b->pressed = raw_active;
            if (raw_active) b->event_pending = true;
        }
    }
}

/* ── Public API ──────────────────────────────────────────────────────── */

/* Probe the chip + write its config registers. Split out of mcp_input_init so
 * mcp_input_update can retry it if the chip wasn't responsive at boot
 * (transient I2C glitch, slow-rising pull-ups, momentary unplug, etc.) without
 * re-doing the one-time bus/GPIO setup. Idempotent: all writes are absolute. */
static bool _configure_mcp(void)
{
    /* Probe: write IOCON (0x00 = default, push-pull active-LOW) */
    uint8_t probe[2] = {REG_IOCON, 0x00};
    if (i2c_master_transmit(s_dev, probe, 2, 50) != ESP_OK) return false;
    _write_reg(REG_IODIRA,   0xFF);  /* all inputs */
    _write_reg(REG_GPPUA,    0x7F);  /* pullups on bits 0-6 */
    _write_reg(REG_GPINTENA, 0x7F);  /* interrupt-on-change bits 0-6 */
    _read_porta();                   /* clear any pending interrupt */
    return true;
}

void mcp_input_init(void)
{
    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << MCP_INTA_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_cfg);

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port            = I2C_NUM_0,
        .sda_io_num          = MCP_SDA,
        .scl_io_num          = MCP_SCL,
        .clk_source          = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt   = 7,
        .flags.enable_internal_pullup = false,
    };
    if (i2c_new_master_bus(&bus_cfg, &s_bus) != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed -- mcp inputs disabled");
        return;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = MCP_ADDR,
        .scl_speed_hz    = I2C_FREQ_HZ,
    };
    if (i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev) != ESP_OK) {
        ESP_LOGE(TAG, "MCP23017 add device failed -- mcp inputs disabled");
        return;
    }

    if (_configure_mcp()) {
        s_ok = true;
        ESP_LOGI(TAG, "MCP23017 ready (SDA=%d SCL=%d INTA=%d)",
                 (int)MCP_SDA, (int)MCP_SCL, (int)MCP_INTA_PIN);
    } else {
        /* Not a hard failure -- mcp_input_update keeps retrying so a chip that
         * comes online late (or after a transient bus issue) is picked up
         * without needing a reboot. */
        ESP_LOGW(TAG, "MCP23017 not responding at 0x%02X -- will keep retrying",
                 MCP_ADDR);
    }
}

void mcp_input_update(void)
{
    if (!s_ok) {
        /* Retry the config every 5 s so a chip absent / unresponsive at boot
         * (loose wire, brown-out during init) doesn't leave physical controls
         * permanently dead. Re-doing _configure_mcp is safe: it's idempotent. */
        if (!s_dev) return;   /* bus/dev setup failed -- nothing to retry against */
        static uint32_t s_last_reinit_ms = 0;
        uint32_t now = _millis();
        if (now - s_last_reinit_ms < 5000) return;
        s_last_reinit_ms = now;
        if (_configure_mcp()) {
            s_ok = true;
            ESP_LOGI(TAG, "MCP23017 came online on retry");
        }
        if (!s_ok) return;
    }

    static uint32_t s_last_read_ms = 0;
    if (_millis() - s_last_read_ms < MCP_MIN_READ_MS) return;

    int p = _read_porta();
    s_last_read_ms = _millis();
    if (p < 0) return;

    uint8_t porta = (uint8_t)p;

    _update_encoder(&s_re1, porta, 4, 5);

    for (uint8_t i = 0; i < 4; i++)
        _update_btn(&s_btns[i], !((porta >> i) & 1));
    _update_btn(&s_re1_sw, !((porta >> 6) & 1));

    if (s_re1.state != S_REST && (_millis() - s_re1.last_change_ms) > ENC_REST_TIMEOUT_MS)
        s_re1.state = S_REST;
}

int32_t re1_get_delta(void)
{
    int32_t v = s_re1.count;
    s_re1.count = 0;
    return v;
}

bool btn_get_event(uint8_t i)
{
    if (i >= 4) return false;
    bool e = s_btns[i].event_pending;
    s_btns[i].event_pending = false;
    return e;
}

bool btn_is_held(uint8_t i)
{
    if (i >= 4) return false;
    return s_btns[i].pressed;
}

bool re1_sw_get_event(void)
{
    bool e = s_re1_sw.event_pending;
    s_re1_sw.event_pending = false;
    return e;
}

bool mcp_input_has_pending(void)
{
    if (s_re1.count != 0) return true;
    if (s_re1_sw.event_pending) return true;
    for (int i = 0; i < 4; i++) {
        if (s_btns[i].event_pending || s_btns[i].pressed) return true;
    }
    return false;
}
