/*
 * Copyright 2022 Scott Bezek (SmartKnob motor_task.cpp —
 *   https://github.com/scottbez1/smartknob), Apache License, Version 2.0.
 * Copyright 2026 Lewis.
 *
 * Changes: ported from ESP32/FreeRTOS to RP2040/Arduino; SimpleFOC API
 * updated for RP2040 PWM; TMC6300 pin assignments updated for custom
 * daughterboard; display/LED coupling removed; HapticData struct extended
 * with per-context detent maps.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "motor_task.h"

#include <Arduino.h>
#include <SimpleFOC.h>
#include "encoders/mt6701/MagneticSensorMT6701SSI.h"
#include <math.h>
#include "pico/platform.h"  // __not_in_flash_func

// ---- SimpleFOC objects ----
// MT6701 reads over SSI (25-bit frame: 1 ignored + 14 angle + 4 status + 6 CRC). The generic
// MagneticSensorSPI class uses the AS5048 register-read convention and mangles
// the MT6701 frame, so we use the dedicated SSI class (defaults: 1 MHz, SPI_MODE2).
static MagneticSensorMT6701SSI s_sensor(ENCODER_CS_PIN);
static BLDCMotor               s_motor(MOTOR_POLE_PAIRS);
// The TMC6300 is a standalone 3-phase gate driver (6 logic inputs + enable);
// SimpleFOC has no TMC6300-specific class -- it is driven by the core
// BLDCDriver6PWM. Argument order is per-phase interleaved (Ah, Al, Bh, Bl,
// Ch, Cl, enable). On the RP2040 each phase's high/low pair MUST sit on the
// SAME PWM slice (channels A/B): GPIO0/1 = slice0 A/B, GPIO2/3 = slice1 A/B,
// GPIO4/5 = slice2 A/B -- the default pins below satisfy this. The driver does
// NOT validate slice pairing, so verify any pin change against the RP2040 PWM map.
static BLDCDriver6PWM          s_driver(
    MOTOR_UH_PIN, MOTOR_UL_PIN,
    MOTOR_VH_PIN, MOTOR_VL_PIN,
    MOTOR_WH_PIN, MOTOR_WL_PIN,
    MOTOR_ENABLE_PIN);

// ---- Shared state (read by core 0, written by both cores) ----
// mutex via critical_section (pico-sdk) -- spin-lock friendly on RP2040
#include "pico/critical_section.h"
static critical_section_t s_cs;
static volatile bool      s_cs_ready = false;

static KnobConfig s_config = KnobConfig_init_zero;
static bool       s_config_dirty = false;

// Current logical detent position (set when config.position_nonce changes)
static int32_t    s_current_position    = 0;
static float      s_current_sub_pos     = 0.0f;

// Angle at which position == config.position (reference anchor). Expressed on
// the UNWRAPPED angle basis (see s_unwrapped_angle below), not the sensor's
// raw [0, 2*PI) reading.
static float      s_angle_reference     = 0.0f;
static uint32_t   s_last_nonce          = 0;

// MagneticSensorMT6701SSI::getAngle() wraps every revolution ([0, 2*PI)).
// _compute_torque()'s detent math needs a continuous angle -- diffing the raw
// reading against s_angle_reference breaks the instant the two straddle the
// wrap point (a sudden ~2*PI jump in "raw", i.e. a torque kick). Instead we
// accumulate an unwrapped angle every tick: the physical motion in one 5 kHz
// tick is always tiny relative to 2*PI, so wrapping just that ONE-TICK delta
// (never the absolute angle) is always unambiguous. s_angle_reference and
// _compute_torque() both operate on this basis, not the raw sensor value.
static float      s_last_raw_angle      = 0.0f;
static float      s_unwrapped_angle     = 0.0f;

// Local pi constant -- avoids depending on M_PI, which isn't guaranteed by
// plain <math.h> on every libc (it's a common but non-standard extension).
#define KNOB_PI 3.14159265358979323846f

// Wrap a one-tick angle delta into (-PI, PI]. Safe ONLY for small deltas
// (well under 2*PI) -- do not use this on an absolute angle-vs-reference
// difference, which is exactly the bug this fixes.
static inline float __not_in_flash_func(_wrap_delta)(float d)
{
    while (d >  KNOB_PI) d -= 2.0f * KNOB_PI;
    while (d < -KNOB_PI) d += 2.0f * KNOB_PI;
    return d;
}

void motor_shared_init(void)
{
    critical_section_init(&s_cs);
    s_cs_ready = true;
}

void motor_task_init(void)
{
    SPI.setRX(ENCODER_MISO_PIN);
    SPI.setSCK(ENCODER_SCK_PIN);
    SPI.begin();

    s_sensor.init(&SPI);

    s_driver.voltage_power_supply = MOTOR_VOLTAGE_LIMIT;
    s_driver.init();

    s_motor.linkSensor(&s_sensor);
    s_motor.linkDriver(&s_driver);

    s_motor.foc_modulation = FOCModulationType::SpaceVectorPWM;
    s_motor.controller     = MotionControlType::torque;
    s_motor.voltage_limit  = MOTOR_VOLTAGE_LIMIT;

    s_motor.init();
    s_motor.initFOC();

    // Capture initial angle so position 0 starts where the knob is resting.
    // Seeds the unwrap tracker from this same first raw reading so
    // s_angle_reference and s_unwrapped_angle start on the same basis.
    s_last_raw_angle  = s_sensor.getAngle();
    s_unwrapped_angle = s_last_raw_angle;
    s_angle_reference = s_unwrapped_angle;
}

// unwrapped_angle must be on the s_unwrapped_angle basis (continuous, never
// wraps at 2*PI) -- NOT a raw s_sensor.getAngle() reading. See the
// s_unwrapped_angle comment near the top of this file for why.
static float __not_in_flash_func(_compute_torque)(float unwrapped_angle, const KnobConfig *cfg)
{
    if (cfg->position_width_radians < 1e-6f) {
        return 0.0f;
    }

    // Fractional position relative to reference anchor
    float raw = (unwrapped_angle - s_angle_reference) / cfg->position_width_radians;

    // Nearest integer detent
    int32_t nearest = (int32_t)roundf(raw);

    // Clamp to configured bounds
    if (cfg->min_position != -1 && nearest < cfg->min_position) {
        nearest = cfg->min_position;
    }
    if (cfg->max_position != -1 && nearest > cfg->max_position) {
        nearest = cfg->max_position;
    }

    // Angular error toward nearest detent center
    float center = s_angle_reference + nearest * cfg->position_width_radians;
    float err    = unwrapped_angle - center;

    // Check snap point: if we're past snap_point fraction of the half-width,
    // we snap to the next detent
    float half_width = cfg->position_width_radians * 0.5f;
    if (fabsf(err) > half_width * cfg->snap_point) {
        nearest += (err > 0) ? 1 : -1;
        // Re-clamp
        if (cfg->min_position != -1 && nearest < cfg->min_position) {
            nearest = cfg->min_position;
        }
        if (cfg->max_position != -1 && nearest > cfg->max_position) {
            nearest = cfg->max_position;
        }
        center = s_angle_reference + nearest * cfg->position_width_radians;
        err    = unwrapped_angle - center;
    }

    // Spring torque toward detent center
    float torque = -err * cfg->detent_strength_unit * MOTOR_DETENT_SCALE;

    // Endstop torque: resist moving outside bounds with a stiffer spring
    bool at_min = (cfg->min_position != -1 && nearest <= cfg->min_position && err < 0.0f);
    bool at_max = (cfg->max_position != -1 && nearest >= cfg->max_position && err > 0.0f);
    if (at_min || at_max) {
        torque = -err * cfg->endstop_strength_unit * MOTOR_ENDSTOP_SCALE;
    }

    // Publish position (written on core 1, read on core 0 via critical_section)
    if (s_cs_ready) {
        critical_section_enter_blocking(&s_cs);
        s_current_position = nearest;
        s_current_sub_pos  = err / cfg->position_width_radians;
        critical_section_exit(&s_cs);
    }

    return torque;
}

void __not_in_flash_func(motor_task_loop)(void)
{
    s_sensor.update();
    s_motor.loopFOC();

    // Accumulate the unwrapped angle (see the s_unwrapped_angle comment above)
    // before anything else this tick reads it.
    float raw_angle = s_sensor.getAngle();
    s_unwrapped_angle += _wrap_delta(raw_angle - s_last_raw_angle);
    s_last_raw_angle   = raw_angle;

    // Pull in a pending config update
    KnobConfig local_cfg = s_config;
    bool       dirty     = false;
    if (s_cs_ready) {
        critical_section_enter_blocking(&s_cs);
        local_cfg      = s_config;
        dirty          = s_config_dirty;
        s_config_dirty = false;
        critical_section_exit(&s_cs);
    }

    // Re-anchor reference angle when position_nonce changes (host moved the
    // knob). Uses the already-updated s_unwrapped_angle (this tick's value),
    // NOT a fresh s_sensor.getAngle() -- s_angle_reference must stay on the
    // unwrapped basis, not the raw one.
    if (dirty && local_cfg.position_nonce != s_last_nonce) {
        s_last_nonce      = local_cfg.position_nonce;
        s_angle_reference = s_unwrapped_angle - local_cfg.position * local_cfg.position_width_radians;
    }

    float torque = _compute_torque(s_unwrapped_angle, &local_cfg);
    s_motor.move(torque);
}

void motor_set_config(const KnobConfig *cfg)
{
    if (!s_cs_ready) return;
    critical_section_enter_blocking(&s_cs);
    s_config       = *cfg;
    s_config_dirty = true;
    critical_section_exit(&s_cs);
}

int32_t motor_get_position(void)
{
    if (!s_cs_ready) return 0;
    critical_section_enter_blocking(&s_cs);
    int32_t pos = s_current_position;
    critical_section_exit(&s_cs);
    return pos;
}

float motor_get_sub_position(void)
{
    if (!s_cs_ready) return 0.0f;
    critical_section_enter_blocking(&s_cs);
    float sub = s_current_sub_pos;
    critical_section_exit(&s_cs);
    return sub;
}
