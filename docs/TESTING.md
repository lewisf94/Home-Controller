# Hardware Test Checklist

Run the applicable checklist after a firmware or hardware change.

Stop after the first unexpected result.

Use [PENDING.md](PENDING.md) to identify tests that still need evidence.

## Test Record

Record these values before each test:

| Field | Value |
|---|---|
| Date | |
| Git commit | |
| Build | |
| Board revision | |
| Serial port | |
| Power source | |
| Network | |
| Playback output | |

## Waveshare Common Smoke Test

- [ ] The board completes one boot.
- [ ] The display uses 800 x 480 landscape.
- [ ] The colors are correct.
- [ ] Touch coordinates match the contact point.
- [ ] Wi-Fi connects.
- [ ] The configured backend authenticates.
- [ ] The current track appears.
- [ ] Album art appears.
- [ ] The progress indicator advances.
- [ ] Play and pause work.
- [ ] Next and previous work.
- [ ] Seek works.
- [ ] Volume works.
- [ ] The browser opens.
- [ ] The browser selects the active album.
- [ ] Carousel renders correctly.
- [ ] Focus renders correctly.
- [ ] Cover Flow renders correctly.
- [ ] Cover Flow side covers remain visible.
- [ ] No album card becomes black.
- [ ] Settings open and close.
- [ ] Saved settings survive a reboot.

## Waveshare Theme Test

Test each mode in both appearances.

- [ ] BASIC dark.
- [ ] BASIC light.
- [ ] GLYPH dark.
- [ ] GLYPH light.
- [ ] PIXEL dark.
- [ ] PIXEL light.
- [ ] PAPER dark.
- [ ] PAPER light.
- [ ] BOLD dark.
- [ ] BOLD light.
- [ ] Theme album art enabled.
- [ ] Theme album art disabled.
- [ ] Every compiled font renders.
- [ ] Long titles scroll without overlap.
- [ ] The progress indicator matches the mode.
- [ ] The volume control matches the mode.
- [ ] Interface sounds match the selected set.

Watch the serial output during repeated mode changes.

No allocation failure or reset is permitted.

## Waveshare Developer-Control Test

- [ ] TYPE opens.
- [ ] COLOUR opens.
- [ ] SHAPE opens.
- [ ] LAYOUT opens.
- [ ] BROWSER opens.
- [ ] ART opens.
- [ ] A zero value remains a valid override.
- [ ] Dark and light color overrides remain independent.
- [ ] Each mode keeps its own accent.
- [ ] Slider movement changes only the readout.
- [ ] Slider release applies the new value.
- [ ] EXPORT TO SERIAL produces complete values.
- [ ] RESET MODE restores the compiled values.
- [ ] A reboot preserves an override.
- [ ] The browser bench gives an approximate visual match.

## Waveshare Reliability Test

### Wi-Fi Recovery

1. Power off the router.
2. Power on the controller.
3. Wait 30 seconds.
4. Power on the router.

Expected result:

- The controller connects without a power cycle.
- The interface remains responsive.
- The offline indicator clears.

### Home Assistant Recovery

1. Start normal playback.
2. Stop Home Assistant.
3. Wait 60 seconds.
4. Start Home Assistant.

Expected result:

- The WebSocket reconnects.
- Authentication completes.
- Track state returns.
- Sendspin reconnects when configured.

### Idle Link Recovery

1. Leave the controller idle for five minutes.
2. Disconnect the network.
3. Wait 60 seconds.
4. Restore the network.

Expected result:

- The heartbeat detects the failed link.
- The connection returns without user input.

### Automatic Dimming

1. Leave the controller untouched for 60 seconds.
2. Observe the first dim level.
3. Leave the controller untouched for five minutes.
4. Observe the second dim level.
5. Touch the display.

Expected result:

- The first level is approximately 30 percent.
- The second level is approximately 10 percent.
- Touch restores the configured brightness.

### Memory Soak

Run these actions for 60 minutes:

- Scroll Cover Flow.
- Change themes.
- Change outputs.
- Change volume.
- Add and remove queue items.
- Control lights.
- Add one runtime album.
- Change tracks repeatedly.

Record:

- Lowest internal-memory total.
- Lowest DMA-capable total.
- Largest DMA-capable block.
- Allocation failures.
- Watchdog resets.
- SDIO errors.

Then run eight hours of playback and idle operation.

## Waveshare Direct Backend

- [ ] Spotify refreshes its access token.
- [ ] Player-state requests use the active device.
- [ ] Spotify Connect device selection works.
- [ ] A dormant device wakes on play.
- [ ] A restricted device shows useful feedback.
- [ ] Spotify rate limiting changes the request delay.
- [ ] Sonos appears in the device selector.
- [ ] Sonos album start sets the queue URI.
- [ ] Sonos receives the Play command.
- [ ] Sonos transport controls work.
- [ ] Sonos volume works.
- [ ] Sonos now-playing data appears.
- [ ] An unreachable pinned Sonos does not stall every request.

## Waveshare Home Assistant Backend

- [ ] The WebSocket returns `auth_ok`.
- [ ] The configured output appears.
- [ ] Music Assistant outputs appear under SPEAKERS.
- [ ] Spotify Connect sources appear in their own section.
- [ ] Duplicate output names do not hide the real speaker.
- [ ] Output selection returns to Now Playing.
- [ ] Sendspin completes its handshake.
- [ ] Built-in audio decodes without a reset.
- [ ] Local playback progress advances smoothly.
- [ ] Queue refresh works.
- [ ] Queue album addition works.
- [ ] Queue song search works.
- [ ] Queue clear works.
- [ ] Light power control works.
- [ ] Light brightness control works.
- [ ] Light color control works.
- [ ] Light state refresh shows the accepted value.
- [ ] A rejected token starts a visible retry cycle.

## Runtime Album Test

- [ ] Saved-album browsing returns candidates.
- [ ] Search updates while text changes.
- [ ] Existing albums show `ADDED`.
- [ ] A new album enters the combined catalogue.
- [ ] The new album has a runtime cover.
- [ ] The cover survives a reboot.
- [ ] Unicode metadata renders correctly.
- [ ] A failed cover download does not remove metadata.
- [ ] Cover repair restores a missing cover.
- [ ] The catalogue remains alphabetically ordered.

## OTA And Crash Test

> WARNING: Keep a USB cable available for recovery.

### OTA

1. Host a known compatible application image.
2. Enter the firmware URL.
3. Start the update.
4. Wait for the reboot.

Expected result:

- Download progress appears.
- The inactive slot receives the image.
- The device boots the new image.
- Saved settings remain.

### Crash Report

1. Flash a test build with a controlled crash.
2. Trigger the crash.
3. Wait for the reboot.
4. Capture the boot report.
5. Reboot once more.

Expected result:

- The first boot reports the crashed task and program counter.
- The report is absent after it is cleared.

## RP2040 Sensor Test

- [ ] The Pico enters BOOTSEL.
- [ ] The UF2 file flashes.
- [ ] USB serial appears.
- [ ] The I2C scan finds address `0x06`.
- [ ] The MT6701 reports valid angles.
- [ ] Manual rotation crosses zero degrees continuously.
- [ ] A centered magnet gives stable data.
- [ ] Removing the magnet produces a clear sensor failure.

The bench I2C path uses GP4 for SDA and GP5 for SCL.

## RP2040 Motor Test

> DANGER: Keep the motor clear of loose objects during this test.

> WARNING: Use a current-limited supply.

- [ ] All three motor phase resistances match.
- [ ] Every phase is open-circuit to the motor body.
- [ ] TMC6300 VIO is correct.
- [ ] TMC6300 VIN is correct.
- [ ] Every bridge input is low at boot.
- [ ] The bridge remains disabled before arming.
- [ ] The arm command expires after 10 seconds.
- [ ] The test starts only after a valid arm.
- [ ] The test stops after 2.4 seconds.
- [ ] Duty remains at or below 12 percent.
- [ ] DIAG causes an immediate stop.
- [ ] The motor and driver remain cool.

Use the detailed sequence in
[rp2040/bringup/README.md](../rp2040/bringup/README.md).

## CYD Direct ESP-IDF

- [ ] The build configures.
- [ ] The build links.
- [ ] The display orientation is correct.
- [ ] The display colors are correct.
- [ ] Touch calibration covers the complete screen.
- [ ] Spotify authenticates.
- [ ] Album art changes on a track change.
- [ ] The MCP23017 appears after a late connection.
- [ ] Buttons remain responsive during HTTPS.
- [ ] The encoder remains responsive during HTTPS.
- [ ] Wi-Fi reconnects after router loss.
- [ ] The initial volume matches the active output.

## CYD Home Assistant

- [ ] The build configures.
- [ ] The build links.
- [ ] Home Assistant authenticates.
- [ ] The configured entity state appears.
- [ ] Music Assistant commands work.
- [ ] Album art appears.
- [ ] Physical controls work.
- [ ] The WebSocket returns after a server restart.

## CYD Arduino

- [ ] PlatformIO builds the firmware.
- [ ] The display color order is correct.
- [ ] The display orientation is correct.
- [ ] Touch orientation is correct.
- [ ] Spotify TLS verification succeeds.
- [ ] The current track appears.
- [ ] Embedded thumbnails appear.
- [ ] The MCP input task remains responsive.

## Cross-Build Edge Cases

- [ ] Ten rapid play-pause requests do not reset the device.
- [ ] Volume works while playback is paused.
- [ ] Seek clamps at zero.
- [ ] Seek clamps at track duration.
- [ ] Network loss during a command causes a controlled failure.
- [ ] Token expiry causes a refresh.
- [ ] Empty album data produces a useful message.
- [ ] Album count above the limit produces a warning.

## Failure Report

Record these items:

1. Expected result.
2. Actual result.
3. Exact reproduction steps.
4. Firmware commit.
5. Complete serial output around the failure.
6. Reset reason.
7. Relevant memory values.
8. Photograph or video when the failure is visual.

Do not continue a destructive or thermal test after an unexpected result.
