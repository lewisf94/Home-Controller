#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "pb_decode.h"
#include "pb_encode.h"
#include "home_controller.pb.h"

/* Encode a ToKnob config with both LED fields, then decode it. */
static void static_round_trip(void)
{
    ToKnob out = ToKnob_init_zero;
    out.protocol_version = 1;
    out.nonce = 7;
    out.which_payload = ToKnob_config_tag;
    out.payload.config.position = 3;
    out.payload.config.led_ring_rgbw.size = 48;
    out.payload.config.button_led_rgbw.size = 16;
    for (int i = 0; i < 48; i++) out.payload.config.led_ring_rgbw.bytes[i] = (uint8_t)(i + 1);
    for (int i = 0; i < 16; i++) out.payload.config.button_led_rgbw.bytes[i] = (uint8_t)(200 + i);

    uint8_t buf[ToKnob_size];
    pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof buf);
    assert(pb_encode(&os, ToKnob_fields, &out));

    ToKnob in = ToKnob_init_zero;
    pb_istream_t is = pb_istream_from_buffer(buf, os.bytes_written);
    assert(pb_decode(&is, ToKnob_fields, &in));
    assert(in.which_payload == ToKnob_config_tag);
    assert(in.nonce == 7);
    assert(in.payload.config.position == 3);
    assert(in.payload.config.led_ring_rgbw.size == 48);
    assert(in.payload.config.button_led_rgbw.size == 16);
    assert(memcmp(in.payload.config.led_ring_rgbw.bytes,
                  out.payload.config.led_ring_rgbw.bytes, 48) == 0);
    assert(memcmp(in.payload.config.button_led_rgbw.bytes,
                  out.payload.config.button_led_rgbw.bytes, 16) == 0);
}

/* The P4 encodes the LED fields with a callback: a tag, a length, and the
 * bytes. Build those wire bytes by hand and make sure the RP2040 decodes the
 * colours. Before the static sizes, the oneof reset removed the decode
 * callbacks and both lengths decoded as 0. */
static void p4_callback_wire_format(void)
{
    uint8_t inner[2 + 48 + 2 + 16];
    size_t n = 0;
    inner[n++] = (10 << 3) | 2;
    inner[n++] = 48;
    for (int i = 0; i < 48; i++) inner[n++] = (uint8_t)(i + 1);
    inner[n++] = (11 << 3) | 2;
    inner[n++] = 16;
    for (int i = 0; i < 16; i++) inner[n++] = (uint8_t)(200 + i);

    uint8_t frame[8 + sizeof inner];
    size_t m = 0;
    frame[m++] = (1 << 3) | 0;  /* protocol_version */
    frame[m++] = 1;
    frame[m++] = (2 << 3) | 0;  /* nonce */
    frame[m++] = 9;
    frame[m++] = (3 << 3) | 2;  /* config */
    frame[m++] = (uint8_t)n;
    memcpy(frame + m, inner, n);
    m += n;

    ToKnob in = ToKnob_init_zero;
    pb_istream_t is = pb_istream_from_buffer(frame, m);
    assert(pb_decode(&is, ToKnob_fields, &in));
    assert(in.which_payload == ToKnob_config_tag);
    assert(in.nonce == 9);
    assert(in.payload.config.led_ring_rgbw.size == 48);
    assert(in.payload.config.led_ring_rgbw.bytes[47] == 48);
    assert(in.payload.config.button_led_rgbw.size == 16);
    assert(in.payload.config.button_led_rgbw.bytes[15] == 215);
}

static void from_knob_state_and_log(void)
{
    FromKnob out = FromKnob_init_zero;
    out.protocol_version = 1;
    out.which_payload = FromKnob_state_tag;
    out.payload.state.current_position = -4;
    out.payload.state.press_nonce = 12;

    uint8_t buf[FromKnob_size];
    pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof buf);
    assert(pb_encode(&os, FromKnob_fields, &out));
    assert(os.bytes_written + 4 <= 68);  /* RP2040 send buffer */

    FromKnob in = FromKnob_init_zero;
    pb_istream_t is = pb_istream_from_buffer(buf, os.bytes_written);
    assert(pb_decode(&is, FromKnob_fields, &in));
    assert(in.which_payload == FromKnob_state_tag);
    assert(in.payload.state.current_position == -4);
    assert(in.payload.state.press_nonce == 12);

    out.which_payload = FromKnob_log_tag;
    strcpy(out.payload.log, "motor ready");
    os = pb_ostream_from_buffer(buf, sizeof buf);
    assert(pb_encode(&os, FromKnob_fields, &out));
    memset(&in, 0, sizeof in);
    is = pb_istream_from_buffer(buf, os.bytes_written);
    assert(pb_decode(&is, FromKnob_fields, &in));
    assert(in.which_payload == FromKnob_log_tag);
    assert(strcmp(in.payload.log, "motor ready") == 0);
}

int main(void)
{
    static_round_trip();
    p4_callback_wire_format();
    from_knob_state_and_log();
    return 0;
}
