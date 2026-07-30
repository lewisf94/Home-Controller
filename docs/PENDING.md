# Pending Verification

This document lists implemented work that still needs a hardware result.

Git history contains the completed change history.

## Waveshare Home Assistant Build

The baseline is the hardware-verified daily build.

The latest BOLD and Developer-control batch is not build-verified.

These release checks remain open:

- [ ] Complete a 60-minute interactive soak.
- [ ] Complete an eight-hour playback and idle soak.
- [ ] Decode music through the built-in speaker.
- [ ] Add an album to the Music Assistant queue.
- [ ] Search for a song and add it to the queue.
- [ ] Clear the Music Assistant queue.
- [ ] Stress light controls during music playback.
- [ ] Stress light controls during inventory refresh.
- [ ] Repair a runtime album that has no cover.
- [ ] Install a firmware update through OTA.
- [ ] Confirm that credentials remain after OTA.
- [ ] Confirm that a test crash appears at the next boot.
- [ ] Confirm that the crash report is then cleared.
- [ ] Build the latest BOLD and Developer-control batch.
- [ ] Test BOLD dark.
- [ ] Test BOLD light.
- [ ] Test all six Developer pages.
- [ ] Export values through the serial monitor.
- [ ] Reset one mode to its compiled defaults.
- [ ] Compare the browser bench with the hardware display.

During the long soak, record these values:

- Lowest internal-memory total.
- Lowest DMA-capable memory total.
- Largest DMA-capable block.
- Reset reason.
- WebSocket reconnect count.
- SDIO allocation failures.

The HA build reserves 64 KB of internal memory for mandatory DMA allocations.

Do not reduce this reserve before the soak passes.

## Waveshare Direct Build

The direct baseline is hardware-verified.

These focused checks remain useful:

- [ ] Test bad-token recovery.
- [ ] Test a restricted Spotify output.
- [ ] Test an unreachable pinned Sonos speaker.
- [ ] Confirm Spotify command-client reuse during repeated button presses.
- [ ] Run Cover Flow while the FPS display is active.

## RP2040 Haptic Knob

Checkpoint 1 is hardware-verified:

- Pico USB boot.
- Native Pico SDK firmware.
- USB serial output.

Checkpoint 2 is hardware-verified:

- MT6701 on GP4 and GP5.
- 1 kHz sensor reads.
- Manual angle tracking.
- No read errors with the magnet correctly positioned.

Checkpoint 3 is build-verified:

- TMC6300 bridge starts disabled.
- The arm command expires after 10 seconds.
- The motor test uses 12 percent duty.
- The motor test stops after 2.4 seconds.
- DIAG causes an immediate stop.

Complete these hardware checks:

- [ ] Measure the power rails before motor connection.
- [ ] Connect the three motor phases.
- [ ] Run the armed motor test.
- [ ] Confirm the expected phase sequence.
- [ ] Confirm that DIAG stops the bridge.
- [ ] Determine the motor pole-pair count.
- [ ] Build and flash the full knob firmware.
- [ ] Test the ESP32-P4 UART link.
- [ ] Calibrate the strain threshold.
- [ ] Test all haptic profiles.

The final PCB must have separate 5.1 kohm pull-down resistors on CC1 and CC2.

The prototype Pico requires a USB-A to USB-C cable.

## CYD Direct ESP-IDF Build

This build was hardware-verified before the recent shared-component changes.

Complete a new hardware pass:

- [ ] Reconfigure and build.
- [ ] Flash the CYD.
- [ ] Verify display colors and orientation.
- [ ] Verify touch calibration.
- [ ] Verify Spotify authentication.
- [ ] Verify album-art changes.
- [ ] Verify MCP23017 recovery after a late connection.
- [ ] Verify Wi-Fi recovery after router loss.
- [ ] Verify the initial volume value.
- [ ] Verify all physical controls.

## CYD Home Assistant Build

This build is not hardware-tested.

Complete these checks:

- [ ] Reconfigure and build.
- [ ] Flash the CYD.
- [ ] Authenticate with Home Assistant.
- [ ] Receive the configured player state.
- [ ] Verify Music Assistant album playback.
- [ ] Verify album art.
- [ ] Verify all physical controls.
- [ ] Verify WebSocket recovery after a Home Assistant restart.

## CYD Arduino Build

This build is in maintenance mode.

Complete these checks before a release:

- [ ] Build with PlatformIO.
- [ ] Verify display color order.
- [ ] Verify touch orientation.
- [ ] Verify Spotify TLS.
- [ ] Verify input responsiveness during HTTPS requests.
- [ ] Verify the embedded-thumbnail fallback.

## Deferred Architecture

These items are not release blockers:

- Share more CYD application lifecycle code.
- Add CYD automatic dimming.
- Replace Home Assistant light refresh delays with state subscriptions.
- Add remove and reorder operations for runtime albums.
- Evaluate local ESP32-P4 audio playback.

## Close A Pending Item

1. Record the firmware commit.
2. Record the hardware configuration.
3. Record the test duration.
4. Record the serial result.
5. Update [TESTING.md](TESTING.md).
6. Remove the completed item from this file.
