# Home Controller — Design Notes

Consolidated PCB-design notes for the custom daughterboard, captured 2026-05-30
from the project brief + chat recommendations. Keep this file up to date as
decisions firm up so future sessions (mine or otherwise) have a single source
of truth.

> **Firmware status (2026-06-18):** the RP2040 firmware and the P4-side UART
> driver are written, committed, and gated behind `KNOB_ENABLED` (default off) —
> see [`KNOB-NOTES.md`](KNOB-NOTES.md) for pin assignments, the UART protocol,
> and the SimpleFOC/MT6701 implementation facts. This file remains the **hardware
> / PCB** source of truth; KNOB-NOTES is the **firmware / pin** source of truth.

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

**Note on SmartKnob `.lib` files:** The copy in `docs/smartknob-repo/` may not
include `.lib` symbol files (only `.dcm` doc stubs and `.pretty` footprints are
guaranteed to be present). If a symbol is missing, download the SmartKnob ZIP
fresh from GitHub. However, check the KiCad 8 stdlib first — several SmartKnob
symbols are now superseded and the stdlib version is better reviewed.

**Verify every third-party symbol pin-for-pin against its datasheet before use.**
SmartKnob targets the MT6701CT (SOIC-8); this build uses the MT6701QT (different
package — do not share that symbol). SnapEDA symbols sometimes swap pins or omit
exposed pads. One wrong pin can destroy a chip.

| Lib path (under `smartknob-master/electronics/lib/`) | Use |
|---|---|
| `strain.lib` | Contains an **HX711** symbol (not a BF350 symbol — BF350 is a passive resistor; use `Device:R` for it). Only useful if you want SmartKnob's particular HX711 pin layout — the KiCad stdlib `Analog_ADC:HX711` is a cleaner choice. |
| `strain.pretty` | **BF350-3AA strain gauge footprint** — this is what you actually need from this library. |
| `VEML7700.lib` + `VEML7700.pretty` + `VEML7700.3dshapes` | Ambient-light sensor full package (not in KiCad 8 stdlib) |
| `SK6812.lib` + `sk6812.pretty` + `sk6812.3dshapes` | Side-firing SK6812-SIDE-A LED (top-firing SK6812 is in KiCad 8 stdlib; the side-firing variant is not) |
| `Holes.pretty` | M1.6 / M2 mounting holes, alignment pins |
| `SolderPads.pretty` | 2/3/4/8-pad solder break-outs |
| `Modified.pretty` | SOT-223 variants, electrolytic cap with cutout, test-point pad (skip the QFN-20 thermal-via entry — use the stdlib footprint instead; see "Source elsewhere") |

**KiCad-version note:** SmartKnob is KiCad 5-era (no `.kicad_sym` files, just legacy
`.lib`). KiCad 6/7/8 Symbol Library Editor offers **File → Migrate Libraries** —
one-way `.lib` → `.kicad_sym`. Footprints (`.kicad_mod`) work as-is.

### Skip from SmartKnob

- `MagnTek.lib` (MT6701 symbol) — SmartKnob uses the CT (SOIC-8) variant; this
  build uses the **QT variant**. Use KiCad 8 stdlib `Sensor_Magnetic:MT6701QT`
  instead (different package, different pinout).
- `LCD_GC9A01` — no round screen on this knob
- `BM28` — not stacking a daughter PCB on the motor; simpler connector works
- `lilygo_micro32` — wrong MCU (we're on ESP32-P4)
- `SN74AVC4T774` — wrong chip; we use single-channel logic translators (see below)
- `GCT_USB` — Waveshare board already has USB-C; daughterboard's RP2040 USB-C
  uses a stock KiCad part
- `Trinamic` (TMC6300 symbol) — already have `TMC6300-LA` from SnapEDA
- `Molex`, `BOM_Only`, `no_pin` — not applicable

### Source elsewhere

| Part | Source |
|---|---|
| **MT6701QT** | KiCad 8 stdlib `Sensor_Magnetic:MT6701QT` — verify pin map against the QT datasheet before use (SmartKnob uses the CT/SOIC-8 variant; different package, different symbol) |
| **RP2040** | Raspberry Pi official KiCad libs (`github.com/raspberrypi/hardware-design-guide`) |
| **HX711** | KiCad 8 stdlib `Analog_ADC:HX711` (confirmed present in KiCad 8.0.0; Sparkfun's HX711 breakout schematic is a good wiring reference) |
| **TMC6300** symbol | SnapEDA `TMC6300-LA.kicad_sym` — already imported to `Symbols & Footprints/TMC6300-LA/`. Not in KiCad 8 stdlib. Verify pins against datasheet before sign-off. |
| **TMC6300** footprint | Use KiCad 8 stdlib `Package_DFN_QFN:QFN-20-1EP_3x3mm_P0.4mm_EP1.65x1.65mm_ThermalVias` — **not** the SnapEDA `.kicad_mod` bundled with the symbol (that file has 21 pads and zero thermal vias; the stdlib version is KLC-reviewed and includes thermal vias for heat dissipation) |
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
- **Prototype module selected (2026-07-27):** Pico-layout RP2040 clone
  (dual-core Arm Cortex-M0+, 264 KB SRAM). The native Pico SDK bring-up harness
  is in `rp2040/bringup/`; its MT6701 I2C test uses GP4 SDA and GP5 SCL at
  3.3 V. This is a hardware test path, separate from the production
  SimpleFOC/SSI firmware in `rp2040/`.
- The clone powers and enumerates with a USB-A to USB-C cable but not a USB-C to
  USB-C cable. Treat that as a clone-board USB-C implementation limitation,
  most likely missing the required 5.1 kΩ Rd pulldown on each CC pin. Do not
  infer a fix from unlabeled TP1-TP6 pads. The final daughterboard USB-C
  receptacle must have separate 5.1 kΩ pulldowns from CC1 and CC2 to GND.

### 3. Strain gauge — **full Wheatstone bridge (all 4 BF350)**
- 4× sensitivity over quarter-bridge, 2× over half-bridge.
- Self-compensating for temperature drift (all four gauges drift together →
  cancels in the differential).
- Wired to **HX711 channel A at gain 128** (designed for ±20 mV span — matches
  a finger-press bridge output).
- **KiCad symbol:** use `Device:R` — a BF350 strain gauge is electrically just
  a 350 Ω resistor. There is no dedicated BF350 schematic symbol. (`strain.lib`
  from SmartKnob contains an HX711 symbol, not a BF350 symbol.)
- **KiCad footprint:** SmartKnob's `strain.pretty/BF350-3AA` — pads sized for
  the gauge tabs and solder-bridge connections.
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

1. **RP2040 breakout — RESOLVED (2026-07-27):** Pico-layout RP2040 clone.
   Use USB-A to USB-C for this prototype; see RP2040 section 2 above for the
   C-to-C limitation and final-board CC requirements.
2. **Waveshare GPIO header mapping** — UART-to-RP2040 RESOLVED (2026-06-18):
   **P4 TX = GPIO32 (J3 pin 31), RX = GPIO46** — both verified broken out on J3
   and clear of LCD/touch/audio/SDIO-to-C6/strapping/USB-JTAG. GPIO33 is NOT on
   the header (was an early placeholder). Still to pick from the header: 5 V, 3V3,
   GND for the daughterboard power. See [`KNOB-NOTES.md`](KNOB-NOTES.md) for the
   full pin rationale.
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

## Landscape & prior-art research (2026-06-16)

Web research into the device's design space (prior art, the Spotify-platform
risk, the haptic knob, and the UI design languages), to inform direction. The
through-line: the concept is well-validated, the planned haptic knob is proven
ground, and the main strategic risk is Spotify's tightening API — which the
planned Home Assistant / Music Assistant backend hedges against.

### 1. Prior art — a close DIY sibling and a discontinued commercial twin

- **Knobby** (Milo Winningham) is the nearest open-source analog: an ESP32 + a
  single rotary encoder + small display acting as a **Spotify Connect remote** —
  it controls playback on *other* devices (Echo, Sonos, …), exactly this project's
  "the device is the controller, not the speaker" model. Spin the knob to browse
  6,000+ genres plus your playlists (preloaded from "Every Noise at Once"). Open
  source. ([hackaday.io](https://hackaday.io/project/184630-knobby-a-little-remote-a-lot-of-possibilities),
  [hackster](https://www.hackster.io/milo-winningham/knobby-a-little-spotify-remote-for-a-lot-of-music-d64977))
  *Differentiator for us:* a far richer UI (4.3" screen, themed skins, Cover Flow,
  direct Sonos UPnP, the haptic knob) vs Knobby's deliberately minimal one-encoder
  design. The Connect-remote control model is shared.
- **ThingPulse ESP32 Spotify Remote** (Color Kit Grande) is another ESP32 Spotify
  remote in the same space. ([thingpulse](https://thingpulse.com/esp32-spotify-remote-with-the-color-kit-grande/))
- **Spotify Car Thing** is the commercial twin: a dedicated Spotify controller
  with a screen, a rotary knob, 4 preset buttons and a back button (Amlogic SoC).
  Spotify discontinued it (2022) then **remotely bricked it on 2024-12-09**, which
  spawned a custom-firmware revival — **DeskThing**, **GlanceThing**, **Nocturne**,
  Thing Labs — that reflash the hardware into desktop Spotify controllers /
  glanceable displays. ([techissuestoday](https://techissuestoday.com/spotify-car-thing-deskthing-and-other-custom-firmware-solutions/),
  [Car Thing — Wikipedia](https://en.wikipedia.org/wiki/Car_Thing))
  *Takeaways:* (1) the concept is validated — clear demand for a dedicated physical
  Spotify controller; (2) DeskThing/Nocturne are a free source of proven
  small-screen now-playing + browse UX to borrow from; (3) cautionary tale —
  Spotify killed *its own* hardware and bricked it remotely.

### 2. Spotify-API platform risk — real, but our core endpoints are safe

- On **2024-11-27** Spotify deprecated a swath of Web API endpoints **for new
  apps**: `audio-features`, `audio-analysis`, `recommendations`, related-artists,
  featured/category playlists, 30-second previews, and algorithmic/editorial
  playlists. Existing approved apps are grandfathered. Stated reason "security";
  widely read as limiting AI/scraping. ([Spotify dev blog](https://developer.spotify.com/blog/2024-11-27-changes-to-the-web-api),
  [TechCrunch](https://techcrunch.com/2024/11/27/spotify-cuts-developer-access-to-several-of-its-recommendation-features/))
- **The endpoints this device depends on are NOT affected**: the player/control
  endpoints — `GET /me/player`, `play`/`pause`/`next`/`previous`/`seek`/`volume`,
  `/me/player/devices`, transfer-playback — remain available (they need a Premium
  account). Core function is safe *today*.
- But the direction (deprecations + Premium-gating + bricking the Car Thing) is a
  genuine platform risk. **The planned Home Assistant + Music Assistant backend
  (Phase 3) is the hedge** — it decouples from depending directly on Spotify's API
  goodwill, and Music Assistant can drive multiple sources. A second strategic
  argument for Phase 3 beyond Sonos.

### 3. The haptic knob is proven, RP2040-FOC-capable ground

- The planned input follows **SmartKnob** (Scott Bezek): a BLDC gimbal motor + a
  magnetic encoder + FOC that synthesises **software-defined detents and endstops**
  (the motor can briefly push back against motion for force feedback). SmartKnob's
  firmware is ~3 FreeRTOS tasks incl. a Motor Task doing FOC + detent physics;
  host interfaces are Web Serial + Python. ([smartknob — GitHub](https://github.com/scottbez1/smartknob))
- **FOC on the RP2040 is feasible and documented** — exactly the daughterboard
  plan in this file. SimpleFOC officially supports RP2040/RP2350, there's a
  Pico + BLDC + encoder **haptic-knob tutorial**, and FOC works with just an
  encoder (no current sensing required). ([SimpleFOC RP2040 docs](https://docs.simplefoc.com/rpi_mcu),
  [Hackaday — Motors Make The Best Knobs](https://hackaday.com/2025/10/09/motors-make-the-best-knobs-with-simplefoc/))
  *De-risks the RP2040 FOC decision* (caveat: the Pico GPIO budget — 3 PWM + 1 EN
  per motor — is fine for our single motor).
- **The music-control UX win is context-dependent feel:** chunky detents while
  flicking the album carousel, fine smooth steps for volume, free-spin for
  scrubbing, hard endstops at list ends, a click/bump on select. A single knob
  that *physically reconfigures per screen* is what a touch-only device can't
  match. Adjacent devices (Surface Dial, Nuimo, Griffin PowerMate) share the same
  turn / press / long-press vocabulary.

### 4. UI/UX: Cover Flow tradeoffs + the design-language lineage

- **Cover Flow** (which we implement): Apple removed it (iTunes 11 in 2012; iOS 7
  music → tiled art; macOS Mojave Finder → Gallery view), largely due to a
  **Mirror Worlds patent settlement** and the mid-2010s flat-design shift away
  from heavy animation/reflections — not purely usability. Its weakness is scanning
  *large* libraries; its strength (and why users mourned it) is exactly
  **album-cover browsing that feels like flipping through LPs** — our use case. So
  Cover Flow is a defensible, on-theme choice here; keep the faster Carousel/Focus
  styles for large lists (already done). ([Cover Flow — Wikipedia](https://en.wikipedia.org/wiki/Cover_Flow),
  [512 Pixels history](https://512pixels.net/2023/10/the-history-of-cover-flow/))
- **The theme system maps onto a real design lineage** worth leaning into:
  - **Braun / Dieter Rams** — functionalism, "weniger, aber besser" (less but
    better), the 10 principles, minimal labels + standout physical controls. (Our
    BASIC theme sits here.)
  - **Teenage Engineering** — the modern Braun homage (OP-1 / TP-7): minimal,
    tactile, playful-yet-functional. The "TE look" already in the backlog.
  - **Nothing OS** — dot-matrix "Glyph" monochrome aesthetic (our GLYPH theme).
    Notably the *same design house* (Jesper Kouthoofd / Teenage Engineering) shaped
    the early Nothing phone — so the TE and GLYPH directions are siblings, not
    competitors.
  - **Data-brutalist / teletype / 1-bit** — our PAPER theme.
  ([Braun → Teenage Engineering](https://onlyonceshop.com/blog/from-braun-to-teenage-engineering),
  [TE design](https://designwanted.com/teenage-engineering-creating-design-perspective/))

### 5. Bonus (from the Sonos research): push beats polling

- For local Sonos, **UPnP event subscription (GENA)** lets the speaker *push* state
  changes instead of being polled — the architecture SoCo / node-sonos / Home
  Assistant all use, and the root-cause fix for the poll-stall we patched. Costs an
  HTTP callback server + subscription lifecycle on-device; position (`RelTime`)
  still isn't pushed (poll/estimate it — which our progress bar already does
  locally). Music Assistant already implements all of this, reinforcing that Sonos
  belongs in the Phase 3 HA backend rather than hand-maintained.

### Actionable takeaways

1. **Concept is validated** (Knobby, the Car Thing afterlife) — differentiate on UI
   richness + the haptic knob, not on the control model.
2. **Core Spotify endpoints are safe**, but treat the **HA / Music Assistant
   backend as a strategic hedge**, not just a Sonos convenience.
3. **RP2040 FOC is de-risked** (SimpleFOC + Pico haptic-knob precedent) — proceed
   with the daughterboard; the differentiator is per-screen detent maps.
4. **Cover Flow is the right call** for album browsing specifically; keep the
   lighter styles for big lists.
5. **Lean into the Braun → TE → Nothing lineage** — BASIC/TE/GLYPH are a coherent
   family, not arbitrary skins.

---

## How to use this file

- Edit it as decisions get locked in (move items from "Open" to "Decisions").
- When asking a future Claude Code session for help, point it at this file
  first — it captures all the architectural context that's otherwise scattered
  across the brief, the blog, and chat history.
- If something here contradicts the project blog or the SmartKnob source,
  this file is the more recent state for **this build** specifically.
