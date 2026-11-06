# Lights Knob Interaction

## Status

This document is the specification for the knob control of lights.

The interaction scheme is a confirmed design decision (Lewis, November 2026).
Do not replace it with a different scheme.

| Part | Status |
|---|---|
| Touchscreen light controls (Home Assistant build) | Work on hardware. This change keeps them. |
| Knob interaction firmware (P4 and RP2040) | Written and host-tested. Not verified on hardware. |
| Haptic profiles | Initial values only. They need physical tuning. |
| Global resistance to prevent accidental knob movement | A design intention. Not implemented. |

The knob stays off in both Waveshare builds unless the build sets
`KNOB_ENABLED=1`. This change does not enable the motor hardware.

## Controls

The Lights button (SW4) opens the Lights screen. The knob then starts in
light selection mode.

### Light selection mode

| Input | Action |
|---|---|
| Rotate the knob | Move the selection to the next or previous light. |
| Short press | Toggle the selected light on or off. |
| Long press (500 ms) | Enter adjustment mode for the selected light. |

### Light adjustment mode

| Input | Action |
|---|---|
| Rotate the knob | Adjust the active parameter. |
| Short press | Switch between brightness and hue. |
| Long press (500 ms) | Return to light selection. |

Adjustment mode starts with brightness as the active parameter.

The selected light stays the same when the knob enters or leaves adjustment
mode.

### Press timing

- A short press occurs on release.
- A long press occurs once, 500 ms after the press starts.
- The release after a long press does not cause a short press.
- A press that is down when the Lights screen opens has no effect. The next
  press after its release operates normally.
- A release that arrives after 500 ms without an earlier tick counts as a long
  press, not a short press.

## Proposed Haptic Behaviour

These profiles are initial values. Each value needs physical tuning on the
real knob.

| Mode | Feel | Initial KnobConfig values |
|---|---|---|
| Light selection | Distinct medium-strength detents, one detent for each light. End stops at the first and the last light. | 15 degrees for each detent, detent strength 0.6, end-stop strength 1.0 |
| Brightness | Small, precise detents. Motor end stops at 0% and 100%. | 3 degrees for each 1% step, detent strength 0.3, end-stop strength 1.0 |
| Hue | Continuous rotation through the colour range. No hard end stops. | No detents, no end stops, 5 degrees of hue for each 3 degrees of rotation |

The proposed global resistance against accidental knob movement remains a
design intention. The firmware does not add a constant torque. A constant
torque needs a check of the motor temperature, the current limit and the
battery consumption before implementation.

## State Transitions

```text
              Lights button
                   |
                   v
   +---------------------------------+
   |       LIGHT SELECTION           |
   |  rotate: change selected light  |<-------------------+
   |  short press: toggle light      |                    |
   +---------------------------------+                    |
                   | long press                           | long press
                   v                                      |
   +---------------------------------+  short press  +---------------------+
   |   ADJUST: BRIGHTNESS (default)  |<------------->|    ADJUST: HUE      |
   |  rotate: brightness 0..100%     |               |  rotate: hue wraps  |
   +---------------------------------+               +---------------------+
                   | long press
                   v
            LIGHT SELECTION
```

Another menu button (SW1, SW2 or SW3) leaves the Lights menu. The firmware
sends the last pending value first.

When touch navigation leaves the Lights screen, the knob returns to the menu
for the screen that shows. The firmware also does this when the Lights screen
does not open within 2 seconds.

## Unsupported Controls

| Light capability | Behaviour |
|---|---|
| On and off only | A short press toggles the light. A long press does not enter adjustment mode. The screen shows a message. |
| Brightness only | Adjustment mode has brightness only. A short press in adjustment mode does not change the parameter. |
| Colour only | Adjustment mode starts on hue, because the light has no brightness control. |
| Unavailable in Home Assistant | The knob does not toggle or adjust the light. The screen shows a message. |
| Capability lost during adjustment | The knob changes to the other parameter, or returns to light selection. |

Colour temperature is not part of the knob scheme. The touchscreen keeps its
colour-temperature control.

## Commands And Network Rules

- The knob uses the same Home Assistant functions as the touch controls:
  `ui_request_light_toggle()`, `ui_request_light_brightness()` and
  `ui_request_light_hue()`.
- These functions put a command on the backend queue and do not block. The
  input path and the screen path never make a network request.
- The firmware sends a brightness or hue value at a maximum of one command
  each 220 ms. This is the same limit as the touch sliders.
- When the mode changes or the menu changes, the firmware sends the last
  pending value. A mode change does not send a toggle.
- The firmware does not send the same value two times in sequence.
- Brightness 0% goes to Home Assistant as `brightness_pct` 0. Home Assistant
  then turns the light off.

## Connection Loss

- When Home Assistant is not ready, the knob does not send commands. The
  screen shows "KNOB: OFFLINE".
- The mode and the selected light stay the same during the connection loss.
- The firmware deletes a pending value when the connection stops. It does not
  send an old value after reconnection.
- After reconnection, the firmware requests a new light list. The selection
  follows the same `entity_id`.
- When the knob stops sending state for 250 ms during a press, the firmware
  cancels that press. No long press occurs from old data.

## Light List Refresh

- The selection follows the `entity_id` of the selected light. A different row
  order does not change the selected light.
- When the selected light is removed, the selection stays at the same row
  position.
- During adjustment, a refresh does not replace the value of the active
  parameter. This prevents a jump back to an old value.

## Visual Feedback

- The selected row has an outline in the accent colour.
- The label at the top left shows the knob mode: "KNOB: SELECT",
  "KNOB: BRIGHTNESS 45%", "KNOB: HUE 120" or "KNOB: OFFLINE".
- A knob toggle repaints the power button at once. The next Home Assistant
  refresh confirms the state.
- A knob brightness or hue change moves the matching row slider and preset
  marker.

## Knob-Press Duration Protocol

The P4 measures the press duration. The RP2040 reports the press level in
`KnobState.button_mask`:

| Bit | Meaning |
|---|---|
| 0 to 3 | MX buttons SW1 to SW4 |
| 4 | The knob press is down now. |
| 5 | Bit 4 is valid. |

The wire format does not change, so no `.pb` file needs regeneration.

| P4 firmware | RP2040 firmware | Result |
|---|---|---|
| New | New | Full scheme: short press and long press. |
| New | Old | Bit 5 is clear. Each `press_nonce` change is a short press. Long press is not available. |
| Old | New | The old P4 ignores bits 4 and 5. The old behaviour stays. |

The RP2040 strain-gauge detection has hysteresis. A press starts above
`HX711_PRESS_THRESHOLD` (5000) and ends below `HX711_RELEASE_THRESHOLD`
(3000). Both values are placeholders that need calibration.

At the HX711 default rate of 10 samples each second, the press timing has
approximately 100 ms of jitter. Tie the HX711 RATE pin HIGH for 80 samples
each second (see `KNOB-NOTES.md`).

## Source Files

| File | Function |
|---|---|
| `waveshare/components/p4_shared/lights_knob.c` | Pure C state machine. It has no ESP-IDF or LVGL dependency. |
| `waveshare/components/p4_shared/knob_input.c` | Connects the state machine to the knob, the UI and the light commands. |
| `waveshare/components/p4_shared/lights_knob_ui.h` | Private seam between `knob_input.c` and `ui.c`. |
| `waveshare/components/p4_shared/ui.c` | Selection outline, mode label and the copy of the knob values into the rows. |
| `waveshare/esp-idf-ha/main/main.c` | `ui_backend_lights_ready()` returns the Home Assistant session state. |
| `waveshare/esp-idf/main/main.c` | `ui_backend_lights_ready()` returns false. The direct build has no lights backend. |
| `rp2040/src/interface_task.cpp` | Press level bits and strain-gauge hysteresis. |
| `tests/native/test_lights_knob.c` | Host tests for the state machine. |

## Verification

### Automated (host tests)

Run `python -m unittest tests.test_native_c -v`.

The tests cover:

- Short press on release, and long press once at 500 ms.
- No short press after a long press.
- Mode changes between selection, brightness and hue.
- Brightness end stops at 0% and 100%, and the 220 ms rate limit.
- Hue wrap at 0 and 360 degrees.
- Selection changes, and selection by `entity_id` after a refresh.
- Lights with no dimming or no colour support, and unavailable lights.
- Connection loss and reconnection.
- No unintended commands when the mode or the menu changes.

The host tests do not cover the motor, the strain gauge, the UART timing, the
LVGL rendering or Home Assistant.

### Hardware checks (not done)

Do these checks when the knob daughterboard is fitted:

1. Build the Home Assistant firmware with `-DKNOB_ENABLED=1`.
2. Press SW4. Make sure that the Lights screen opens and no light changes.
3. Turn the knob. Make sure that the outline moves one row for each detent.
4. Push and release the knob quickly. Make sure that the selected light toggles.
5. Hold the knob for 500 ms. Make sure that "KNOB: BRIGHTNESS" shows.
6. Release the knob. Make sure that the light does not toggle.
7. Turn the knob to each end. Make sure that the end stops occur at 0% and 100%.
8. Push and release the knob. Make sure that "KNOB: HUE" shows.
9. Turn the knob through more than one full hue range. Make sure that no end stop occurs.
10. Hold the knob for 500 ms. Make sure that the same light stays selected.
11. Stop Home Assistant. Make sure that "KNOB: OFFLINE" shows and no command is sent.
12. Start Home Assistant. Make sure that the selection stays on the same light.
13. Tune the haptic values and the press thresholds. Record the values in this document.
