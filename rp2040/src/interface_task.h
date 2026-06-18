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
#pragma once

// Call once from core 0 setup()
void interface_task_init(void);

// Call every loop() iteration on core 0
void interface_task_loop(void);
