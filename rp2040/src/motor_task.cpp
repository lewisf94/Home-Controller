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

// Angle at which position == config.position (reference anchor)
static float      s_angle_reference     = 0.0f;
static uint32_t   s_last_nonce          = 0;

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

    // Capture initial angle so position 0 starts where the knob is resting
    s_angle_reference = s_sensor.getAngle();
}

static float __not_in_flash_func(_compute_torque)(float current_angle, const KnobConfig *cfg)
{
    if (cfg->position_width_radians < 1e-6f) {
        return 0.0f;
    }

    // Raw fractional position relative to reference anchor
    float raw = (current_angle - s_angle_reference) / cfg->position_width_radians;

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
    float err    = current_angle - center;

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
        err    = current_angle - center;
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

    // Re-anchor reference angle when position_nonce changes (host moved the knob)
    if (dirty && local_cfg.position_nonce != s_last_nonce) {
        s_last_nonce    = local_cfg.position_nonce;
        float cur_angle = s_sensor.getAngle();
        s_angle_reference = cur_angle - local_cfg.position * local_cfg.position_width_radians;
    }

    float torque = _compute_torque(s_sensor.getAngle(), &local_cfg);
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
