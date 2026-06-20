/*
 * Copyright 2022 Scott Bezek (SmartKnob interface_task.cpp — protocol pattern,
 *   https://github.com/scottbez1/smartknob), Apache License, Version 2.0.
 * Copyright 2026 Lewis.
 *
 * Changes: HX711 full Wheatstone bridge (4-gauge) instead of 2-gauge; 4 MX
 * buttons added; SK6812 dual-chain via Adafruit NeoPixel; VEML7700 + MAX17048
 * reads added; UART target changed to match P4 pin assignment.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "interface_task.h"

#include <Arduino.h>
#include <HX711.h>
#include <Adafruit_NeoPixel.h>
#include <Adafruit_VEML7700.h>
#include <Adafruit_MAX1704X.h>
#include <Wire.h>

#include "motor_task.h"
#include "proto_gen/home_controller.pb.h"
#include <pb_encode.h>
#include <pb_decode.h>

// ---- Pin assignments ----
// UART to ESP32-P4 -- Serial2 = UART1 on RP2040.
// MUST be UART1, not UART0/Serial2: UART0's pins (GPIO0/1) are taken by the
// motor's U-phase PWM (MOTOR_UH_PIN/MOTOR_UL_PIN in motor_task.h), and a GPIO
// cannot be both PWM and UART. UART1 maps TX to {4,8,20,24} and RX to
// {5,9,21,25}; GPIO8/9 are the only free pair (4/5 = motor, 20/21 = LEDs).
#define KNOB_UART_TX   8
#define KNOB_UART_RX   9
#define KNOB_UART_BAUD 921600

// HX711 (BF350 full Wheatstone bridge)
#define HX711_DOUT_PIN 10
#define HX711_CLK_PIN  11
#define HX711_PRESS_THRESHOLD 5000  // raw ADC units; calibrate on hardware

// MX hot-swap buttons (active-low with pull-up)
#define BTN_SW1_PIN 12
#define BTN_SW2_PIN 13
#define BTN_SW3_PIN 14
#define BTN_SW4_PIN 15

// SK6812 RGBW LED chains
#define LED_RING_PIN     20
#define LED_RING_COUNT   12
#define LED_BUTTON_PIN   21
#define LED_BUTTON_COUNT  4

// I2C bus for VEML7700 + MAX17048
#define I2C_SDA_PIN 26
#define I2C_SCL_PIN 27

// Protocol version
#define PROTOCOL_VERSION 1

// Slow sensor poll intervals (ms)
#define BATT_POLL_MS    5000
#define LUX_POLL_MS     2000
#define STATE_TX_MS        5   // publish state every 5 ms (or on change)

// COBS max frame size: ToKnob worst-case (~200 B) + overhead
#define COBS_BUF_SIZE 256

// ---- Static objects ----
static HX711              s_hx711;
static Adafruit_NeoPixel  s_ring(LED_RING_COUNT,   LED_RING_PIN,   NEO_GRBW + NEO_KHZ800);
static Adafruit_NeoPixel  s_btns(LED_BUTTON_COUNT, LED_BUTTON_PIN, NEO_GRBW + NEO_KHZ800);
static Adafruit_VEML7700  s_veml;
static Adafruit_MAX17048  s_maxfuel;

// ---- Protocol state ----
static uint32_t s_press_nonce  = 0;
static bool     s_was_pressed  = false;

static int32_t  s_battery_pct  = -1;
static int32_t  s_ambient_lux  = -1;

static uint32_t s_last_batt_ms = 0;
static uint32_t s_last_lux_ms  = 0;
static uint32_t s_last_tx_ms   = 0;

static int32_t  s_last_sent_pos = INT32_MIN;

// RX accumulation buffer (COBS frames terminated by 0x00)
static uint8_t  s_rx_buf[COBS_BUF_SIZE];
static uint16_t s_rx_len = 0;

// ---- LED callback buffers (filled when KnobConfig is decoded) ----
static uint8_t s_ring_rgbw[48];
static uint8_t s_btn_rgbw[16];
static size_t  s_ring_len = 0;
static size_t  s_btn_len  = 0;

// ---- CRC-32 (IEEE 802.3 polynomial, matching ESP32's esp_rom_crc32_le) ----
static uint32_t _crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            if (crc & 1) crc = (crc >> 1) ^ 0xEDB88320;
            else         crc >>= 1;
        }
    }
    return ~crc;
}

// ---- COBS encode: src -> dst (returns total encoded length, INCLUDING 0x00 delimiter) ----
static size_t _cobs_encode(const uint8_t *src, size_t src_len, uint8_t *dst)
{
    size_t  out   = 0;
    size_t  code_pos = out++;  // position of the next overhead byte
    uint8_t code  = 1;

    for (size_t i = 0; i < src_len; i++) {
        if (src[i] != 0x00) {
            dst[out++] = src[i];
            code++;
            if (code == 0xFF) {
                dst[code_pos] = code;
                code_pos = out++;
                code = 1;
            }
        } else {
            dst[code_pos] = code;
            code_pos = out++;
            code = 1;
        }
    }
    dst[code_pos] = code;
    dst[out++] = 0x00;  // frame delimiter
    return out;
}

// ---- COBS decode: src -> dst (returns decoded length, or 0 on error) ----
static size_t _cobs_decode(const uint8_t *src, size_t src_len, uint8_t *dst)
{
    size_t out = 0;
    size_t i   = 0;
    while (i < src_len) {
        uint8_t code = src[i++];
        if (code == 0) break;
        for (uint8_t j = 1; j < code; j++) {
            if (i >= src_len) return 0;
            dst[out++] = src[i++];
        }
        if (code < 0xFF && i < src_len) {
            dst[out++] = 0x00;
        }
    }
    return out;
}

// ---- nanopb callbacks for bytes fields in KnobConfig ----
static bool _ring_rgbw_decode_cb(pb_istream_t *stream, const pb_field_t *field, void **arg)
{
    s_ring_len = stream->bytes_left < 48 ? stream->bytes_left : 48;
    return pb_read(stream, s_ring_rgbw, s_ring_len);
}

static bool _btn_rgbw_decode_cb(pb_istream_t *stream, const pb_field_t *field, void **arg)
{
    s_btn_len = stream->bytes_left < 16 ? stream->bytes_left : 16;
    return pb_read(stream, s_btn_rgbw, s_btn_len);
}

// ---- LED update from decoded config ----
static void _apply_leds(void)
{
    // Ring LEDs (SK6812 RGBW, 4 bytes per LED)
    for (size_t i = 0; i < LED_RING_COUNT && (i * 4 + 3) < s_ring_len; i++) {
        uint8_t r = s_ring_rgbw[i*4 + 0];
        uint8_t g = s_ring_rgbw[i*4 + 1];
        uint8_t b = s_ring_rgbw[i*4 + 2];
        uint8_t w = s_ring_rgbw[i*4 + 3];
        s_ring.setPixelColor(i, s_ring.Color(r, g, b, w));
    }
    s_ring.show();

    // Button LEDs (SK6812 RGBW, 4 bytes per LED)
    for (size_t i = 0; i < LED_BUTTON_COUNT && (i * 4 + 3) < s_btn_len; i++) {
        uint8_t r = s_btn_rgbw[i*4 + 0];
        uint8_t g = s_btn_rgbw[i*4 + 1];
        uint8_t b = s_btn_rgbw[i*4 + 2];
        uint8_t w = s_btn_rgbw[i*4 + 3];
        s_btns.setPixelColor(i, s_btns.Color(r, g, b, w));
    }
    s_btns.show();
}

// ---- Handle a decoded ToKnob message ----
static void _handle_to_knob(const ToKnob *msg)
{
    if (msg->which_payload == ToKnob_config_tag) {
        motor_set_config(&msg->payload.config);
        _apply_leds();
    }

    // Send Ack for every received packet (echoes nonce)
    uint8_t pb_buf[64];
    FromKnob ack_msg = FromKnob_init_zero;
    ack_msg.protocol_version     = PROTOCOL_VERSION;
    ack_msg.which_payload        = FromKnob_ack_tag;
    ack_msg.payload.ack          = msg->nonce;

    pb_ostream_t ostream = pb_ostream_from_buffer(pb_buf, sizeof(pb_buf));
    if (!pb_encode(&ostream, FromKnob_fields, &ack_msg)) return;

    uint32_t crc = _crc32(pb_buf, ostream.bytes_written);
    uint8_t crc_buf[4] = { (uint8_t)(crc), (uint8_t)(crc>>8), (uint8_t)(crc>>16), (uint8_t)(crc>>24) };

    uint8_t framed[COBS_BUF_SIZE];
    // Concatenate pb + crc, then COBS-encode
    uint8_t plain[68];
    memcpy(plain, pb_buf, ostream.bytes_written);
    memcpy(plain + ostream.bytes_written, crc_buf, 4);
    size_t framed_len = _cobs_encode(plain, ostream.bytes_written + 4, framed);
    Serial2.write(framed, framed_len);
}

// ---- Process one accumulated COBS frame ----
static void _process_frame(const uint8_t *frame, size_t len)
{
    if (len < 5) return;  // minimum: 1 byte pb + 4 bytes CRC

    uint8_t decoded[COBS_BUF_SIZE];
    size_t  dec_len = _cobs_decode(frame, len, decoded);
    if (dec_len < 5) return;

    // Verify CRC (last 4 bytes)
    uint32_t crc_recv = (uint32_t)decoded[dec_len-4]
                      | (uint32_t)decoded[dec_len-3] << 8
                      | (uint32_t)decoded[dec_len-2] << 16
                      | (uint32_t)decoded[dec_len-1] << 24;
    uint32_t crc_calc = _crc32(decoded, dec_len - 4);
    if (crc_recv != crc_calc) return;

    ToKnob msg = ToKnob_init_zero;
    msg.payload.config.led_ring_rgbw.funcs.decode  = _ring_rgbw_decode_cb;
    msg.payload.config.button_led_rgbw.funcs.decode = _btn_rgbw_decode_cb;

    pb_istream_t istream = pb_istream_from_buffer(decoded, dec_len - 4);
    if (!pb_decode(&istream, ToKnob_fields, &msg)) return;

    _handle_to_knob(&msg);
}

// ---- Send current KnobState to the P4 ----
static void _send_state(void)
{
    // Build button_mask from MX button GPIOs (active-low: 0 = pressed)
    uint32_t mask = 0;
    if (!digitalRead(BTN_SW1_PIN)) mask |= (1 << 0);
    if (!digitalRead(BTN_SW2_PIN)) mask |= (1 << 1);
    if (!digitalRead(BTN_SW3_PIN)) mask |= (1 << 2);
    if (!digitalRead(BTN_SW4_PIN)) mask |= (1 << 3);

    // Strain-gauge press detection
    if (s_hx711.is_ready()) {
        long raw = s_hx711.read();
        bool pressed = (raw > HX711_PRESS_THRESHOLD);
        if (pressed && !s_was_pressed) {
            s_press_nonce++;
        }
        s_was_pressed = pressed;
    }

    KnobState state = KnobState_init_zero;
    state.current_position  = motor_get_position();
    state.sub_position_unit = motor_get_sub_position();
    state.press_nonce       = s_press_nonce;
    state.button_mask       = mask;
    state.battery_percent   = s_battery_pct;
    state.ambient_lux       = s_ambient_lux;

    // KnobState_size (from pb.h) must fit in pb_buf[64] with 4 bytes of CRC headroom.
    // If new fields push KnobState_size past 64, increase pb_buf and plain below.
    static_assert(KnobState_size + 4 <= 68, "KnobState too large for send buffers");

    uint8_t pb_buf[64];
    FromKnob msg = FromKnob_init_zero;
    msg.protocol_version  = PROTOCOL_VERSION;
    msg.which_payload     = FromKnob_state_tag;
    msg.payload.state     = state;

    pb_ostream_t ostream = pb_ostream_from_buffer(pb_buf, sizeof(pb_buf));
    if (!pb_encode(&ostream, FromKnob_fields, &msg)) return;

    uint32_t crc = _crc32(pb_buf, ostream.bytes_written);
    uint8_t plain[68];
    memcpy(plain, pb_buf, ostream.bytes_written);
    plain[ostream.bytes_written + 0] = (uint8_t)(crc);
    plain[ostream.bytes_written + 1] = (uint8_t)(crc >> 8);
    plain[ostream.bytes_written + 2] = (uint8_t)(crc >> 16);
    plain[ostream.bytes_written + 3] = (uint8_t)(crc >> 24);

    uint8_t framed[COBS_BUF_SIZE];
    size_t  framed_len = _cobs_encode(plain, ostream.bytes_written + 4, framed);
    Serial2.write(framed, framed_len);

    s_last_sent_pos = state.current_position;
}

void interface_task_init(void)
{
    // UART to P4
    Serial2.setTX(KNOB_UART_TX);
    Serial2.setRX(KNOB_UART_RX);
    Serial2.begin(KNOB_UART_BAUD);

    // HX711 strain gauge
    s_hx711.begin(HX711_DOUT_PIN, HX711_CLK_PIN);

    // MX buttons with internal pull-ups (active-low)
    pinMode(BTN_SW1_PIN, INPUT_PULLUP);
    pinMode(BTN_SW2_PIN, INPUT_PULLUP);
    pinMode(BTN_SW3_PIN, INPUT_PULLUP);
    pinMode(BTN_SW4_PIN, INPUT_PULLUP);

    // SK6812 LED chains
    s_ring.begin();
    s_ring.clear();
    s_ring.show();
    s_btns.begin();
    s_btns.clear();
    s_btns.show();

    // I2C for VEML7700 + MAX17048
    Wire1.setSDA(I2C_SDA_PIN);
    Wire1.setSCL(I2C_SCL_PIN);
    Wire1.begin();

    s_veml.begin(&Wire1);
    s_maxfuel.begin(&Wire1);
}

void interface_task_loop(void)
{
    uint32_t now = millis();

    // Drain UART RX bytes; accumulate until 0x00 frame delimiter
    while (Serial2.available()) {
        uint8_t b = (uint8_t)Serial2.read();
        if (b == 0x00) {
            if (s_rx_len > 0) {
                _process_frame(s_rx_buf, s_rx_len);
            }
            s_rx_len = 0;
        } else {
            if (s_rx_len < COBS_BUF_SIZE - 1) {
                s_rx_buf[s_rx_len++] = b;
            }
        }
    }

    // Slow sensor polls
    if (now - s_last_batt_ms > BATT_POLL_MS) {
        s_last_batt_ms = now;
        s_battery_pct  = (int32_t)s_maxfuel.cellPercent();
    }
    if (now - s_last_lux_ms > LUX_POLL_MS) {
        s_last_lux_ms = now;
        s_ambient_lux = (int32_t)s_veml.readLux();
    }

    // Publish state every STATE_TX_MS or when position changes
    int32_t cur_pos = motor_get_position();
    bool pos_changed = (cur_pos != s_last_sent_pos);
    if (pos_changed || (now - s_last_tx_ms > STATE_TX_MS)) {
        s_last_tx_ms = now;
        _send_state();
    }
}
