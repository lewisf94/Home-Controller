# Waveshare ESP32-P4 Backlog

This document lists open work for the two Waveshare builds.

Use [PENDING.md](PENDING.md) for hardware-verification debt.

## Current Baseline

The direct and Home Assistant baselines are hardware-verified.

The builds share the interface, audio, album, credential, knob, and reliability
components.

These functions are complete:

- Display, touch, Wi-Fi, and Spotify communication.
- Music Assistant, Sendspin, Sonos, and Spotify Connect control.
- Carousel, Focus, and Cover Flow.
- BASIC, GLYPH, PIXEL, PAPER, BOLD, and HIFI themes.
- Dark and light appearances.
- PPA display acceleration.
- Runtime album addition and cover storage.
- Settings, interface sounds, OTA support, and crash reports.
- Background reconnect and Home Assistant heartbeat.

Keep `CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT=1`.

The two-unit setting causes a boot loop with PPA acceleration.

## Priority 1: Hardware Verification

Complete these tests before new large interface work:

1. Build both Waveshare targets with the latest theme batch.
2. Test BOLD in both appearances.
3. Test HIFI in both appearances, with each of its four heading fonts.
4. Record the `theme ... ready:` memory line for each of the six modes.
5. Test all six Developer pages.
6. Confirm that saved Developer overrides survive this update.
7. Compare the theme bench with the hardware display.
8. Run the Home Assistant interactive soak for 60 minutes.
9. Run the playback and idle soak for eight hours.
10. Play audio through the built-in speaker.
11. Test queue add, search, and clear operations.
12. Stress light controls during audio playback.
13. Test output transfer to a Music Assistant speaker.
14. Test output transfer to a Spotify Connect device.
15. Start an output transfer while the built-in speaker plays.
16. Test OTA installation from a known firmware image.
17. Confirm the next boot reports and clears a test crash.
18. Repair an older runtime album that has no cover.

Record each result in [TESTING.md](TESTING.md).

## Priority 2: RP2040 Knob

The RP2040 Pico and MT6701 sensor tests are hardware-verified.

The fail-safe TMC6300 motor test is build-verified.

Complete this work in sequence:

1. Verify the TMC6300 supply rails.
2. Verify the TMC6300 logic rails.
3. Run the armed low-duty motor test.
4. Confirm that DIAG causes an immediate shutdown.
5. Determine the motor pole-pair count.
6. Test the complete haptic-knob firmware.
7. Enable the Waveshare UART link.
8. Verify every menu detent profile.
9. Calibrate the strain-input threshold.

Use [rp2040/bringup/README.md](../rp2040/bringup/README.md) for the test sequence.

## Priority 3: Reliability

- Add a repeatable bad-token recovery test.
- Add a repeatable router-loss recovery test.
- Measure the lowest DMA-capable memory during the long soak.
- Confirm that Cover Flow does not trigger the task watchdog.
- Confirm that runtime cover repair does not reduce DMA reserve.
- Confirm that repeated output changes do not leak memory.

Do not reduce the 64 KB internal-memory reserve without measurements.

## Performance Work

Measure each change before you keep it.

- Profile the PPA rotation, blit, and flush path.
- Test a shorter LVGL refresh period.
- Measure the GLYPH Cover Flow post-processing pass.
- Remove LittleFS art staging only after the RAM path passes a soak.
- Reuse more temporary buffers only when the memory trace shows fragmentation.

The Cover Flow rasterizer is already row-major and uses covered-span clipping.

Do not remove its side-card limit without a performance test.

## Interface Work

- Improve feedback for a restricted Spotify device.
- Verify the first automatic album selection after startup.
- Review previous-track behavior after three seconds of playback.
- Tune the 60-second and 300-second dim thresholds on hardware.
- Improve the setup flow for expired or rejected credentials.
- Add remove and reorder operations for runtime albums.

Keep interface changes in the shared Waveshare component.

## Capacity

The Waveshare builds use two 10 MB OTA application slots.

`MAX_CARDS` is 64. The current compiled library is below this limit.

When the library reaches the limit, select one solution:

1. Increase `MAX_CARDS` after a memory and flash review.
2. Reduce thumbnail dimensions.
3. Move more thumbnails to runtime storage.
4. Add a paged library model.

Do not change the partition table without an upgrade and recovery test.

## Deferred Work

These items are outside the current release:

- Local Spotify Connect playback on the ESP32-P4.
- A new Waveshare hardware-control panel without the RP2040.
- Support for unrelated audio services without Music Assistant.
- A custom board revision before the prototype electronics are verified.
