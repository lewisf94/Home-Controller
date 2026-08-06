# Knob Notes: RP2040 Haptic Knob Co-MCU Reference

This document records the hardware and protocol facts for the custom RP2040
daughterboard. This board acts as the SmartKnob-style haptic input device for
the Waveshare ESP32-P4 build. This document captures every finding from the
implementation and from the 2026-06-18 verification pass. Read this document
before you repeat this research.

See [KNOB-PARTS.md](KNOB-PARTS.md) for the full parts list, with quantities
and order status for every part named in this document.

---

## Hardware overview

The RP2040 daughterboard carries these parts:

- TMC6300: a three-phase half-bridge gate driver, with six active-high PWM
  inputs.
- A gimbal motor, driven through SimpleFOC field-oriented torque control.
- MT6701QT: a 14-bit magnetic angle encoder, read over an SSI link on SPI.
- A BF350 full Wheatstone bridge, read through an HX711 amplifier, for the
  knob-press strain gauge.
- Four MX hot-swap buttons: active-low, with an internal pull-up resistor.
- An SK6812 RGBW LED set: a 12-LED ring, plus four button LEDs.
- VEML7700: an ambient-light sensor, on I2C.
- MAX17048: a battery fuel gauge, on I2C.

The RP2040 talks to the ESP32-P4 over one UART link, at 921600 baud. All
motor-control, strain-gauge, and LED logic runs entirely on the RP2040. The
P4 sends haptic configuration packets. The RP2040 sends back position and
button events.

---

## RP2040 pin assignments (verified conflict-free)

| Function | Pins | Notes |
|---|---|---|
| Motor U-phase PWM | GPIO0 (UH), GPIO1 (UL) | PWM slice 0. This pair must share one slice. |
| Motor V-phase PWM | GPIO2 (VH), GPIO3 (VL) | PWM slice 1 |
| Motor W-phase PWM | GPIO4 (WH), GPIO5 (WL) | PWM slice 2 |
| Motor enable | GPIO6 | Active-high digital output |
| MT6701 SPI0 | MISO=16, SCK=18, CS=17 | SPI0 default pins. This is the only device on this bus. |
| UART1 to P4 | TX=8, RX=9 | UART1 (Serial2). See the critical note below. |
| HX711 strain gauge | DOUT=10, CLK=11 | Bit-banged digital I/O |
| MX buttons SW1-SW4 | GPIO12, 13, 14, 15 | INPUT_PULLUP, active-low |
| SK6812 ring | GPIO20 | 12 LEDs, NEO_GRBW |
| SK6812 buttons | GPIO21 | 4 LEDs, NEO_GRBW |
| I2C1 (VEML7700 and MAX17048) | SDA=26, SCL=27 | Wire1 / I2C1 |

### Critical: why UART1 (GPIO8/9), not UART0 (GPIO0/1)

GPIO0 and GPIO1 are the default UART0 pins on the RP2040. These same two pins
are also the motor U-phase PWM pins, on slice 0, channels A and B. A GPIO pin
cannot act as a PWM output and a UART TX or RX pin at the same time. An
assignment of both roles to these pins would remove either the U motor phase
or the UART link.

A PWM slice-pairing rule constrains the motor pins. The SimpleFOC
`BLDCDriver6PWM` class needs the high and low pins of each phase on the same
RP2040 PWM slice. This pairing keeps the high and low pins on one shared
counter. This sharing reduces the switching-timing skew between them. A
valid pair is GPIO_n and GPIO_n+1, where n is an even number: 0/1 is slice 0,
2/3 is slice 1, and 4/5 is slice 2. A change to the motor pins would remove
this pairing, so the design moves the UART pins instead.

The `arduino-pico` core maps UART1 TX to GPIO 4, 8, 20, or 24, and UART1 RX
to GPIO 5, 9, 21, or 25. GPIO8 and GPIO9 form the only free UART1 pair, since
GPIO4/5 serve the motor and GPIO20/21 serve the LEDs. In `arduino-pico`,
UART1 is `Serial2`. Configure this UART with `Serial2.setTX(8)` and
`Serial2.setRX(9)`, before the call to `Serial2.begin(921600)`.

### RP2040 UART baud-rate accuracy at 921600 baud

The RP2040 UART uses a fractional divider, oversampled at a rate of 16. At a
125 MHz system clock, the divider value is 125 000 000 divided by
(16 x 921 600), which equals about 8.43. The integer part of this value is 8.
The fractional part, on a 6-bit scale, is the rounded value of 0.43 x 64,
which equals 28. This division gives an actual baud rate near 920 635, an
error of about 0.1%, well inside the plus-or-minus 2% tolerance for this
protocol. A rate of 921 600 baud is reliable on the RP2040. Community reports
confirm this reliability.

---

## ESP32-P4 pin assignments (verified)

| Function | GPIO | J3 header position | Notes |
|---|---|---|---|
| UART1 TX to knob | 32 | Pin 31 (right side) | Confirmed on the schematic |
| UART1 RX from knob | 46 | Bottom-right cluster | Confirmed; GPIO33 is not present on J3 |

### ESP32-P4 GPIO capabilities: no input-only pins

The classic ESP32 reserves GPIO34 through GPIO39 as input-only pins. The
ESP32-P4 has no such restriction: every ESP32-P4 GPIO pin supports both
input and output. GPIO34 through GPIO38 are strapping pins. The chip samples
each strapping pin only at boot time, and each pin acts as a normal GPIO pin
afterward.

The UART1 TX and RX signals can route to any normal GPIO pin, through the
GPIO matrix, with a call to
`uart_set_pin(UART_NUM_1, tx_gpio, rx_gpio, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE)`.
GPIO32 and GPIO46 avoid every one of these reserved ranges:

- SDIO, for the C6 WiFi link: GPIO14 through GPIO19.
- Strapping pins: GPIO34 through GPIO38.
- USB-JTAG: GPIO24 and GPIO25.
- Board-support pins: I2C on GPIO7 and GPIO8. I2S on GPIO9 through GPIO13.
  The amplifier on GPIO53. The LCD on GPIO26 and GPIO27. The touch-reset pin
  on GPIO23. The SD card on GPIO39 through GPIO44.

Test GPIO46 with a continuity check before soldering. This pin sits at the
edge of a hard-to-read region of the schematic, in the bottom-right area of
the J3 header. GPIO47 and GPIO48 are equivalent alternatives, if GPIO46 is
not reachable.

---

## SimpleFOC on the RP2040: verified behaviour

- `BLDCDriver6PWM` is the correct SimpleFOC class for the TMC6300. SimpleFOC
  has no separate `TMC6300Driver6PWM` class; a search of the SimpleFOC source
  code confirms zero matches for that name. The TMC6300 is a standalone gate
  driver, controlled by six independent active-high logic inputs. The generic
  `BLDCDriver6PWM` class controls this driver correctly.
- Constructor argument order:
  `BLDCDriver6PWM(Ah, Al, Bh, Bl, Ch, Cl, enable)`, with the high and low
  argument of each phase next to each other. The firmware code uses this
  order.
- PWM polarity: the `INxH` and `INxL` inputs of the TMC6300 are both active-high.
  The SimpleFOC default configuration, with neither the
  `SIMPLEFOC_PWM_HIGHSIDE_ACTIVE_HIGH` nor the `_LOWSIDE_ACTIVE_HIGH` build
  flag set, assumes an active-high signal on both sides. This default matches
  the TMC6300, so the firmware sets no polarity flag.
- FOC loop frequency: a 5 kHz loop is achievable on RP2040 core 1, with this
  magnetic encoder. This rate is the target for `motor_task_loop()`.
- `__not_in_flash_func`: this is the correct Pico SDK macro for code that
  must run from RAM. The firmware marks `_compute_torque` and
  `motor_task_loop` with this macro, to avoid a cache-miss delay when core 0
  runs an SPI, I2C, or Serial operation. A flash write from core 0, for
  example an EEPROM or LittleFS write, pauses the flash-execute access of
  core 1 completely. Treat this pause as a certain, momentary halt of the
  motor control loop.
- Known RP2040 SimpleFOC issues, neither of which affects this design:
  - An I2S and SimpleFOC six-PWM conflict, reported in February 2025. This
    issue does not apply, since the RP2040 daughterboard has no I2S use.
  - A SimpleFOC drivers document states that the MT6701 cannot share an SPI
    bus with another device. This limit does not apply here, since the
    MT6701 is the only device on SPI0.

---

## MT6701QT magnetic encoder (SSI)

- Use the `MagneticSensorMT6701SSI` class from the SimpleFOC Drivers library
  (the `Arduino-FOC-drivers` git dependency). Do not use the generic
  `MagneticSensorSPI(cs, 14, 0x3FFF)` class. That generic class uses the
  AS5048 register-read convention. This convention corrupts the 25-bit SSI
  frame of the MT6701 (one ignored bit, 14 angle bits, 4 status bits, and 6
  CRC bits).
- SPI mode: SPI_MODE2, with CPOL=1 and CPHA=0. In this mode, the clock line
  idles high, and the device samples data on the falling edge. The library
  sets this mode automatically.
- Clock rate: the default rate is 1 MHz, and the maximum rate is 8 MHz. A
  rate of 1 MHz is sufficient for a 5 kHz FOC loop. One 25-bit read at 1 MHz
  takes about 25 microseconds, well under the 200-microsecond FOC period.
- Init API: call `s_sensor.init(&SPI)`, and pass the `SPIClass` pointer. Make
  this call after `SPI.setRX()`, `SPI.setSCK()`, and `SPI.begin()`. No delay
  is needed after `SPI.begin()`.
- Output: `s_sensor.getAngle()` returns an angle in radians, in the range
  zero to two-pi.
- MT6701QT compared to MT6701: the two parts are functionally identical for
  SSI operation. QT marks the QFN package variant. Both parts use the same
  frame format, the same SPI mode, and the same library.
- Frame format, 24 bits total: bits 23 through 10 hold the 14-bit angle, bits
  9 through 6 hold a 4-bit status field, and bits 5 through 0 hold a 6-bit
  CRC value (polynomial X^6+X+1). The SimpleFOC library extracts the angle
  bits, and does not check the CRC value. This omission is a common
  simplification. For a haptic knob, the next FOC cycle corrects an
  occasional bad reading, so this omission is acceptable.

---

## VEML7700 ambient-light sensor

- The daughterboard reads this sensor through the Adafruit VEML7700 library,
  on I2C1 (SDA=26, SCL=27, `Wire1` on the RP2040). The RP2040 firmware calls
  `s_veml.begin(&Wire1)` during setup.
- I2C address: 0x10. This address is the Adafruit library default, and the
  fixed address for this part from the datasheet.
- Poll rate: the RP2040 reads a new lux value every 2000 ms
  (`LUX_POLL_MS`), through a call to `s_veml.readLux()`. The firmware sends
  this value to the P4, in the `ambient_lux` field of `KnobState` (protobuf
  tag 6).
- Brightness mapping on the P4: the function in `knob_input.c` maps 0 lux to
  10% panel brightness, and maps 1000 lux to 100% panel brightness. This
  function clamps the result at 100%.
- Update gate: a `KnobState` message arrives from the RP2040 about every
  5 ms during a knob scroll, but the lux value inside each message changes
  only every 2000 ms. The P4 writes a new brightness value only when the
  mapped percentage moves by 3 percentage points or more. This gate avoids
  hundreds of redundant PWM writes during a single scroll.
- Open item: this lux-derived brightness value, and the existing idle
  auto-dim feature, both drive the panel duty cycle today, with no
  coordination between the two. A future change should make the idle
  auto-dim feature scale this lux-derived base value. This change would
  replace the current state, where each feature owns the duty cycle on its
  own.

---

## UART protocol

Transport: UART1 at 921600 baud, 8 data bits, no parity, 1 stop bit, and no
flow control.

Framing: the firmware encodes a nanopb protobuf message, appends a 4-byte
CRC32 value, applies COBS encoding, then appends a `0x00` frame delimiter.

### CRC32

Both sides produce the standard IEEE 802.3 and zlib CRC32 value. Both sides
produce the check value `0xCBF43926` for the input string "123456789". The
two implementations agree, and this document verifies that agreement:

| Side | Implementation |
|---|---|
| ESP32-P4 | `esp_rom_crc32_le(0, data, len)`, an ESP-IDF ROM function. This function inverts the seed value on entry, and inverts the result value on exit. This behaviour equals the standard zlib algorithm, with an initial seed of 0xFFFFFFFF and a final XOR of 0xFFFFFFFF. |
| RP2040 | A software implementation, with bit-reversal, the reflected polynomial 0xEDB88320, a seed of 0xFFFFFFFF, and a final `~crc` step. |

Do not call `esp_rom_crc32_le(0xFFFFFFFF, ...) ^ 0xFFFFFFFF`. This call
applies the seed inversion twice, and produces an incorrect result. This
error drops every packet, with no error message.

### COBS

The firmware uses standard Consistent Overhead Byte Stuffing. The encode and
decode functions are verified correct, including the 254-byte full-run edge
case (a segment with code value 0xFF). The encode function returns the total
length, including the `0x00` delimiter byte. Each caller passes this full
length to `write()` or to `uart_write_bytes()`.

### nanopb

The firmware vendors nanopb version 0.4.9.1, in two locations:

- `rp2040/lib/nanopb/`: the PlatformIO library copy.
- `waveshare/esp-idf/components/nanopb/`: the ESP-IDF local component copy,
  named `nanopb`.

The `nanopb/nanopb` package does not exist on the ESP component registry.
Neither project depends on that registry entry; both projects use the
vendored copy of the runtime.

The firmware commits the generated files, `home_controller.pb.h` and
`home_controller.pb.c`, at `proto/home_controller.pb.h`. Build scripts copy
these files to `rp2040/src/proto_gen/` and to `waveshare/esp-idf/main/`. Do
not regenerate these files unless the `.proto` schema changes.

### Retry and acknowledgment

The P4 stores the last `ToKnob` message it sent, and retransmits this
message every 250 ms, through a timer created with `xTimerCreate("knob_retry",
...)`. The P4 continues this retry until the RP2040 echoes the matching nonce
in a `FromKnob.ack` field. The receive task clears the retry once it reads
the matching nonce.

---

## Dual-core init order (RP2040)

The `setup()` function, on core 0, and the `setup1()` function, on core 1,
run at the same time. The code must initialize the cross-core critical
section before either core uses that section.

```
core 0 setup():   motor_shared_init()  -> critical_section_init() + s_cs_ready=true
                  interface_task_init()
core 1 setup1():  motor_task_init()    -> runs FOC, uses the lock (safe: ready-flag guards it)
```

The code must call `motor_shared_init()` from the core-0 `setup()` function
first. Each call to `critical_section_enter_blocking()` checks the
`s_cs_ready` flag first. This check makes each such call a safe no-op, if the
FOC loop on core 1 starts before the lock is ready.

---

## Files added

| File | Purpose |
|---|---|
| `proto/home_controller.proto` | The schema for `KnobConfig`, `KnobState`, `ToKnob`, and `FromKnob`. |
| `proto/home_controller.pb.h/.c` | The pre-generated nanopb output. |
| `rp2040/platformio.ini` | The RP2040 PlatformIO project file. |
| `rp2040/src/main.cpp` | The dual-core entry point. |
| `rp2040/src/motor_task.h/.cpp` | The FOC loop and the detent physics (Apache 2.0, with SmartKnob attribution). |
| `rp2040/src/interface_task.h/.cpp` | The UART protocol, the sensor reads, and the LED control, on core 0. |
| `rp2040/src/proto_gen/` | A copy of the generated `.pb` files. |
| `rp2040/lib/nanopb/` | The vendored nanopb 0.4.9.1 runtime. |
| `waveshare/components/p4_shared/knob.c` (with `include/knob.h`) | The P4-side UART driver, used by both builds (Apache 2.0, with SmartKnob attribution). |
| `waveshare/components/p4_shared/knob_input.c` (with `include/knob_input.h`) | The context-aware input mapper, used by both builds. |
| `waveshare/components/p4_shared/home_controller.pb.c` (with `include/home_controller.pb.h`) | A copy of the generated `.pb` files. |
| `waveshare/esp-idf/components/nanopb/` | The vendored nanopb 0.4.9.1 runtime for ESP-IDF, shared into the HA build through `EXTRA_COMPONENT_DIRS`. |
| `NOTICE` | The Apache 2.0 section 4(d) attribution, for SmartKnob, SimpleFOC, and nanopb. |

The knob source files moved from `waveshare/esp-idf/main/` into the shared
`p4_shared` component. One copy of these files now serves both the direct
Spotify build and the Home Assistant build.

### The `KNOB_ENABLED` compile flag

The `main.c` file of each build places the call to `knob_input_start()`
behind an `#if KNOB_ENABLED` guard. The default value is 0. Since 2026-07-14,
the `main/CMakeLists.txt` file of each build forwards this flag, so a
command-line build can set the flag with no source change:

```
idf.py build -DKNOB_ENABLED=1    # turn the knob support on
idf.py build -DKNOB_ENABLED=0    # turn the knob support off again
```

The CMake cache stores this flag value. A build that omits the flag keeps
the previous value. Set the flag explicitly to turn knob support off.

When this flag is 0, the firmware never configures the UART, and never
touches a knob-related GPIO pin. A P4 build with no knob hardware attached
runs correctly, with no change in behaviour, when this flag is 0.

---

## Pre-flight code review, 2026-07-05: two findings, both fixed

This review covers `rp2040/src/main.cpp`, `rp2040/src/motor_task.cpp`,
`rp2040/src/interface_task.cpp`, `waveshare/components/p4_shared/knob.c`,
`waveshare/components/p4_shared/knob_input.c`, and
`proto/home_controller.proto`. The review is a read-only review, since no
circuit board existed yet at this date. The review found two real issues,
and the project fixed both issues on the same day. Neither fix has a
hardware test yet, since no circuit board existed. The P4-side changes are
build-verified on both Waveshare targets. A PlatformIO toolchain was not
available in the environment that produced the RP2040 fix, so that fix has
no compile check yet. Review the RP2040 fix with care at the first real
build.

### Finding 1: no angle-unwrap handling in `_compute_torque()` (significant), fixed

The function `MagneticSensorMT6701SSI::getAngle()` returns an angle in
radians, in the range zero up to (but not including) two-pi. This value
wraps at each physical revolution. The function `_compute_torque()`, in
`motor_task.cpp`, computed
`raw = (current_angle - s_angle_reference) / position_width_radians`
directly, with no unwrap or accumulation step between calls. At the moment
`current_angle` crosses the two-pi boundary relative to `s_angle_reference`,
the computed delta jumps. This delta jumps by plus or minus two-pi, instead
of the true small step. This jump is a sudden torque discontinuity: a kick,
or a snap to the wrong detent.

Exposure by menu, from the `_send_*_config` functions in `knob_input.c`:

- MENU_VOLUME (`_send_volume_config`, at 3.6 degrees per detent, over a
  range of 0 to 100): this menu always wraps. The full 0-to-100 range equals
  exactly 360 degrees, or two-pi. The volume anchor is set once, at menu
  activation, in `_activate_menu()`, and the code never re-anchors this
  value per detent. A normal turn from 0 to 100 crosses the wrap point.
- MENU_NOW_PLAYING (`_send_now_playing_config`, at 5 degrees per detent, for
  up to `duration_ms / 500` detents): this menu wraps for any track longer
  than about two minutes (72 degrees times that many detents), for the same
  reason as MENU_VOLUME: no per-detent re-anchor.
- MENU_ALBUMS (`_send_albums_config`, at 10 degrees per detent): this menu
  is partly protected. The function `_on_state()` calls
  `_send_albums_config(pos)` after every detent, and this call re-anchors
  `s_angle_reference` on the RP2040, once the round trip completes. The
  local FOC loop runs at 5 kHz, while a re-anchor needs a full UART round
  trip: the P4 processes a `KnobState` message, sends a `KnobConfig`
  message, and the RP2040 decodes and applies that message on its next
  tick. A fast multi-detent turn can outrun this re-anchor, and cross the
  wrap point before the re-anchor catches up. The browser design of this
  project favours fast scrolling by flick. Treat this case as a real,
  expected case, not a rare edge case.

Fix applied, in `rp2040/src/motor_task.cpp`: the function `motor_task_loop()`
now keeps a persistent value, `s_unwrapped_angle`, and updates this value on
every tick. The function `_wrap_delta()` wraps only the change within one
tick. This one-tick change stays small relative to two-pi, so each step
stays unambiguous, even though the absolute angle is not. Both
`s_angle_reference` and the parameter of `_compute_torque()`, renamed to
`unwrapped_angle` for clarity, now use this continuous value, in place of a
raw `getAngle()` reading. This single change fixes all three menus, with no
dependency on re-anchor frequency. The fix defines a local constant,
`KNOB_PI`, in place of `M_PI`. The RP2040 codebase had no prior use of
`M_PI`. The availability of this constant on this toolchain was not
confirmed, since a PlatformIO toolchain was not available when the fix was
written.

First-flash check: turn the volume knob through a full 0-to-100 sweep. This
sweep is certain to cross the old wrap point. Confirm no torque kick or
glitch during the sweep.

### Finding 2: Albums and Now-Playing menus anchor to position zero on activation, not to the live value (moderate), fixed

The function `_activate_menu()` correctly anchors MENU_VOLUME to the live
device volume, read through `ui_get_volume()`. This anchor stops the first
detent from snapping playback to an incorrect value. A comment at that call
explains the reason. MENU_ALBUMS and MENU_NOW_PLAYING did not receive the
same treatment. The config-builder call of each menu used a fixed value of
0. This fixed value ignored which album was centred, and ignored how far
into the track playback had reached.

The delta-based scroll function, `ui_scroll_browser(delta)`, is not affected
by this issue, since its steps are relative: scrolling still moves in the
correct direction. The issue affects the endstop feel instead: the RP2040
treats position 0 as the point where the menu was activated. Consider a
turn backward from album 40 of 56. This turn reaches a false endstop after
about 0 detents, instead of after the 40 albums that are actually behind the
current position. A turn forward, in the same case, feels like it has 55
detents of range remaining. This false feeling occurs even when the browser
is already near the end of the list. The Now-Playing menu has the same
issue, relative to the actual playback position.

Fix applied: the function `_activate_menu()`, in `knob_input.c`, now anchors
MENU_NOW_PLAYING to the value `ui_get_progress_ms() / SCRUB_STEP_MS`. That
getter function already existed; the fix only adds the call at this point.
The fix anchors MENU_ALBUMS to a new function,
`ui_get_centered_album_index()`, added to the `ui_*` interface in `ui.c` and
`ui.h`. This function matches the lock-and-read pattern of
`ui_get_volume()` exactly. Both menus fall back to a default value of 0, if
a read happens before the first poll or browser build. This fallback
matches the existing -1 fallback for Volume. This fix is build-verified on
both Waveshare targets.

### Minor observations, updated 2026-07-14

- `ToKnob` compile-time size guard: resolved as not possible, and documented
  instead. nanopb cannot compute a `ToKnob_size` constant, since the LED
  byte fields use `pb_callback_t`, an unbounded type; the generated `pb.h`
  file states this limit directly. The function `_send_packet()`, in
  `knob.c`, now carries a hand-computed worst-case comment (about 140 bytes,
  against a 256-byte buffer). The function `pb_encode()` still fails with a
  clear log message, if a future schema change exceeds this bound.
- Unacked-configuration diagnostic: complete. The retry timer in `knob.c`
  now logs a warning after about 2 seconds with no acknowledgment: "knob not
  acking (nonce N, M retries) - link down or unplugged?" This warning then
  repeats about every 30 seconds. This warning makes a disconnected or
  miswired link visible on the test bench, in place of an unexplained retry
  loop.
- The field `ToKnob.request_state` exists in the proto schema, but no code
  sends or handles this field yet. This gap is not a bug: the RP2040 already
  sends its state on its own, every `STATE_TX_MS` interval, or on a change.
  This field is unused schema surface, kept in reserve for a future "request
  an immediate state push" need.

---

## Pre-hardware setup pass, 2026-07-14: every check possible without the board is complete

This section records preparation work for wiring the daughterboard. Every
item below is build-verified. No item below has run against real knob
hardware yet.

- The RP2040 firmware compiled for the first time, with `pio run`, through a
  PlatformIO installation at `~/.platformio`. The build succeeded: RAM use
  was 4.6% (12,048 bytes), and flash use was 4.6% (95,776 bytes). This build
  produced `rp2040/.pio/build/rp2040/firmware.uf2`. This result closes the
  standing risk that the RP2040 code had never passed a compile check: the
  angle-unwrap fix, the `KNOB_PI` constant, and the `static_assert` line all
  compile correctly.
  - The file `platformio.ini` named two library versions that failed
    dependency resolution. The fix changed `Adafruit VEML7700 Lib` to
    `Adafruit VEML7700 Library`, and changed `Adafruit MAX1704X @ ^1.2.2`,
    a version that never existed, to `@ ^1.0.3`.
- The HA build carried a knob compile fault that had not yet triggered:
  `main.c` called a function named `knob_input_init()`, while the actual
  function name is `knob_input_start()`. This fix corrects the call, and
  changes the fixed `#define KNOB_ENABLED 0` line of the HA build to an
  `#ifndef` guard, matching the direct build.
- The build flag `idf.py build -DKNOB_ENABLED=1` now works correctly. Before
  this fix, the README described this flag. The CMake configuration of
  neither build forwarded the flag to the compiler, so the flag had no
  effect. The `main/CMakeLists.txt` file of each build now translates this
  cache flag into a compile definition. This flag value is sticky in the
  CMake cache; set `-DKNOB_ENABLED=0` explicitly to turn the flag off again.
- Both P4 build targets build correctly with `KNOB_ENABLED=1` set: 28% free
  space on the direct build, and 25% free space on the HA build. Each build
  then returned to the deployable state, with the flag set to 0.
- A bench diagnostic addition: the P4 now logs "knob not acking ... link
  down or unplugged?" after about 2 seconds with no acknowledgment on a
  configuration send, then repeats the log every 30 seconds. A miswired
  UART connection is the most likely first hookup fault, and this log makes
  that fault visible, in place of no message at all.

### Component-list cross-check, against Lewis's build list, 2026-07-14

Every item in the build list matches the firmware, with these notes:

| Item | Firmware status |
|---|---|
| MAX17048, listed address 0x32 | List error. The correct MAX17048 I2C address is 0x36, a fixed address from the datasheet, and the default address in the Adafruit library the firmware uses. No code change is needed; correct the build list before this error affects a PCB design or a debug assumption. |
| VEML7700, address 0x10 | Matches the Adafruit default, on Wire1 (RP2040 SDA=26, SCL=27). This sensor drives the P4's automatic brightness feature, already implemented. |
| TMC6300 six-PWM driver, with a 3.3-microhenry VM inductor | Matches `BLDCDriver6PWM` on GPIO0 through GPIO5, with enable on GPIO6, active-high, with no polarity flag needed. The inductor is a hardware-only part. |
| SparkFun gimbal motor, not yet arrived | The firmware assumes `MOTOR_POLE_PAIRS 7` (a typical value for a 12N14P gimbal motor); confirm this value once the motor arrives. The absence of the motor or the driver does not block a bench test of the UART, the buttons, and the strain gauge: FOC alignment fails gracefully, and core 0 runs regardless. |
| MT6701 SSI encoder, with a diametric magnet | Matches `MagneticSensorMT6701SSI`, the sole device on SPI0 (MISO on GPIO16, SCK on GPIO18, CS on GPIO17). The diametric magnet type is the correct type. |
| BF350 bridge, with an HX711 amplifier | Matches DOUT on GPIO10, CLK on GPIO11. The value `HX711_PRESS_THRESHOLD 5000` is a placeholder, pending calibration. PCB note: tie the HX711 RATE pin HIGH, for an 80-samples-per-second rate. At the default rate of 10 samples per second, a quick tap can go undetected, and long-press timing gains plus or minus 100 ms of jitter. |
| Four MX switches, hot-swap type | Matches GPIO12 through GPIO15, with INPUT_PULLUP set, active-low. The design defers debounce to the hardware stage, for tuning against the real switches. |
| SK6812 ring and button LEDs, with level shifters | Matches GPIO20 (12 ring LEDs) and GPIO21 (4 button LEDs), in GRBW order. Level shifters are a PCB-stage concern; the firmware does not depend on them. |
| A 3.7 V LiPo cell | The TMC6300's VM input range is 2 V to 11 V. Running the motor directly from the cell is the same arrangement SmartKnob uses. |
| MAX17048, not yet ordered | This is not a blocker. With the chip absent, `begin()` fails, and the battery reading stays at zero or an invalid value. This failure reaches only a debug log today. |
| An IMU or a hall-effect sensor, listed as a possible addition | Neither part is in the firmware yet. Free RP2040 GPIO pins for such a part: 7, 19, 22, 28, and 29. An IMU could share I2C1, at address 0x68 or 0x69, with no conflict against the existing 0x10 and 0x36 addresses. The proto schema is append-only, so it supports this addition later. |
| An ESP32-H2 and a Raspberry Pi, as a Thread border router, plus a USB-to-TTL adapter for the C6 | These parts sit outside this firmware's scope; they match the Home Assistant plan. |

### Wiring the UART link: the one cross-connection to get right

| P4 (J3 header) | Direction | RP2040 |
|---|---|---|
| GPIO32 = UART1 TX (J3 pin 31) | to | GPIO9 = UART1 RX |
| GPIO46 = UART1 RX (bottom-right J3 cluster) | from | GPIO8 = UART1 TX |
| GND | both directions | GND (a common ground connection is required) |

Both sides use 3.3 V logic levels, so the UART link needs no level shifter.
Test GPIO46 on J3 with a continuity check before soldering. This pin sits at
the edge of a hard-to-read region of the schematic. GPIO47 and GPIO48 are
drop-in alternatives, if GPIO46 turns out to be unreachable; change the
value of `KNOB_UART_RX_PIN`, in `p4_shared/include/knob.h`, to switch pins.

### Bring-up order, for the first flash

1. Flash the RP2040 alone, over USB, with `pio run -t upload`, or by copying
   `rp2040/.pio/build/rp2040/firmware.uf2` onto the BOOTSEL drive. The
   RP2040 runs on its own at this step; the P4 is not needed yet.
2. Wire the UART connection, from the table above, plus the common ground
   connection. Power on the RP2040.
3. Build and flash the P4, with the knob support turned on:
   `idf.py build -DKNOB_ENABLED=1`, then `idf.py -p COM4 flash monitor`.
4. In the serial monitor, expect the line
   `knob: knob UART init OK (tx=32 rx=46 baud=921600)`, then expect no "knob
   not acking" warnings. A steady flow of acknowledgments proves the link
   works. If the warning appears, swap the TX and RX wires first; this
   swap is the most common wiring fault.
5. Press each MX button, SW1 through SW4, and expect a
   `knob_input: active menu -> N` line for each press.
6. Press the strain sensor, and expect a play or pause toggle. Calibrate the
   value `HX711_PRESS_THRESHOLD` against this test.
7. Once the TMC6300 and the gimbal motor arrive, test the motor: confirm the
   pole-pair count against `MOTOR_POLE_PAIRS 7`. Turn the knob, and expect
   the browser to scroll with detents. Run a full 0-to-100 volume sweep, and
   expect no torque kick; this test validates the angle-unwrap fix at its
   guaranteed wrap point.
8. Confirm that a change in ambient light changes the panel brightness.
   Confirm that a battery percentage value appears in the debug log, once
   the MAX17048 chip is fitted.

### Input conditioning to tune once the hardware exists

These two items stay untuned until the board exists. The correct threshold
values depend on the real strain-bridge gain and the real switch bounce
profile:

- HX711 press hysteresis. The file `interface_task.cpp` currently signals a
  press on a single crossing of `raw > HX711_PRESS_THRESHOLD`. A strain
  reading is noisy near this threshold, so add a Schmitt-trigger pattern: a
  separate release threshold, for example at about 70% of the press
  threshold, once the resting and pressed raw values are known from a real
  test. Without this change, a reading that hovers near the threshold
  produces repeated `press_nonce` increments, and each increment triggers an
  unwanted play or pause toggle.
- MX button debounce. The function `_send_state()` reads a raw
  `digitalRead()` value at each 5 ms transmission interval. A mechanical
  bounce at a button edge can toggle the `button_mask` value several times.
  Each toggle can trigger a separate menu activation from one physical
  press. Add a stable-state debounce of a few milliseconds, per button. The
  `mcp_input` consume-on-read latch of the CYD build is the reference
  pattern for this kind of debounce. Tune the debounce window against the
  real switches.
