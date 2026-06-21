// Copyright 2026 Lewis. Apache License, Version 2.0.
#pragma once

// Initialise the knob UART driver and start the context-aware input mapper.
// Call once from app_main after audio_init().
void knob_input_start(void);
