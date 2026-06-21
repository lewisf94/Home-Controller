// Copyright 2026 Lewis. Apache License, Version 2.0.
#pragma once

#include "home_controller.pb.h"
#include "driver/uart.h"
#include "esp_err.h"

// UART to the RP2040 co-MCU. The ESP32-P4 GPIO matrix can route UART1 to any
// free pin. Pins verified against the Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3
// schematic (J3 40-pin interface header):
//   TX = GPIO32  J3 pin 31 (right side)
//   RX = GPIO46  J3 pin 35 (right side)  -- GPIO33 is NOT on J3, do not use it
//
// Pins avoided: GPIO 14-19 (ESP32-C6 SDIO D0-D3/CLK/CMD -- breaks WiFi),
// GPIO 34-38 (strapping / UART0 default), GPIO 24-25 (USB-JTAG),
// BSP pins (I2C 7/8, I2S 9-13, amp 53, LCD 26/27, touch-rst 23, SD 39-44).
#define KNOB_UART_NUM    UART_NUM_1
#define KNOB_UART_TX_PIN 32
#define KNOB_UART_RX_PIN 46
#define KNOB_BAUD        921600

esp_err_t knob_init(void);
void      knob_send_config(const KnobConfig *cfg);

typedef void (*knob_state_cb_t)(const KnobState *state);
void knob_set_state_callback(knob_state_cb_t cb);
