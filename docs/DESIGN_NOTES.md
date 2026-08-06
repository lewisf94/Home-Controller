# Hardware Design Notes

This file records decisions for the custom controller daughterboard. Update it
when a hardware decision changes.

Use [CLAUDE.md](../CLAUDE.md) for firmware pins and protocol information. Use
[KNOB-PARTS.md](KNOB-PARTS.md) for the full parts list, with quantities and
status, drawn from the decisions in this file.

## Architecture

| Block | Location | Function |
|---|---|---|
| ESP32-P4 | Waveshare board | Main application, display, audio, and network |
| RP2040 | Custom daughterboard | Motor control, sensors, buttons, and LEDs |
| TMC6300 and MT6701 | Custom daughterboard | Haptic motor drive and angle measurement |
| BF350 and HX711 | Custom daughterboard | Knob press measurement |
| VEML7700 and MAX17048 | Custom daughterboard | Ambient light and battery state |
| Raspberry Pi 5 | Separate device | Home Assistant and Music Assistant |

Use a Pico-layout RP2040 module for prototypes. Use a bare RP2040 for the final
daughterboard.

## Library Rules

> **WARNING:** Verify every symbol and footprint against the applicable
> datasheet. An incorrect pin assignment can destroy a component.

SmartKnob uses an MT6701CT package. This project uses an MT6701QT package. Do
not use the SmartKnob MT6701 symbol.

### Reusable SmartKnob Items

| Item | Use |
|---|---|
| `strain.pretty` | BF350-3AA footprint |
| VEML7700 files | Ambient-light sensor |
| SK6812 files | Side-firing LED |
| `Holes.pretty` | Small mounting holes |
| `SolderPads.pretty` | Solder breakout pads |

Legacy SmartKnob symbols can require conversion to the current KiCad format.
Use the KiCad library migration tool.

### Items from Other Sources

| Part | Preferred source |
|---|---|
| MT6701QT | KiCad standard library |
| RP2040 | Raspberry Pi official libraries |
| HX711 | KiCad standard library |
| TMC6300 symbol | Imported TMC6300-LA symbol |
| TMC6300 footprint | KiCad QFN-20 footprint with thermal vias |
| MAX17048 | Manufacturer library or verified external library |
| SN74AHCT1G125 | Manufacturer library or verified external library |
| MX hot-swap socket | Keebio footprint |
| USB-C connector | KiCad standard library |
| BMI270 | Manufacturer library or verified external library |
| DRV5032 | Manufacturer library |

Use the KiCad QFN footprint for the TMC6300. The imported external footprint
does not contain suitable thermal vias.

## Locked Decisions

### LED Level Shifters

Use one SN74AHCT1G125 for each SK6812 data chain.

Connect the output-enable pin for continuous operation. The part accepts a
3.3 V input when its supply is 5 V.

The project uses two independent LED chains:

- One chain for the knob ring.
- One chain for the four button LEDs.

Supply the SK6812 LEDs from 5 V. Confirm the available current before final
layout.

### RP2040

Use a bare RP2040 on the final board. Include:

- 12 MHz crystal.
- W25Q128 QSPI flash.
- 3.3 V regulator.
- USB-C connector.
- BOOTSEL button.
- Reset button.
- SWD connector.

Use separate 5.1 kOhm pull-down resistors from CC1 and CC2 to ground. The
prototype clone does not operate correctly with a USB-C to USB-C cable.

Do not infer a configuration-channel repair from unlabeled test pads.

### Strain Gauge

Use four BF350 strain gauges as a full Wheatstone bridge.

Connect the bridge to HX711 channel A at gain 128. Put two gauges in tension and
two gauges in compression.

Use a generic resistor symbol for each BF350. Use the BF350-3AA footprint for
its solder tabs.

Put the gauges on PCB flexure beams. Use the SmartKnob flexure slots only as a
mechanical reference.

### Motor Connector

Use a three-pin JST connector or a three-position screw terminal. Select the
final connector after inspection of the motor leads.

### Programming and Debug

| Item | Function | Requirement |
|---|---|---|
| USB-C | UF2 programming | Required |
| BOOTSEL | Enter RP2040 bootloader | Required |
| Reset | Pull RUN LOW | Required |
| SWD connector | Hardware debugging | Recommended |
| Debug UART pads | Development logs | Optional |

The Waveshare board uses its own USB connection. The daughterboard does not
need an ESP32-P4 programming circuit.

## Open Decisions

1. Select the final motor connector.
2. Select the final IMU.
3. Select the final Hall sensor.
4. Select the power switch.
5. Confirm the available 5 V rail and current.
6. Select the final regulator for the bare RP2040.

The current IMU candidate is BMI270. The current Hall-sensor candidate is
DRV5032.

## Verified Interface Pins

The ESP32-P4 uses GPIO32 for UART TX and GPIO46 for UART RX.

The RP2040 uses GPIO8 for UART TX and GPIO9 for UART RX.

Refer to [CLAUDE.md](../CLAUDE.md) for all fixed RP2040 pins.

## Motor Test Hardware

The prototype motor has three equal phase resistances. Each phase pair measured
7.8 ohms.

Each motor phase was open circuit to the metal body.

Use [rp2040/bringup/README.md](../rp2040/bringup/README.md) for the controlled
TMC6300 test.

## Prior-Art Conclusions

Prior products confirm demand for a dedicated music controller:

- Knobby uses an ESP32 and rotary encoder as a Spotify remote.
- Spotify Car Thing used a display, rotary control, and preset buttons.
- Community firmware continued the Car Thing concept after Spotify ended it.

The project differs through its larger interface, direct Sonos control, theme
system, and haptic knob.

The required Spotify player-control endpoints remain available. Home Assistant
and Music Assistant reduce dependence on one service API.

SmartKnob and SimpleFOC show that RP2040 haptic control is practical. The main
product benefit is a different physical response for each interface context.

Cover Flow is suitable for visual album browsing. Carousel and Focus remain
available for faster movement through large libraries.

The theme system uses related industrial-design references:

- BASIC uses restrained functional design.
- GLYPH uses a dot-matrix instrument style.
- PIXEL uses a low-resolution computer style.
- PAPER uses a printed technical-instrument style.
- BOLD uses a geometric-sans display style.
- HIFI uses a broadcast-console style.

## Reference Material

The repository contains datasheets, schematics, symbols, and footprints. Use the
component datasheet as the final authority.

Do not change third-party reference files to match project documentation style.
