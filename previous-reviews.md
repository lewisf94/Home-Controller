# Engineering Review Register

This register prevents repeated reviews of the same area.

Git history contains the complete original review reports.

## Review History

| Date | Area | Principal result |
|---|---|---|
| 2026-05-22 | Security | Found disabled TLS verification and unsafe credential instructions. |
| 2026-05-23 | Security | Confirmed credential handling and found an unchecked request length. |
| 2026-05-24 | Performance | Found repeated TLS handshakes and unnecessary response processing. |
| 2026-05-26 | Code quality | Found duplicated components, dead code, and player-state drift. |
| 2026-05-28 | Reliability | Found permanent Wi-Fi failure and weak recovery paths. |
| 2026-05-29 | Architecture | Found duplicated application lifecycle code and an informal backend contract. |
| 2026-05-30 | Missing functions | Found broken CYD configuration and several interface gaps. |
| 2026-05-31 | Performance | Found command latency, Sonos connection cost, and render-path work. |
| 2026-06-01 | Implementation | Fixed command reuse and unnecessary CYD input locking. |
| 2026-06-01 | Verification | Confirmed prior fixes and added Sonos client reuse. |

## Resolved Security Findings

- TLS certificate verification is active.
- The firmware does not use `setInsecure()`.
- Credential files are not tracked.
- Token response bodies are not written to logs.
- The token helper cache is not tracked.
- Token request lengths are checked.
- No real credential file has been committed.

Flash encryption and NVS encryption remain optional product decisions.

## Resolved Reliability Findings

- Wi-Fi uses background reconnect.
- A dormant Spotify output can wake on play.
- Playback command failures produce diagnostics.
- A missing MCP23017 is probed again.
- The interface reports offline state.
- Empty album data produces a message.
- Album truncation produces a warning.
- JPEG input requires the expected start marker.
- LittleFS information errors produce warnings.
- Queue allocation failure stops startup cleanly.
- Home Assistant handles clean WebSocket closure.
- Home Assistant retries after rejected startup authentication.
- The HA build reserves internal memory for SDIO DMA.

## Resolved Performance Findings

- Spotify player-state requests reuse an HTTPS client.
- Spotify command requests reuse an HTTPS client.
- Sonos status requests reuse an HTTP client.
- Player polling uses an adaptive interval.
- Large Waveshare buffers use PSRAM.
- Cover Flow uses row-major composition.
- Cover Flow skips occluded pixels.
- PPA performs display rotation and blit work.
- CYD input does not take the LVGL lock without pending work.

## Resolved Architecture Findings

- CYD builds share the `cyd_shared` component.
- Waveshare builds share `p4_shared`.
- Waveshare builds share `app_core`.
- The backend-neutral player contract replaces the old Spotify-only name.
- Waveshare interface builders use smaller helper functions.
- The RP2040 knob uses a versioned binary protocol.

## Open Review Topics

Review these areas before you repeat a completed review:

1. RP2040 motor safety after the first hardware run.
2. Long-duration P4 DMA behavior.
3. OTA rollback and interrupted-update recovery.
4. Runtime album catalogue corruption recovery.
5. Home Assistant light-state subscriptions.
6. CYD build reproducibility after dependency updates.
7. Custom-board power and USB-C safety.

## Review Procedure

1. Read this register.
2. Select the oldest open area.
3. Define the review boundary.
4. Inspect the current source.
5. Verify each suspected defect.
6. Mark false findings clearly.
7. Fix approved defects.
8. Run the applicable tests.
9. Add one concise entry to this register.

Do not report an unverified suspicion as a confirmed defect.

Do not repeat a closed review without a new reason.
