# Home Controller — Design Notes

Consolidated PCB-design notes for the custom daughterboard, captured 2026-05-30
from the project brief + chat recommendations. Keep this file up to date as
decisions firm up so future sessions (mine or otherwise) have a single source
of truth.

---

## Architecture summary

| Block | Where it lives | Role |
|---|---|---|
| **ESP32-P4** | Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3 board (off-the-shelf) | Main MCU, screen, audio, Wi-Fi, BLE |
| **RP2040** | Custom daughterboard (bare chip in final; breakout for prototyping) | Dedicated FOC math @ 5 kHz+, hardware interrupts, dedicated UART to P4 |
| **Smart-knob front-end** | Custom daughterboard | TMC6300 + MT6701 + gimbal motor + BF350 strain gauges + HX711 |
| **Buttons** | Custom daughterboard | 4 × hot-swap MX-style mechanical switches, direct to RP2040 GPIOs |
| **I2C sensors** | Custom daughterboard | VEML7700 (light), MAX17048 (battery gauge); maybe IMU + Hall (kickstand) |
| **LEDs** | Custom daughterboard | SK6812 ring around knob + 1 per button cap |
| **Pi 5 (separate device)** | Home Assistant OS + Thread border router (ESP32-H2 over USB) | Spotify bridge + Matter/Thread for smart lights |

---

## KiCad library plan

### Copy from SmartKnob → `Symbols & Footprints/`

| Lib path (under `smartknob-master/electronics/lib/`) | Use |
|---|---|
| `MagnTek.lib` | MT6701 schematic symbol |
| `strain.lib` + `strain.pretty` | BF350-3AA gauge symbol + footprint |
| `VEML7700.lib` + `VEML7700.pretty` + `VEML7700.3dshapes` | Ambient-light sensor full package |
| `SK6812.lib` + `sk6812.pretty` + `sk6812.3dshapes` | Side-firing RGB LED |
| `Holes.pretty` | M1.6 / M2 mounting holes, alignment pins |
| `SolderPads.pretty` | 2/3/4/8-pad solder break-outs |
| `Modified.pretty` | Thermal-via QFN-20, SOT-223 variants, electrolytic cap with cutout, test-point pad |

**KiCad-version note:** SmartKnob is KiCad 5-era (no `.kicad_sym` files, just legacy
`.lib`). KiCad 6/7/8 Symbol Library Editor offers **File → Migrate Libraries** —
one-way `.lib` → `.kicad_sym`. Footprints (`.kicad_mod`) work as-is.

### Skip from SmartKnob

- `LCD_GC9A01` — no round screen on this knob
- `BM28` — not stacking a daughter PCB on the motor; simpler connector works
- `lilygo_micro32` — wrong MCU (we're on ESP32-P4)
- `SN74AVC4T774` — wrong chip; we use single-channel logic translators (see below)
- `GCT_USB` — Waveshare board already has USB-C; daughterboard's RP2040 USB-C
  uses a stock KiCad part
- `Trinamic` (TMC6300 symbol) — already have `TMC6300-LA` from elsewhere
- `Molex`, `BOM_Only`, `no_pin` — not applicable

### Source elsewhere

| Part | Source |
|---|---|
| **RP2040** | Raspberry Pi official KiCad libs (`github.com/raspberrypi/hardware-design-guide`) |
| **HX711** | SnapEDA / Ultra Librarian; Sparkfun's HX711 breakout schematic is a good reference design |
| **MAX17048** | ADI/Maxim KiCad lib or SnapEDA |
| **SN74AHCT1G125** *(see decision below)* | TI KiCad libs or SnapEDA; SOT-23-5 footprint is in stock KiCad `Package_TO_SOT_SMD` |
| **Kailh MX hot-swap socket** | [`keebio/keyswitches.pretty`](https://github.com/keebio/Keebio-Parts.pretty) — search "Kailh_MX_Hotswap" |
| **3.3 µH inductor (TMC6300 L1)** | Generic; pick size (0805/1210) at order time → stock KiCad `Inductor_SMD` |
| **MX1.25 2-pin (Waveshare power cable side)** | Stock KiCad `Connector_JST` (GH 1.25 mm pitch) |
| **USB-C (for RP2040 programming)** | Stock KiCad `Connector_USB` |
| **BMI270 (IMU candidate)** | Bosch KiCad libs or SnapEDA |
| **DRV5032 (Hall candidate)** | TI KiCad libs; SOT-23-3 footprint stock |

---

## Decisions locked in

### 1. Level shifter for SK6812 → **SN74AHCT1G125**
- Adafruit-endorsed for NeoPixel-family LEDs; Worldsemi (SK6812 maker)
  recommends HCT-family in their app notes.
- SOT-23-5, ~$0.30, decades of community validation.
- Tradeoff vs SN74LV1T34DBV: extra OE pin (tie to GND for always-on); broader
  stock base and more troubleshooting refs.
- One IC per LED chain — see decision #4 for chain count.

### 2. RP2040 — bare chip for the final PCB, breakout for prototyping
- Design the schematic now around the bare chip following the
  **RP2040 hardware design guide PDF** (in `Datasheets/`).
- Required around the chip: 12 MHz crystal, **W25Q128 QSPI flash**, **3V3 LDO**
  (or buck), USB-C, **BOOTSEL** button (pulls QSPI CS low), **RESET** button
  (pulls RUN low).
- Same firmware runs on both; breakout headers expose the same nets.

### 3. Strain gauge — **full Wheatstone bridge (all 4 BF350)**
- 4× sensitivity over quarter-bridge, 2× over half-bridge.
- Self-compensating for temperature drift (all four gauges drift together →
  cancels in the differential).
- Wired to **HX711 channel A at gain 128** (designed for ±20 mV span — matches
  a finger-press bridge output).
- Gauges placed on **flexure beams cut into the PCB**, two stretch + two
  compress on press-down. Copy the slot pattern from SmartKnob's
  `electronics/view_base/view_base.kicad_pcb` (open in KiCad PCB editor).

### 4. SK6812 LEDs — **two independent chains, 5 V supply**
- **Knob ring:** ~12 × SK6812 3535 (side-firing, 3.5 mm), ~30° spacing.
  Practical current ~200 mA average (peak full-white-full-bright would be
  ~720 mA but never run that way).
- **Under-buttons:** 1 per cap = 4 LEDs.
- **Why separate chains:** independent brightness/colour control, independent
  failure isolation, shorter chains = less accumulated timing jitter.
- **Cost:** two SN74AHCT1G125 instead of one — worth it.
- **5 V supply** preferred (cleaner timing margin than running off LiPo). The
  Waveshare board exposes a 5 V rail on its header — confirm pin from the
  user manual.

### 5. Motor connector — JST-PH 3-pin **or** 3-pin screw terminal
- Both stock KiCad footprints. Lewis to confirm based on the motor's leads
  (most gimbal motors ship with flying leads → JST-PH crimps are easiest).
- Skip BM28 (only matters if stacking PCBs on a hollow-shaft motor).

### 6. Programming / debug — what's on the daughterboard

| Element | Purpose | Required? |
|---|---|---|
| **USB-C** | RP2040 firmware flash via BOOTSEL (drag-and-drop `.uf2`) | Yes |
| **BOOTSEL button** | Hold to enter bootloader (pulls QSPI CS low) | Yes |
| **RESET button** | Power-cycle the RP2040 (pulls RUN low) | Yes |
| **SWD header** (3-pin, 1.27 mm: SWCLK / SWDIO / GND) | Breakpoint debugging via Pi Pico Debug Probe (~$12) | Strongly recommended |
| **Debug UART pads** (TX / RX / GND, 0.1″) | `printf` logging during dev — separate from the P4↔RP2040 UART | Cheap, nice-to-have |

ESP32-P4 is programmable via the Waveshare's onboard USB-C — no extra hardware
needed on the daughterboard for it.

---

## Open / pending decisions

1. **RP2040 breakout chosen for prototyping** — which exact module? (Pico, Pico W,
   bare-RP2040 breakout?) Affects header pinout during the prototype phase.
2. **Waveshare GPIO header mapping** — pull from the
   `waveshare ESP32-P4-WIFI6-Touch-LCD-4-3 user manual.pdf` (in `Datasheets/`).
   Pick the free pins for: UART-to-RP2040, I2C bus, 5 V, 3V3, GND. Avoid pins
   already used by the LCD/touch/audio codec/SDIO-to-C6.
3. **Motor connector** — JST-PH vs screw terminal — depends on the Sparkfun
   gimbal motor's leads when it arrives.
4. **IMU + Hall** — Lewis has these on hand but parts unconfirmed. Recommended
   defaults if undecided: **BMI270** (6-DoF, I2C, low-power) and **DRV5032**
   (SOT-23-3, digital out, micro-power Hall).
5. **Power button** — specific switch part not chosen yet; affects footprint
   selection.
6. **5 V rail source** — confirm Waveshare exposes 5 V on its header (likely
   yes, per the user manual). If not, add a small TPS61023-style boost on the
   daughterboard from the LiPo.

---

## SN74LV1T34DBV vs SN74AHCT1G125 — side-by-side

|  | SN74LV1T34DBV | SN74AHCT1G125 |
|---|---|---|
| Channels | 1, unidirectional, no enable | 1, unidirectional, 3-state (OE pin) |
| Package | SOT-23-5 | SOT-23-5 |
| V_CC range | 1.8–5.5 V | 4.5–5.5 V |
| Input threshold | Optimised for cross-rail translation | TTL-compatible (V_IH ≥ 2 V at V_CC = 5 V) |
| Cost | ~$0.30 | ~$0.30 |
| NeoPixel/SK6812 endorsement | Works fine | **Adafruit + Worldsemi-recommended** |
| Extra schematic clutter | None | OE pin to ground |

**Decision: SN74AHCT1G125.** Either works; this one is the safer/better-documented
choice for SK6812 specifically.

---

## SN74AVC4T774 ≠ SN74LV1T34/AHCT1G125 — why SmartKnob's is different

SmartKnob's knob carries the round LCD on top, so the **display SPI bus** (CLK,
MOSI, CS) + LED data all needed level translation in one spot. A 4-bit
auto-direction translator (SN74AVC4T774) handles all of them in one QFN-16,
tidier than four separate SOT-23s. **Lewis's design has no display on the knob**,
so only the two SK6812 data lines need shifting → one single-channel SOT-23 per
chain is cleaner and cheaper. Not a mistake in the SmartKnob — just a
consequence of the simpler architecture.

---

## Reference materials in this project folder

- **`Datasheets/`** — MT6701QT, TMC6300, BF350, SK6812, VEML7700, MAX17048,
  HX711, RP2040 (datasheet + hardware design guide), OT-EM3215 gimbal motor,
  ESP32-P4 datasheet + tech ref, Waveshare board user manual.
- **`Schematics/`** — SmartKnob base PDF, Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3
  schematic, TMC6300 reference schematic.
- **`Symbols & Footprints/`** — TMC6300-LA (already imported). Add the other
  libs per the "Copy from SmartKnob" section above.
- **`smartknob/smartknob-master/electronics/view_base/view_base.kicad_sch`** —
  the canonical reference schematic; open in KiCad 6+ to inspect the strain-
  gauge bridge wiring, TMC6300 + motor circuit, and MT6701 hookup.
- **`smartknob/smartknob-master/electronics/view_base/view_base.kicad_pcb`** —
  the reference PCB; useful for copying the **flexure slot pattern** for the
  strain gauges.

---

## How to use this file

- Edit it as decisions get locked in (move items from "Open" to "Decisions").
- When asking a future Claude Code session for help, point it at this file
  first — it captures all the architectural context that's otherwise scattered
  across the brief, the blog, and chat history.
- If something here contradicts the project blog or the SmartKnob source,
  this file is the more recent state for **this build** specifically.
