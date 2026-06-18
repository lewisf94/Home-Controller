// Copyright 2026 Lewis. Apache License, Version 2.0.
// Starts motor_task on core 1, interface_task on core 0.

#include <Arduino.h>
#include "motor_task.h"
#include "interface_task.h"

// Core 1: FOC torque loop
void setup1(void) { motor_task_init(); }
void loop1(void)  { motor_task_loop(); }

// Core 0: UART protocol, sensors, LEDs
void setup(void)  { interface_task_init(); }
void loop(void)   { interface_task_loop(); }
