// Copyright 2026 Lewis. Apache License, Version 2.0.
#pragma once

#include "home_controller.pb.h"
#include "driver/uart.h"
#include "esp_err.h"

// Default UART assignment (change to match physical wiring)
#define KNOB_UART_NUM    UART_NUM_1
#define KNOB_UART_TX_PIN 16
#define KNOB_UART_RX_PIN 17
#define KNOB_BAUD        921600

esp_err_t knob_init(void);
void      knob_send_config(const KnobConfig *cfg);

typedef void (*knob_state_cb_t)(const KnobState *state);
void knob_set_state_callback(knob_state_cb_t cb);
