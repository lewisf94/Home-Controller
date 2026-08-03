# Project Roadmap

This roadmap describes the current delivery sequence.

Use [PENDING.md](PENDING.md) for open hardware checks.

Use [P4-TODO.md](P4-TODO.md) for the detailed Waveshare backlog.

## Current State

The Waveshare ESP32-P4 is the primary platform.

The Home Assistant baseline is the daily hardware-verified build.

The direct Spotify and Sonos baseline is also hardware-verified.

The latest BOLD and Developer-control batch needs a build and hardware test.

The HIFI theme and the output-transfer batch also need a build and hardware
test.

The RP2040 Pico and MT6701 sensor path is hardware-verified.

The TMC6300 motor test is build-verified.

The CYD builds remain available for maintenance and comparison.

## Build Matrix

| Build | Status | Role |
|---|---|---|
| Waveshare Home Assistant | Verified baseline | Primary daily build |
| Waveshare direct | Verified baseline | Direct Spotify and Sonos build |
| CYD ESP-IDF direct | Requires a new hardware pass | Maintained legacy hardware |
| CYD ESP-IDF HA | Not hardware-tested | HA backend experiment |
| CYD Arduino | Maintenance mode | Compatibility build |
| RP2040 knob | Sensor verified | Haptic-control prototype |

## Phase 1: Complete The Knob Motor Test

Goal: prove the motor and TMC6300 path with a bounded test.

1. Verify the TMC6300 supply connections.
2. Verify the TMC6300 logic connections.
3. Connect the three motor phases.
4. Flash the fail-safe motor-test firmware.
5. Arm the test through USB serial.
6. Run the low-duty phase sequence.
7. Confirm that DIAG stops the bridge.
8. Determine the motor pole-pair count.

Exit criteria:

- The motor moves without excessive current.
- The bridge remains cool.
- DIAG causes a safe shutdown.
- The phase sequence is repeatable.
- The motor pole-pair count is known.

## Phase 2: Complete The Haptic Knob

Goal: connect the production knob firmware to the Waveshare controller.

1. Build the RP2040 firmware.
2. Verify MT6701 angle tracking under the final magnet fixture.
3. Calibrate motor control.
4. Verify the strain input.
5. Verify the button inputs.
6. Verify the LED output.
7. Enable the Waveshare knob option.
8. Verify the UART protocol.
9. Test each menu detent profile.
10. Run a thermal and stability soak.

Exit criteria:

- The angle remains continuous through zero degrees.
- Every menu starts at its live application value.
- The UART link recovers after a reset.
- The motor remains inside the selected current and temperature limits.
- User input remains responsive during network operations.

## Phase 3: Close Waveshare Release Gates

Goal: prove long-duration stability of the current daily build.

1. Run the 60-minute interactive soak.
2. Run the eight-hour playback and idle soak.
3. Test queue mutations.
4. Test light controls during audio.
5. Test built-in audio playback.
6. Test OTA.
7. Test crash reporting.
8. Test runtime cover repair.
9. Build the latest BOLD, HIFI, and Developer-control batch.
10. Test BOLD in both appearances.
11. Test HIFI in both appearances, with each heading font.
12. Test all Developer pages and serial export.
13. Confirm that saved Developer overrides survive the update.
14. Test output transfer to each output type.
15. Compare the browser bench with the hardware display.

Exit criteria:

- No watchdog reset occurs.
- No SDIO allocation failure occurs.
- DMA-capable memory remains above the measured release threshold.
- Home Assistant reconnects without a power cycle.
- OTA preserves device configuration.

## Phase 4: Improve Runtime Library Management

Goal: manage the album library without a development computer.

Current functions:

- Browse saved albums.
- Search for albums.
- Add runtime albums.
- Store runtime covers.
- Sort the combined catalogue.

Planned functions:

- Remove a runtime album.
- Reorder pinned albums.
- Repair missing metadata.
- Repair missing cover art.
- Report storage use.
- Handle a library larger than `MAX_CARDS`.

Exit criteria:

- Every operation has clear on-screen feedback.
- A failed operation does not damage the existing catalogue.
- A reboot preserves the runtime catalogue.
- Both Waveshare backends use the same interface behavior.

## Phase 5: Refresh CYD Builds

Goal: keep the CYD variants reproducible.

1. Reconfigure both ESP-IDF builds.
2. Fix shared-component dependency drift.
3. Build the Arduino variant.
4. Complete the direct-build hardware pass.
5. Complete the Home Assistant hardware pass.
6. Record unsupported functions.

Do not add new CYD-only interface features before this phase passes.

## Phase 6: Custom Hardware

Goal: create a reliable integrated controller after prototype verification.

Required inputs:

- Verified motor current.
- Verified motor pole-pair count.
- Verified magnet geometry.
- Verified TMC6300 thermal behavior.
- Verified strain-sensor circuit.
- Verified USB-C CC resistors.
- Verified battery and charging design.
- Verified connector pinout.

Do not start the final PCB before these values are known.

## Completed Work

The project has completed these major stages:

- CYD direct Spotify controller.
- CYD LVGL migration.
- Waveshare display, touch, Wi-Fi, and Spotify bring-up.
- Waveshare Home Assistant backend.
- Music Assistant and Sendspin support.
- Direct Sonos control.
- Shared Waveshare interface and application components.
- Cover Flow compositor and PPA acceleration.
- Compiled theme fonts.
- BOLD theme, HIFI theme, and browser design bench.
- Live Developer controls.
- Runtime album addition.
- Device credential setup.
- OTA and crash reporting.
- Native RP2040 sensor bring-up.

Git history contains implementation details for each completed stage.

## Engineering Rules

- Keep network calls outside the render and input paths.
- Keep LVGL access under the display lock.
- Keep shared Waveshare behavior in shared components.
- Keep large P4 buffers in PSRAM when permitted.
- Keep mandatory DMA allocations in internal memory.
- Keep the RP2040 motor disabled until explicit arming.
- Test one hardware risk at a time.
- Record measured results before you change a limit.
