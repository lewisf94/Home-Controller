/*
 * Copyright 2022 Scott Bezek (SmartKnob motor_task.h —
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
#pragma once

#include <stdint.h>
#include "proto_gen/home_controller.pb.h"

// ---- Pin assignments (adjust to match actual PCB layout) ----
// TMC6300 6PWM outputs
#define MOTOR_UH_PIN      0
#define MOTOR_UL_PIN      1
#define MOTOR_VH_PIN      2
#define MOTOR_VL_PIN      3
#define MOTOR_WH_PIN      4
#define MOTOR_WL_PIN      5
#define MOTOR_ENABLE_PIN  6

// MT6701QT magnetic encoder (SPI0)
#define ENCODER_CS_PIN   17
#define ENCODER_SCK_PIN  18
#define ENCODER_MISO_PIN 16

// Number of pole pairs for the gimbal motor (7 is typical; tune to match hardware)
#define MOTOR_POLE_PAIRS   7
// Voltage limit (V): keep below supply rail; motor saturates well before this
#define MOTOR_VOLTAGE_LIMIT 4.0f
// Scale factor translating normalised detent/endstop units -> motor voltage Nm
#define MOTOR_DETENT_SCALE   1.5f
#define MOTOR_ENDSTOP_SCALE  3.0f

// Initialise the cross-core critical section. MUST be called from core 0
// setup() BEFORE either core touches shared state, because setup() and setup1()
// run concurrently and core 0's loop reads motor position via the lock.
void motor_shared_init(void);

// Call once from core 1 setup1()
void motor_task_init(void);

// Call every loop() iteration on core 1; runs the FOC torque loop
void motor_task_loop(void);

// Thread-safe config update from core 0 (interface_task)
void motor_set_config(const KnobConfig *cfg);

// Read current logical detent position and sub-position fraction
int32_t motor_get_position(void);
float   motor_get_sub_position(void);
