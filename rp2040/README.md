# rp2040 — haptic knob co-MCU firmware

Firmware for the custom **RP2040 daughterboard** that acts as the SmartKnob-style
haptic input for the Waveshare ESP32-P4 build. The RP2040 runs all the
real-time work (FOC motor control, strain-gauge press, LEDs, sensor reads) and
talks to the ESP32-P4 over a single UART. The P4 sends haptic config; the RP2040
sends back knob position and button events.

> This is **our own firmware**, written fresh. Scott Bezek's
> [SmartKnob](https://github.com/scottbez1/smartknob) is used as the reference for
> the FOC + detent physics only (Apache 2.0 — see [`../NOTICE`](../NOTICE) and the
> per-file change notices on `src/motor_task.*`).

## Hardware

TMC6300 gate driver + gimbal motor (FOC via SimpleFOC), MT6701QT magnetic encoder
(SSI), BF350 full-bridge strain gauges + HX711 (knob press), 4× MX hot-swap
buttons, SK6812 RGBW ring + 4 button LEDs, VEML7700 ambient light, MAX17048
battery gauge. Full hardware design is in [`../docs/DESIGN_NOTES.md`](../docs/DESIGN_NOTES.md).

## Architecture (arduino-pico dual-core)

```
core 0  setup()/loop()   — interface_task: UART protocol, HX711, MX buttons,
                           SK6812 LEDs, I2C sensors
core 1  setup1()/loop1() — motor_task: SimpleFOC FOC torque loop + per-menu
                           detent/endstop physics (__not_in_flash_func hot path)
```

Cross-core state uses a pico-sdk `critical_section_t`. `motor_shared_init()` runs
first in core-0 `setup()` because `setup()` and `setup1()` run concurrently.

## Build

```bash
cd rp2040
pio run                 # build
pio run -t upload       # flash via BOOTSEL (drag-drop .uf2 also works)
```

nanopb is vendored at `lib/nanopb/` (not a registry dep). Generated protocol
files live in `src/proto_gen/` (copies of `../proto/`).

## Pin assignments and protocol

The pin map (verified conflict-free), the UART framing
(nanopb + CRC32 + COBS @ 921600 baud), and the SimpleFOC / MT6701 implementation
facts are all documented in [`../docs/KNOB-NOTES.md`](../docs/KNOB-NOTES.md) — read
that before changing pins. Key constraint: the motor 6-PWM pairs (GPIO0/1, 2/3,
4/5) must stay on their PWM slices, and the UART is on UART1 (GPIO8/9), NOT UART0
(whose default GPIO0/1 collide with the motor U-phase).

The matching P4-side driver is `waveshare/esp-idf/main/knob.c` + `knob_input.c`,
gated behind `KNOB_ENABLED`.
