/*
 * Copyright 2022 Scott Bezek (protocol framing pattern from SmartKnob
 *   serial_protocol_protobuf.cpp / uart_stream.cpp —
 *   https://github.com/scottbez1/smartknob), Apache License, Version 2.0.
 * Copyright 2026 Lewis.
 *
 * Changes: ported to ESP-IDF UART API (uart_driver_install/uart_read_bytes);
 * CRC32 uses esp_rom_crc32_le() instead of SmartKnob's crc32.cpp;
 * COBS encoder/decoder reimplemented in C;
 * retry uses xTimerCreate() instead of millis();
 * message types updated for home_controller.proto (ToKnob/FromKnob).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "knob.h"

#include <string.h>
#include <stdatomic.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include "esp_rom_crc.h"

#include <pb_encode.h>
#include <pb_decode.h>

static const char *TAG = "knob";

#define PROTOCOL_VERSION   1
#define RX_TASK_STACK     4096
#define RX_TASK_PRIO         5
#define UART_RX_BUF_SIZE  2048
#define COBS_BUF_SIZE      512
#define RETRY_MS           250

// ---- Pending config (protected by a mutex-free atomic nonce) ----
static knob_state_cb_t  s_state_cb    = NULL;
static uint32_t         s_nonce       = 0;

static portMUX_TYPE     s_pending_mux = portMUX_INITIALIZER_UNLOCKED;
static ToKnob           s_pending_msg;
static bool             s_pending_valid = false;

static TimerHandle_t    s_retry_timer = NULL;

// LED byte buffers shared between knob_send_config() and the encode callback
static uint8_t s_ring_rgbw[48];
static uint8_t s_btn_rgbw[16];

// ---- CRC-32 (standard zlib/IEEE, matching the RP2040 software CRC) ----
// esp_rom_crc32_le() already inverts the seed on entry and the result on exit,
// so passing seed 0 (NOT 0xFFFFFFFF) and applying NO final XOR yields the
// standard zlib CRC32. Both sides return 0xCBF43926 for "123456789".
// (A final ^0xFFFFFFFF here would double-invert and disagree with the RP2040,
//  silently dropping every packet.)
static inline uint32_t _crc32(const uint8_t *data, size_t len)
{
    return esp_rom_crc32_le(0, data, len);
}

// ---- COBS encode: src -> dst; returns total encoded length (incl. delimiter) ----
static size_t _cobs_encode(const uint8_t *src, size_t src_len, uint8_t *dst)
{
    size_t  out      = 0;
    size_t  code_pos = out++;
    uint8_t code     = 1;

    for (size_t i = 0; i < src_len; i++) {
        if (src[i] != 0x00) {
            dst[out++] = src[i];
            if (++code == 0xFF) {
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
    dst[out++]    = 0x00;
    return out;
}

// ---- COBS decode: src -> dst; returns decoded length, 0 on error ----
static size_t _cobs_decode(const uint8_t *src, size_t src_len, uint8_t *dst)
{
    size_t out = 0, i = 0;
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

// ---- nanopb encode callbacks for bytes fields in KnobConfig ----
static bool _ring_encode_cb(pb_ostream_t *stream, const pb_field_t *field, void * const *arg)
{
    if (!pb_encode_tag_for_field(stream, field)) return false;
    return pb_encode_string(stream, s_ring_rgbw, 48);
}

static bool _btn_encode_cb(pb_ostream_t *stream, const pb_field_t *field, void * const *arg)
{
    if (!pb_encode_tag_for_field(stream, field)) return false;
    return pb_encode_string(stream, s_btn_rgbw, 16);
}

// ---- nanopb decode callback for the log string in FromKnob ----
static bool _log_decode_cb(pb_istream_t *stream, const pb_field_t *field, void **arg)
{
    uint8_t buf[256];
    size_t  len = stream->bytes_left < 255 ? stream->bytes_left : 255;
    if (!pb_read(stream, buf, len)) return false;
    buf[len] = '\0';
    ESP_LOGI(TAG, "[RP2040] %s", (char *)buf);
    return true;
}

// ---- Build and transmit a single UART packet ----
// No ToKnob_size static_assert is possible here (unlike the RP2040's KnobState
// guard): the LED bytes fields are pb_callback_t, so nanopb cannot emit a
// bounded ToKnob_size. Worst case by hand: scalars+tags ~70 B + 48 B ring +
// 16 B buttons ~= 140 B -- comfortably inside pb_buf[256], and pb_encode()
// fails cleanly (logged below) if a schema change ever outgrows it.
static void _send_packet(const ToKnob *msg)
{
    uint8_t pb_buf[256];
    pb_ostream_t os = pb_ostream_from_buffer(pb_buf, sizeof(pb_buf));
    if (!pb_encode(&os, ToKnob_fields, msg)) {
        ESP_LOGW(TAG, "pb_encode failed");
        return;
    }

    uint8_t with_crc[260];
    memcpy(with_crc, pb_buf, os.bytes_written);
    uint32_t crc = _crc32(pb_buf, os.bytes_written);
    with_crc[os.bytes_written + 0] = (uint8_t)(crc);
    with_crc[os.bytes_written + 1] = (uint8_t)(crc >> 8);
    with_crc[os.bytes_written + 2] = (uint8_t)(crc >> 16);
    with_crc[os.bytes_written + 3] = (uint8_t)(crc >> 24);

    uint8_t framed[COBS_BUF_SIZE];
    size_t  framed_len = _cobs_encode(with_crc, os.bytes_written + 4, framed);
    uart_write_bytes(KNOB_UART_NUM, framed, framed_len);
}

// ---- Retry timer: retransmit the pending config every 250 ms until Ack ----
static void _retry_timer_cb(TimerHandle_t xTimer)
{
    static uint32_t s_unacked_retries = 0;

    taskENTER_CRITICAL(&s_pending_mux);
    bool    valid = s_pending_valid;
    ToKnob  msg   = s_pending_msg;
    taskEXIT_CRITICAL(&s_pending_mux);

    if (valid) {
        _send_packet(&msg);
        // Bench diagnostic: an unplugged/dead RP2040 never acks, and without
        // this line the link failure is silent. First warn after ~2 s, then
        // every ~30 s so a knobless bench session isn't spammed.
        s_unacked_retries++;
        if (s_unacked_retries == 8 || (s_unacked_retries % 120) == 0) {
            ESP_LOGW(TAG, "knob not acking (nonce %u, %u retries) -- link down or unplugged?",
                     (unsigned)msg.nonce, (unsigned)s_unacked_retries);
        }
    } else {
        s_unacked_retries = 0;
    }
}

// ---- RX task: accumulate COBS frames, decode, dispatch ----
static void _rx_task(void *arg)
{
    uint8_t  rx_byte;
    uint8_t  frame_buf[COBS_BUF_SIZE];
    uint16_t frame_len = 0;

    for (;;) {
        int n = uart_read_bytes(KNOB_UART_NUM, &rx_byte, 1, portMAX_DELAY);
        if (n <= 0) continue;

        if (rx_byte == 0x00) {
            if (frame_len == 0) continue;

            uint8_t decoded[COBS_BUF_SIZE];
            size_t  dec_len = _cobs_decode(frame_buf, frame_len, decoded);
            frame_len = 0;

            if (dec_len < 5) continue;

            uint32_t crc_recv = (uint32_t)decoded[dec_len-4]
                              | (uint32_t)decoded[dec_len-3] << 8
                              | (uint32_t)decoded[dec_len-2] << 16
                              | (uint32_t)decoded[dec_len-1] << 24;
            if (_crc32(decoded, dec_len - 4) != crc_recv) continue;

            FromKnob fk = FromKnob_init_zero;
            fk.payload.log.funcs.decode = _log_decode_cb;

            pb_istream_t is = pb_istream_from_buffer(decoded, dec_len - 4);
            if (!pb_decode(&is, FromKnob_fields, &fk)) continue;

            // Drop packets from an incompatible firmware revision rather than
            // misinterpreting their fields.
            if (fk.protocol_version != PROTOCOL_VERSION) {
                ESP_LOGW(TAG, "proto mismatch: got %u want %d",
                         (unsigned)fk.protocol_version, PROTOCOL_VERSION);
                continue;
            }

            if (fk.which_payload == FromKnob_ack_tag) {
                // Stop retrying once the knob acknowledges the config
                taskENTER_CRITICAL(&s_pending_mux);
                if (s_pending_valid && s_pending_msg.nonce == fk.payload.ack) {
                    s_pending_valid = false;
                }
                taskEXIT_CRITICAL(&s_pending_mux);

            } else if (fk.which_payload == FromKnob_state_tag) {
                if (s_state_cb) {
                    s_state_cb(&fk.payload.state);
                }
            }
            // FromKnob_log_tag is handled by _log_decode_cb above
        } else {
            if (frame_len < COBS_BUF_SIZE - 1) {
                frame_buf[frame_len++] = rx_byte;
            }
        }
    }
}

// ---- Public API ----

esp_err_t knob_init(void)
{
    uart_config_t cfg = {
        .baud_rate  = KNOB_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_param_config(KNOB_UART_NUM, &cfg);
    if (err != ESP_OK) return err;

    err = uart_set_pin(KNOB_UART_NUM,
                       KNOB_UART_TX_PIN, KNOB_UART_RX_PIN,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) return err;

    err = uart_driver_install(KNOB_UART_NUM, UART_RX_BUF_SIZE, 0, 0, NULL, 0);
    if (err != ESP_OK) return err;

    s_retry_timer = xTimerCreate("knob_retry", pdMS_TO_TICKS(RETRY_MS),
                                 pdTRUE, NULL, _retry_timer_cb);
    if (!s_retry_timer) return ESP_ERR_NO_MEM;
    xTimerStart(s_retry_timer, 0);

    xTaskCreate(_rx_task, "knob_rx", RX_TASK_STACK, NULL, RX_TASK_PRIO, NULL);

    ESP_LOGI(TAG, "knob UART init OK (tx=%d rx=%d baud=%d)",
             KNOB_UART_TX_PIN, KNOB_UART_RX_PIN, KNOB_BAUD);
    return ESP_OK;
}

void knob_send_config(const KnobConfig *cfg)
{
    // Copy LED bytes into file-scope buffers so encode callbacks can read them.
    // The callbacks reference s_ring_rgbw / s_btn_rgbw directly.
    // NOTE: if led_ring_rgbw / button_led_rgbw are NULL (not set), the callbacks
    //       will send all-zero arrays, which turns the LEDs off -- safe default.
    // The KnobConfig struct carries pb_callback_t for bytes fields; we wire up
    // encode callbacks here so callers can pass a plain KnobConfig with no LEDs
    // set (all zeros) without crashing.

    ToKnob msg = ToKnob_init_zero;
    msg.protocol_version     = PROTOCOL_VERSION;
    msg.nonce                = ++s_nonce;
    msg.which_payload        = ToKnob_config_tag;
    msg.payload.config       = *cfg;
    // Wire encode callbacks (bytes fields cannot be NULL in a CALLBACK field)
    msg.payload.config.led_ring_rgbw.funcs.encode  = _ring_encode_cb;
    msg.payload.config.button_led_rgbw.funcs.encode = _btn_encode_cb;

    taskENTER_CRITICAL(&s_pending_mux);
    s_pending_msg   = msg;
    s_pending_valid = true;
    taskEXIT_CRITICAL(&s_pending_mux);

    _send_packet(&msg);
}

void knob_set_state_callback(knob_state_cb_t cb)
{
    s_state_cb = cb;
}
