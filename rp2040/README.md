# RP2040 Haptic Knob Firmware

This folder contains firmware for the RP2040 haptic-knob controller. The
controller sends input events to the ESP32-P4 through UART.

The RP2040 controls all time-critical hardware:

- Field-oriented motor control.
- MT6701 magnetic encoder.
- Strain-gauge press input.
- Four switches.
- RGBW LEDs and button LEDs.
- Ambient-light and battery sensors.

The ESP32-P4 sends haptic settings to the RP2040. The RP2040 sends position and
button events to the ESP32-P4.

This project uses SmartKnob as an engineering reference. The firmware in this
folder is project-specific firmware.

## Hardware

| Function | Part |
|---|---|
| Motor driver | TMC6300 |
| Motor | Gimbal brushless motor |
| Angle sensor | MT6701QT |
| Press sensor | Four BF350 strain gauges and HX711 |
| Buttons | Four MX switches |
| Lighting | SK6812 RGBW ring and four button LEDs |
| Ambient sensor | VEML7700 |
| Battery gauge | MAX17048 |

Refer to [DESIGN_NOTES.md](../docs/DESIGN_NOTES.md) for hardware decisions.

## Architecture

The firmware uses both RP2040 cores:

```text
Core 0: UART protocol, buttons, press input, LEDs, and I2C sensors
Core 1: Motor control, detents, and end stops
```

Cross-core data uses a Pico SDK critical section. Call
`motor_shared_init()` before either core accesses shared motor data.

## Build

Run these commands:

```powershell
cd rp2040
pio run
```

To upload with PlatformIO, run:

```powershell
pio run -t upload
```

The UF2 file is `.pio/build/rp2040/firmware.uf2`.

The project vendors nanopb in `lib/nanopb/`. Generated protocol files are in
`src/proto_gen/`.

## Motor and UART Pins

The six motor-control pins use GPIO0 through GPIO5. Keep each PWM pair on its
assigned PWM slice.

UART1 uses GPIO8 and GPIO9. Do not move the protocol to the default UART0 pins.
The default UART0 pins conflict with the motor U phase.

The ESP32-P4 driver uses `KNOB_ENABLED`. Set this CMake option to `1` to include
the driver.

## Native SDK Bring-Up

The `bringup/` folder contains a separate Pico SDK test harness. Use it before
you connect the production motor-control hardware.

The harness tests the RP2040, MT6701, and TMC6300 in separate checkpoints.
Refer to [bringup/README.md](bringup/README.md) for the procedure.
