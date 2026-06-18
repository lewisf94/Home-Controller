// Copyright 2026 Lewis. Apache License, Version 2.0.
#pragma once

#include "home_controller.pb.h"
#include "driver/uart.h"
#include "esp_err.h"

// UART to the RP2040 co-MCU. The ESP32-P4 GPIO matrix can route UART1 to any
// free pin. PLACEHOLDER pins below -- CONFIRM against the Waveshare schematic /
// 40-pin header before wiring, then update these.
//
// DO NOT use GPIO 14-19: those are the onboard ESP32-C6 SDIO link
// (D0=14, D1=15, D2=16, D3=17, CLK=18, CMD=19; slave-reset=54) -- using them
// breaks WiFi the moment esp_hosted brings up the C6. Also avoid: BSP pins
// (I2C 7/8, I2S 9-13, amp 53, LCD 26/27, touch-rst 23, SD 39-44), strapping
// pins (34-38), and USB-JTAG (24/25). GPIO 32/33 are clear of all of these,
// but still verify they are broken out on the header before soldering.
#define KNOB_UART_NUM    UART_NUM_1
#define KNOB_UART_TX_PIN 32
#define KNOB_UART_RX_PIN 33
#define KNOB_BAUD        921600

esp_err_t knob_init(void);
void      knob_send_config(const KnobConfig *cfg);

typedef void (*knob_state_cb_t)(const KnobState *state);
void knob_set_state_callback(knob_state_cb_t cb);
