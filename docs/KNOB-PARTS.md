# RP2040 Haptic Knob: Parts List

This document lists every part for the RP2040 haptic-knob daughterboard, in
one place. Read [DESIGN_NOTES.md](DESIGN_NOTES.md) for the reasoning behind
each choice. Read [KNOB-NOTES.md](KNOB-NOTES.md) for the firmware facts that
depend on each part, for example an I2C address or a pin assignment.

This project targets two build stages:

- A prototype stage, on a Pico-layout RP2040 module.
- A final stage, on a custom PCB with a bare RP2040 chip.

Each table below states which stage a part belongs to.

---

## Core compute (final PCB stage)

| Part | Qty | Footprint or package | Purpose | Status |
|---|---|---|---|---|
| RP2040 | 1 | QFN-56 | Main controller for the daughterboard | Locked |
| 12 MHz crystal | 1 | Not yet selected | RP2040 clock reference | Locked requirement; exact part open |
| W25Q128 | 1 | QSPI flash package | Program storage | Locked |
| 3.3 V regulator | 1 | Not yet selected | Powers the RP2040 and the 3.3 V logic | Locked requirement; exact part open |
| USB-C receptacle | 1 | KiCad standard library footprint | UF2 programming and power | Locked requirement; exact part open |
| BOOTSEL button | 1 | Tactile switch | Enter the RP2040 bootloader | Locked |
| Reset button | 1 | Tactile switch | Pulls RUN low | Locked |
| SWD connector | 1 | Not yet selected | Hardware debugging | Recommended |
| Debug UART pads | — | Bare pads | Development logs | Optional |
| 5.1 kOhm resistor | 2 | 0402 or 0603 | CC1 and CC2 pull-down, for correct USB-C power negotiation | Locked |

Do not use a USB-C-to-USB-C cable with a clone board that lacks these
pull-down resistors. [KNOB-NOTES.md](KNOB-NOTES.md) and CLAUDE.md record a
tested clone that needs a USB-A-to-USB-C cable instead, for this exact reason.

For the prototype stage, use an off-the-shelf Pico-layout RP2040 module in
place of every part in this table. A Pico-layout module already includes the
crystal, the flash, the regulator, the USB connector, and both buttons.

---

## Motor drive

| Part | Qty | Footprint or package | Purpose | Status |
|---|---|---|---|---|
| TMC6300 | 1 | QFN-20, with thermal vias | Three-phase gate driver, six active-high PWM inputs | Locked |
| VM inductor | 1 | 3.3 microhenry | TMC6300 supply filtering | Locked |
| Gimbal brushless motor | 1 | SparkFun gimbal motor (typical 12N14P, `MOTOR_POLE_PAIRS 7` assumed) | Physical haptic feedback | Not yet arrived; confirm pole-pair count on arrival |
| Motor connector | 1 | 3-pin JST, or a 3-position screw terminal | Connects the motor leads to the board | Open; select after inspecting the motor leads |

Use the KiCad QFN footprint for the TMC6300, not the imported external
footprint. The imported footprint has no thermal vias.

---

## Angle sensing

| Part | Qty | Footprint or package | Purpose | Status |
|---|---|---|---|---|
| MT6701QT | 1 | QFN package (QT variant, not the SmartKnob CT variant) | 14-bit magnetic angle encoder, read over SSI on SPI0 | Locked |
| Diametric magnet | 1 | Sized to the MT6701QT datasheet | Provides the field the MT6701QT reads | Locked; required for correct operation |

Do not reuse the SmartKnob MT6701 symbol. That symbol matches the MT6701CT
package, not the MT6701QT package this project uses.

---

## Press sensing

| Part | Qty | Footprint or package | Purpose | Status |
|---|---|---|---|---|
| BF350 strain gauge | 4 | BF350-3AA footprint | Full Wheatstone bridge, for knob-press detection | Locked |
| HX711 | 1 | KiCad standard library footprint | Strain-gauge amplifier, channel A at gain 128 | Locked |

Mount the four gauges on PCB flexure beams. Use the SmartKnob flexure slots
only as a mechanical reference, not as the final mechanical design. Put two
gauges in tension and two gauges in compression. Tie the HX711 RATE pin HIGH,
for an 80-samples-per-second read rate; the default 10-samples-per-second rate
can miss a quick tap.

---

## Buttons

| Part | Qty | Footprint or package | Purpose | Status |
|---|---|---|---|---|
| MX hot-swap socket | 4 | Keebio footprint | Four physical buttons, active-low | Locked |
| MX switch | 4 | Standard MX stem | Switch for each hot-swap socket | Locked |

---

## Lighting

| Part | Qty | Footprint or package | Purpose | Status |
|---|---|---|---|---|
| SK6812 RGBW LED | 12 | Side-firing package, from the SmartKnob library | Knob ring | Locked |
| SK6812 RGBW LED | 4 | Side-firing package, from the SmartKnob library | Button backlighting | Locked |
| SN74AHCT1G125 | 2 | Manufacturer or verified external library | Level shifter, one per LED data chain | Locked |

Connect the output-enable pin of each SN74AHCT1G125 for continuous operation.
Each part accepts a 3.3 V input signal from a 5 V supply. Supply both LED
chains from 5 V. Confirm the available 5 V current before the final layout;
see the 5 V rail entry in the Power table below.

---

## Ambient light and battery sensing

| Part | Qty | Footprint or package | Purpose | Status |
|---|---|---|---|---|
| VEML7700 | 1 | From the SmartKnob library | Ambient-light sensor, I2C address `0x10` | Locked |
| MAX17048 | 1 | Manufacturer or verified external library | Battery fuel gauge, I2C address `0x36` | Not yet ordered |

The `0x36` address is the fixed datasheet address for the MAX17048, and the
Adafruit library default the firmware uses. An earlier build list recorded
this part at address `0x32`; that entry was a list error, not a firmware
error. With the MAX17048 absent, `begin()` fails, and the firmware logs a
battery reading of zero. This is not a blocker for a bench test of the other
parts.

---

## Power

| Part | Qty | Footprint or package | Purpose | Status |
|---|---|---|---|---|
| LiPo cell | 1 | 3.7 V nominal | Runs the TMC6300 directly, within its 2 V to 11 V supply range | Locked |
| Power switch | 1 | Not yet selected | Main power switch | Open |
| 5 V regulator or rail source | 1 | Not yet selected | Supplies both SK6812 LED chains | Open; confirm available current |

---

## Candidate parts (not yet locked)

| Part | Qty | Purpose | Status |
|---|---|---|---|
| BMI270 | 1 | Inertial measurement unit, an optional future addition | Candidate; not ordered |
| DRV5032 | 1 | Hall-effect sensor, an optional future addition | Candidate; not ordered |

Free RP2040 GPIO pins remain for either addition: GPIO7, GPIO19, GPIO22,
GPIO28, and GPIO29. An IMU could share I2C1, at address `0x68` or `0x69`,
with no conflict against the VEML7700 or the MAX17048. The protocol schema is
append-only, so either addition needs no breaking protocol change.

---

## Bring-up and test tools (not part of the daughterboard)

| Item | Purpose |
|---|---|
| USB-A-to-USB-C cable | Required for the native Pico SDK bring-up harness in `rp2040/bringup/`, and for a clone board without correct CC1/CC2 pull-down resistors |
| A separate Pico-layout RP2040 module | Prototype-stage development, in place of the bare-chip parts in the Core Compute table |

---

## Open decisions before you order the final PCB run

1. The final motor connector: a 3-pin JST connector, or a 3-position screw
   terminal.
2. The final IMU, if the project adds one: BMI270 is the current candidate.
3. The final Hall sensor, if the project adds one: DRV5032 is the current
   candidate.
4. The power switch.
5. The available 5 V rail and its current capacity.
6. The final regulator for the bare RP2040.
7. The final crystal, USB-C receptacle, and SWD connector part numbers.

Confirm each open decision in [DESIGN_NOTES.md](DESIGN_NOTES.md) before you
place a final PCB order.
