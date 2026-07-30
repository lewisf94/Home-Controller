# RP2040 Native SDK Bring-Up

This Pico SDK harness tests the SmartKnob prototype in controlled checkpoints.
It does not replace the production firmware.

## Checkpoint Status

| Checkpoint | Function | Status |
|---|---|---|
| 1 | RP2040 USB serial and heartbeat | Hardware-verified |
| 2 | MT6701 I2C angle measurement | Hardware-verified |
| 3 | TMC6300 motor test | Build-verified |

Checkpoint 2 measured 1000 successful reads per second with no errors. The test
used a centered diametric magnet.

Checkpoint 3 starts with VIO and all bridge inputs LOW. The firmware does not
start the motor at boot.

The arm command expires after 10 seconds. The test runs for 2.4 seconds at
12 percent duty.

## MT6701 Wiring

> **WARNING:** Disconnect USB before you change a wire. An incorrect supply
> connection can damage the RP2040.

| MT6701 breakout | Pico connection |
|---|---|
| VDD | `3V3` |
| GND | `GND` |
| SDA | `GP4` |
| SCL | `GP5` |
| Analog/PWM | Not connected |

Do not connect VDD to VBUS or 5 V. A 5 V pull-up can put an unsafe voltage on
an RP2040 input.

Keep the four wires short. Fit 4.7 kOhm pull-up resistors if the breakout does
not contain suitable resistors.

Use a diametrically magnetized two-pole magnet. Put its center above the center
of the MT6701 package.

Use an air gap between 0.5 mm and 2.0 mm. Use non-magnetic material to hold the
magnet during this temporary test.

Expected output:

```text
SMARTKNOB_RP2040_OK sdk=2.3.0 clock=125000000Hz
MT6701_I2C address=0x06 sda=GP4 scl=GP5 baud=100000Hz
mt6701=OK raw=8192 angle=180.00deg reads=1000 errors=0 read_us=821/821/830
```

## TMC6300 Wiring

> **WARNING:** Secure the motor before you enable the driver. An unsecured motor
> can move suddenly and damage its wires.

> **CAUTION:** Disconnect the MT6701 wires before you flash the motor-test UF2.
> GP4 and GP5 become motor-control outputs.

| TMC6300 | Pico |
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

Connect motor power separately:

| TMC6300 | Connection |
|---|---|
| VIN | Verified 5 V motor supply |
| GND | Motor-supply ground and Pico ground |
| U | Motor red phase |
| V | Motor yellow phase |
| W | Motor light-blue phase |

The initial phase order is not critical. Interchange two motor phases to reverse
the direction.

The prototype motor measured 7.8 ohms between each phase pair. Each phase was
open circuit to the motor body.

Use the Elegoo MB-V2 only for the short low-duty test. Its nominal current limit
is close to the motor's measured locked-phase current.

## Motor Test Procedure

1. Disconnect the TMC6300 and the motor.
2. Flash `smartknob_motor_test.uf2`.
3. Start the serial monitor.
4. Make sure that serial shows `startup=SAFE`.
5. Disconnect all power.
6. Connect GP0 through GP7 to the TMC6300.
7. Connect the Pico ground to the motor-supply ground.
8. Leave U, V, and W disconnected.
9. Connect the verified 5 V supply to VIN.
10. Apply power to the Pico and the motor supply.
11. Press `E`.
12. Make sure that serial shows `tmc6300=ARMED`.
13. Make sure that DIAG remains inactive.
14. Press `X`.
15. Make sure that the driver becomes disabled.
16. Disconnect all power.
17. Connect the three motor phases.
18. Secure the motor.
19. Apply power.
20. Press `E`.
21. Press `T`.
22. Press `X` immediately if you detect unexpected movement, sound, or heat.

The `E` command enables VIO but keeps all bridge inputs off. The `T` command is
rejected until the driver is armed.

## Build

The build script uses Pico SDK 2.3.0 and the installed ARM toolchain.

```powershell
cd "C:\Users\lewis\Documents\home-controller\rp2040\bringup"
.\build.ps1
```

The script creates these files:

- `build\release\smartknob_rp2040.uf2`
- `build\release\smartknob_motor_test.uf2`

## Monitor

Run:

```powershell
.\monitor.ps1 -PortName COM4
```

| Key | Function |
|---|---|
| `E` | Arm the driver |
| `T` | Run the motor test |
| `X` | Stop and disable |
| `S` | Show status |
| `H` | Show help |
| `Q` | Close the monitor |

## Flash

1. Disconnect all prototype wiring.
2. Hold BOOTSEL.
3. Connect the tested USB-A to USB-C cable.
4. Copy the required UF2 file to the `RPI-RP2` drive.
5. Wait for the board to restart.
6. Open the new serial port.

Use 115200 baud for consistent serial-tool settings.
