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

Checkpoint 2 is hardware-verified. It adds the MT6701 over I2C while leaving
every motor-driver output unconfigured. With a centred diametric magnet it
samples cleanly at 1 kHz and reports angle plus I2C timing statistics once per
second (`1000` reads, `0` errors measured on the prototype). While undetected,
it reports the live SDA/SCL levels and scans the I2C address space every five
seconds.

Checkpoint 3 is build-verified but not yet hardware-verified. It is a separate
TMC6300 motor-test UF2: all six bridge inputs and VIO are forced LOW before USB
starts, the driver requires an explicit arm command, arming expires after 10
seconds, and the motor routine is limited to 2.4 seconds at 12% duty. DIAG or
the stop command immediately turns every bridge input off and pulls VIO LOW.

## MT6701 test wiring

Unplug USB before changing any wire.

| MT6701 breakout | Pico signal | Pico board marking |
|---|---|---|
| VDD | 3V3(OUT) | `3V3` (not `3V3_EN`) |
| GND | GND | `GND` |
| SDA | GP4 / I2C0 SDA | `GP4` |
| SCL | GP5 / I2C0 SCL | `GP5` |
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
MT6701_I2C address=0x06 sda=GP4 scl=GP5 baud=100000Hz
mt6701=OK raw=8192 angle=180.00deg reads=1000 errors=0 read_us=821/821/830
```

## TMC6300 motor test

The motor-test firmware uses GP4 and GP5 for the W-phase bridge controls.
Disconnect the MT6701 SDA/SCL wires before flashing the motor-test UF2. Do not
try to run the I2C sensor and this motor test simultaneously.

The production six-PWM pin map is used:

| TMC6300 breakout | Pico |
|---|---|
| UH | GP0 |
| UL | GP1 |
| VH | GP2 |
| VL | GP3 |
| WH | GP4 |
| WL | GP5 |
| VIO | GP6 |
| DIAG | GP7 |
| GND | GND |
| SEN | Not connected |
| VCP | Not connected |

Motor power is separate from Pico USB power:

| TMC6300 breakout | Connection |
|---|---|
| VIN | Verified 5 V motor rail |
| GND | Motor-rail ground and Pico ground |
| U | Motor red phase |
| V | Motor yellow phase |
| W | Motor light-blue phase |

The phase order is arbitrary for the first test; swapping any two reverses
direction. The prototype motor measured 7.8 ohms across all three phase pairs
and open circuit from every phase to its metal body.

The Elegoo MB-V2 is suitable only for this brief low-duty test. Set its rail to
5 V and verify voltage and polarity with a multimeter before connecting VIN.
Its nominal 700 mA maximum is too close to the motor's approximately 640 mA
5 V locked phase-to-phase current for sustained or full-duty operation.

Bring-up order:

1. Leave the TMC6300 and motor disconnected. Flash `smartknob_motor_test.uf2`
   and confirm serial reports `startup=SAFE`.
2. Unplug everything. Wire GP0-GP7 and common ground to the TMC6300, but leave
   U/V/W empty. Connect the verified 5 V motor rail to VIN.
3. Power the Pico and motor rail. Press `E` in the monitor. Confirm the standby
   LED turns on, DIAG remains off, and serial reports `tmc6300=ARMED`.
4. Press `X`; confirm standby turns off. Then remove all power.
5. Connect motor U/V/W, secure the motor, restore power, press `E`, then `T`.
   Press `X` immediately for any unexpected sound, heat, or movement.

The test never spins at boot. `E` only enables VIO with all bridge inputs off;
`T` is rejected unless the driver was armed first.

## Build

The official Raspberry Pi Pico VS Code extension and its matching Pico SDK
2.3.0, ARM GCC 15.2, CMake, Ninja, pioasm, and picotool packages are installed
under `%USERPROFILE%\.pico-sdk`.

```powershell
cd "C:\Users\lewis\Documents\home-controller\rp2040\bringup"
.\build.ps1
```

The results are:

- `build\release\smartknob_rp2040.uf2` - MT6701 sensor test
- `build\release\smartknob_motor_test.uf2` - fail-safe TMC6300 motor test

## Monitor

The monitor enables DTR/RTS, shows output, and sends the single-key safety
commands:

```powershell
.\monitor.ps1 -PortName COM4
```

Keys: `E` arm, `T` run the 2.4-second test, `X` stop/disable, `S` status,
`H` help, `Q` close the monitor.

## Flash

1. Disconnect all prototype wiring.
2. Hold BOOTSEL while connecting the known-working USB-A to USB-C cable.
3. Copy `build\release\smartknob_rp2040.uf2` to the `RPI-RP2` drive.
4. The drive disconnects and the board restarts automatically.
5. Open the new USB serial COM port at 115200 baud.

The USB CDC link does not depend on the selected baud rate, but 115200 keeps
serial tools configured consistently.
