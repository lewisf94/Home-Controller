// Copyright 2026 Lewis. Apache License, Version 2.0.
// Starts motor_task on core 1, interface_task on core 0.

#include <Arduino.h>
#include "motor_task.h"
#include "interface_task.h"

// Core 1: FOC torque loop
void setup1(void) { motor_task_init(); }
void loop1(void)  { motor_task_loop(); }

// Core 0: UART protocol, sensors, LEDs.
// motor_shared_init() initialises the cross-core lock first -- setup() and
// setup1() run concurrently, so the lock must exist before core 1's FOC loop
// (started from setup1) publishes position through it.
void setup(void)  { motor_shared_init(); interface_task_init(); }
void loop(void)   { interface_task_loop(); }
