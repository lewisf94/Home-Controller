# RP2040 native-SDK bring-up

Native Raspberry Pi Pico C/C++ SDK hardware test harness for the SmartKnob
prototype. It proves the Pico and MT6701 breakout independently before the
TMC6300 or motor is connected.

This is deliberately separate from the production haptic-knob firmware in the
parent `rp2040/` project. That firmware already contains the SimpleFOC motor
loop, MT6701 SSI path, controls, and P4 protocol. This harness is the safe place
to validate breakout wiring and can also inform a later native-SDK migration;
it does not replace the production firmware yet.

## Checkpoints

Checkpoint 1 is hardware-verified on the Pico-layout RP2040 clone:

- USB CDC serial prints a one-second heartbeat.
- The standard Pico GPIO25 LED pulses when the clone has one fitted.
- No MT6701, TMC6300, motor, or load-cell pins are configured.

Checkpoint 2 is build- and flash-verified, but the sensor is not connected yet.
It adds the MT6701 over I2C while leaving every motor-driver output
unconfigured. It samples at 1 kHz and reports angle plus I2C timing statistics
once per second. With no sensor connected, `mt6701=NOT_FOUND` is the expected
result.

## MT6701 test wiring

Unplug USB before changing any wire.

| MT6701 breakout | Pico signal | Pico physical pin |
|---|---|---|
| VDD | 3V3(OUT) | 36 |
| GND | GND | 38 |
| SDA | GP4 / I2C0 SDA | 6 |
| SCL | GP5 / I2C0 SCL | 7 |
| Analog/PWM | Not connected | - |

Use 3.3 V, not VBUS/5 V. The sensor accepts 5 V, but a breakout's I2C pull-ups
can then put 5 V on RP2040 GPIO and damage it. Keep the four signal wires short.
The firmware enables the RP2040's weak internal pull-ups as a fallback; fit
external 4.7k pull-ups from SDA and SCL to 3.3 V if the breakout does not already
have them.

The MT6701 requires a diametrically magnetized two-pole magnet. For a temporary
test, centre a 6 mm diameter magnet over the IC with a 0.5-2.0 mm air gap using
non-magnetic card/plastic and reusable putty away from the sensing face.

Expected serial output with a connected sensor:

```text
SMARTKNOB_RP2040_OK sdk=2.3.0 clock=125000000Hz
MT6701_I2C address=0x06 sda=GP4 scl=GP5 baud=400000Hz
mt6701=OK raw=8192 angle=180.00deg reads=1000 errors=0 read_us=100/105/120
```

## Build

The official Raspberry Pi Pico VS Code extension and its matching Pico SDK
2.3.0, ARM GCC 15.2, CMake, Ninja, pioasm, and picotool packages are installed
under `%USERPROFILE%\.pico-sdk`.

```powershell
cd "C:\Users\lewis\Documents\home-controller\rp2040\bringup"
.\build.ps1
```

The result is `build\release\smartknob_rp2040.uf2`.

## Flash

1. Disconnect all prototype wiring.
2. Hold BOOTSEL while connecting the known-working USB-A to USB-C cable.
3. Copy `build\release\smartknob_rp2040.uf2` to the `RPI-RP2` drive.
4. The drive disconnects and the board restarts automatically.
5. Open the new USB serial COM port at 115200 baud.

The USB CDC link does not depend on the selected baud rate, but 115200 keeps
serial tools configured consistently.
