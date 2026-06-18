# KNOB-NOTES — RP2040 haptic knob co-MCU reference

Hardware and protocol notes for the custom RP2040 daughterboard that acts as
the SmartKnob-style haptic input device for the Waveshare ESP32-P4 build.
Captures every finding from the implementation and the 2026-06-18 deep-research
verification pass so the same ground doesn't need to be re-covered.

---

## Hardware overview

The RP2040 daughterboard carries:
- **TMC6300** — 3-phase half-bridge gate driver (6 active-high PWM inputs)
- **Gimbal motor** — driven via SimpleFOC FOC torque control
- **MT6701QT** — 14-bit magnetic angle encoder (SSI over SPI)
- **BF350 full Wheatstone bridge + HX711** — knob-press strain gauge
- **4× MX hot-swap buttons** — active-low, internal pull-up
- **SK6812 RGBW** — 12-LED ring + 4 button LEDs
- **VEML7700** — ambient light sensor (I2C)
- **MAX17048** — battery fuel gauge (I2C)

The RP2040 talks to the ESP32-P4 via a single **UART link at 921600 baud**.
All FOC, strain-gauge, and LED logic runs entirely on the RP2040.
The P4 sends haptic config packets; the RP2040 sends back position/button
events.

---

## RP2040 pin assignments (verified conflict-free)

| Function | Pins | Notes |
|---|---|---|
| Motor U-phase PWM | GPIO0 (UH), GPIO1 (UL) | PWM slice 0 — required same-slice pairing |
| Motor V-phase PWM | GPIO2 (VH), GPIO3 (VL) | PWM slice 1 |
| Motor W-phase PWM | GPIO4 (WH), GPIO5 (WL) | PWM slice 2 |
| Motor enable | GPIO6 | Active-high digital out |
| MT6701 SPI0 | MISO=16, SCK=18, CS=17 | SPI0 default pins; only device on this bus |
| **UART1 to P4** | **TX=8, RX=9** | **UART1 (Serial2) — see critical note below** |
| HX711 strain gauge | DOUT=10, CLK=11 | Bit-bang digital I/O |
| MX buttons SW1–SW4 | GPIO12, 13, 14, 15 | INPUT_PULLUP, active-low |
| SK6812 ring | GPIO20 | 12 LEDs, NEO_GRBW |
| SK6812 buttons | GPIO21 | 4 LEDs, NEO_GRBW |
| I2C1 (VEML7700 + MAX17048) | SDA=26, SCL=27 | Wire1 / I2C1 |

### CRITICAL: why UART1 (GPIO8/9), not UART0 (GPIO0/1)

GPIO0/1 are the RP2040's UART0 default pins AND the motor U-phase PWM
(slice 0, channels A/B). A GPIO cannot simultaneously be PWM output and UART
TX/RX — assigning both would lose either motor phase U or the UART link.

Motor pins are constrained by the **PWM slice-pairing requirement**: SimpleFOC's
BLDCDriver6PWM needs each phase's high/low pair on the same RP2040 PWM slice
(they share a counter, minimising dead-time skew). Valid pairs are GPIO_n and
GPIO_n+1 where n is even: 0/1=slice0, 2/3=slice1, 4/5=slice2. Moving the motor
would break this pairing, so the UART moves instead.

UART1 in arduino-pico maps TX to {4, 8, 20, 24} and RX to {5, 9, 21, 25}.
GPIO8/9 is the only free UART1 pair (4/5 = motor, 20/21 = LEDs).
In arduino-pico, UART1 = `Serial2`; configure with `Serial2.setTX(8)` /
`Serial2.setRX(9)` before `Serial2.begin(921600)`.

### RP2040 UART baud rate accuracy at 921600

The RP2040 UART uses a 16× oversampled fractional divider.
At 125 MHz system clock: divider = 125 000 000 / (16 × 921 600) ≈ 8.43.
Integer part = 8, fractional = round(0.43 × 64) = 28 (6-bit).
This gives ~920 635 baud (≈ 0.1% error) — well within the ±2% tolerance.
921 600 baud is reliable on RP2040. Confirmed working in community reports.

---

## ESP32-P4 pin assignments (verified)

| Function | GPIO | J3 header position | Notes |
|---|---|---|---|
| UART1 TX → knob | 32 | Pin 31 (right side) | Confirmed on schematic |
| UART1 RX ← knob | 46 | Bottom-right cluster | Confirmed; 33 is NOT on J3 |

### ESP32-P4 GPIO capabilities (no input-only pins)

Unlike the classic ESP32 (where GPIO34–39 were input-only), **all ESP32-P4 GPIOs
are full bidirectional**. The ESP32-P4 has no hardware input-only restrictions.
GPIO34–38 are strapping pins (sampled only at boot; normal GPIO afterwards).

UART1 TX/RX can be routed to any normal GPIO via the GPIO matrix using
`uart_set_pin(UART_NUM_1, tx_gpio, rx_gpio, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE)`.
GPIO32 and GPIO46 are both clear of:
- SDIO (C6 link): GPIO14–19
- Strapping: GPIO34–38
- USB-JTAG: GPIO24–25
- BSP (I2C 7/8, I2S 9–13, amp 53, LCD 26/27, touch-rst 23, SD 39–44)

**Beep-test GPIO46 before soldering** — it sits at the edge of the readable
schematic region (bottom-right of J3). GPIO47 or 48 are equivalent drop-ins if
46 is not accessible.

---

## SimpleFOC on RP2040 — verified behaviour

- **`BLDCDriver6PWM` is the correct class** for the TMC6300. There is no
  `TMC6300Driver6PWM` class in SimpleFOC (confirmed: zero occurrences in
  SimpleFOC source). The TMC6300 is a standalone gate driver driven by 6
  independent active-high logic inputs; SimpleFOC's generic `BLDCDriver6PWM`
  handles it correctly.
- **Constructor argument order:** `BLDCDriver6PWM(Ah, Al, Bh, Bl, Ch, Cl, enable)` —
  per-phase interleaved, which is what the code uses.
- **PWM polarity:** The TMC6300's INxH / INxL inputs are both active-high.
  SimpleFOC's default (no `SIMPLEFOC_PWM_HIGHSIDE_ACTIVE_HIGH` / `_LOWSIDE_ACTIVE_HIGH`
  build flags) assumes active-high for both sides — this matches the TMC6300.
  No polarity flags are needed.
- **FOC loop frequency:** 5 kHz is achievable on RP2040 core 1 with a magnetic
  encoder. This is the target for `motor_task_loop()`.
- **`__not_in_flash_func`**: Correct pico-sdk macro to run code from RAM. Marked
  on `_compute_torque` and `motor_task_loop` to avoid XIP cache-miss jitter when
  core 0 does SPI / I2C / Serial operations. Any flash write from core 0 (e.g.
  EEPROM/LittleFS) pauses core 1 XIP entirely — treat as guaranteed motor freeze.
- **Known RP2040 SimpleFOC quirks** (does not affect this design):
  - I2S conflicts with SimpleFOC 6PWM on RP2040 (Feb 2025 issue) — not relevant,
    no I2S on the RP2040 daughterboard.
  - MT6701 cannot share an SPI bus with other devices (SimpleFOC drivers README) —
    not relevant, MT6701 is the sole device on SPI0.

---

## MT6701QT magnetic encoder (SSI)

- **Use `MagneticSensorMT6701SSI`** from the SimpleFOC Drivers library
  (`Arduino-FOC-drivers` git dependency). Do NOT use the generic
  `MagneticSensorSPI(cs, 14, 0x3FFF)` — that uses the AS5048 register-read
  convention and mangles the MT6701's 24-bit SSI streaming frame.
- **SPI mode:** SPI_MODE2 (CPOL=1, CPHA=0): clock idles high, data sampled on
  falling edge. The library sets this automatically.
- **Clock:** Default 1 MHz; maximum 8 MHz. 1 MHz is sufficient for a 5 kHz FOC
  loop (a single 24-bit read at 1 MHz takes ≈ 24 µs; well under the 200 µs
  FOC period).
- **Init API:** `s_sensor.init(&SPI)` — pass the SPIClass pointer. Call after
  `SPI.setRX/setSCK/begin()`. No delay needed after `SPI.begin()`.
- **Output:** `s_sensor.getAngle()` returns radians in [0, 2π].
- **MT6701QT vs MT6701:** Functionally identical for SSI operation. QT = QFN
  package variant. Same frame format, same SPI mode, same library.
- **Frame format (24 bits):** bits 23–10 = 14-bit angle, bits 9–6 = 4-bit status,
  bits 5–0 = 6-bit CRC (poly X^6+X+1). The SimpleFOC library extracts the angle
  bits and **does not validate the CRC** (common simplification). For a haptic
  knob, an occasional corrupted reading is corrected by the next FOC cycle — this
  is acceptable.

---

## UART protocol

**Transport:** UART1 @ 921600 baud, 8N1, no flow control.
**Framing:** nanopb protobuf → 4-byte CRC32 appended → COBS encoded → 0x00
frame delimiter.

### CRC32

Both sides produce standard IEEE 802.3 / zlib CRC32 (check value `0xCBF43926`
for "123456789"). **They agree** — verified:

| Side | Implementation |
|---|---|
| ESP32-P4 | `esp_rom_crc32_le(0, data, len)` — ROM function; internally inverts seed on entry and result on exit, equivalent to standard zlib with initial seed 0xFFFFFFFF and final XOR 0xFFFFFFFF |
| RP2040 | Software bit-reversal, poly 0xEDB88320 (reflected), seed 0xFFFFFFFF, final `~crc` |

Do NOT call `esp_rom_crc32_le(0xFFFFFFFF, ...) ^ 0xFFFFFFFF` — that double-inverts
and produces a different result, silently dropping every packet.

### COBS

Standard Consistent Overhead Byte Stuffing. The encode/decode implementations
are verified correct including the 254-byte full-run edge case (code=0xFF segment).
The encode function returns the **total length including the 0x00 delimiter**
(callers pass the full length to `write()`/`uart_write_bytes()`).

### nanopb

Version 0.4.9.1 vendored:
- `rp2040/lib/nanopb/` — PlatformIO library
- `waveshare/esp-idf/components/nanopb/` — ESP-IDF local component (name `nanopb`)

The `nanopb/nanopb` package does **not** exist on the ESP component registry.
No registry dependency — both projects use the vendored runtime.

Generated files (`home_controller.pb.h/.c`) committed at
`proto/home_controller.pb.h`, copied to `rp2040/src/proto_gen/` and
`waveshare/esp-idf/main/`. Do not regenerate unless the `.proto` schema changes.

### Retry and ACK

P4 stores the last sent `ToKnob` and retransmits every 250 ms via
`xTimerCreate("knob_retry", ...)` until the RP2040 echoes the nonce in a
`FromKnob.ack` field. Cleared in the RX task once the matching nonce is received.

---

## Dual-core init order (RP2040)

`setup()` and `setup1()` run **concurrently** on RP2040 (core 0 and core 1).
The cross-core critical section must be initialised before either core uses it.

```
core 0 setup():   motor_shared_init()  → critical_section_init() + s_cs_ready=true
                  interface_task_init()
core 1 setup1():  motor_task_init()    → runs FOC, uses the lock (safe: ready-flag guards it)
```

`motor_shared_init()` MUST be called from core-0 `setup()` FIRST.
All `critical_section_enter_blocking()` calls are guarded by `s_cs_ready` so
core 1's FOC loop sees a harmless no-op if it races ahead before the lock is ready.

---

## Files added

| File | Purpose |
|---|---|
| `proto/home_controller.proto` | Schema (KnobConfig / KnobState / ToKnob / FromKnob) |
| `proto/home_controller.pb.h/.c` | Pre-generated nanopb output |
| `rp2040/platformio.ini` | RP2040 PlatformIO project |
| `rp2040/src/main.cpp` | Dual-core entry point |
| `rp2040/src/motor_task.h/.cpp` | FOC loop, detent physics (Apache 2.0 + SmartKnob attribution) |
| `rp2040/src/interface_task.h/.cpp` | UART protocol, sensors, LEDs (core 0) |
| `rp2040/src/proto_gen/` | Copy of generated .pb files |
| `rp2040/lib/nanopb/` | Vendored nanopb 0.4.9.1 runtime |
| `waveshare/esp-idf/main/knob.h/.c` | P4-side UART driver (Apache 2.0 + SmartKnob attribution) |
| `waveshare/esp-idf/main/knob_input.h/.c` | Context-aware input mapper |
| `waveshare/esp-idf/main/home_controller.pb.h/.c` | Copy of generated .pb files |
| `waveshare/esp-idf/components/nanopb/` | Vendored nanopb 0.4.9.1 for ESP-IDF |
| `NOTICE` | Apache 2.0 req 4(d) attribution (SmartKnob, SimpleFOC, nanopb) |

### `KNOB_ENABLED` compile flag

`waveshare/esp-idf/main/main.c` gates `knob_input_start()` behind:
```c
#if KNOB_ENABLED
    knob_input_start();
#endif
```
Default is 0 (undefined = off). The UART is never configured and no GPIO is
touched when the flag is absent. P4 builds without the knob hardware are
completely unaffected.

---

## Hardware verification checklist

- [ ] Beep-test GPIO46 on J3 header before soldering the harness
- [ ] Confirm motor pole pairs match `MOTOR_POLE_PAIRS 7` (or update)
- [ ] Calibrate `HX711_PRESS_THRESHOLD 5000` for the actual strain-gauge bridge
- [ ] Run `KNOB_ENABLED=1 idf.py build flash monitor` and confirm knob UART init OK log
- [ ] Turn knob → album carousel scrolls, knob re-anchors to new position
- [ ] Strain-gauge press → play/pause toggles
- [ ] Battery % and ambient lux appear in serial log
