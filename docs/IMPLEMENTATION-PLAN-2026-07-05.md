# Implementation Plan: Lights Fix + Four-Menu Stack + RP2040 Haptics (2026-07-05)

## Status

This document is a historical planning record from 2026-07-05. The project
completed Parts 0, 1, 3, 4, and 5 as described. Part 2 described a separate Volume screen
in the swipe stack. `docs/QUEUE-DESIGN.md` later replaced that Volume screen
with volume control on the Now Playing page, plus a new Queue page. Read
`docs/QUEUE-DESIGN.md` for the current navigation design.

## Overview

Three coordinated work streams to complete the home-controller project:

1. **Part 0 — Lights toggle bug fix** (FIRST, diagnose then apply candidate fixes)
2. **Part 1 — Lights full control** (colour, temperature, live state via HA subscription)
3. **Part 2 — Four-screen navigation stack** (Albums / Now Playing / Volume / Lights with swipe)
4. **Part 3 — RP2040 haptic firmware + protocol v2** (interaction state machine, LED patterns, mode physics)
5. **Part 4 — Hardware feasibility audit** (verify blog plan is achievable on this hardware)
6. **Part 5 — Improvement suggestions** (optional future work)

**Key decisions:**
- Swipe stack order: Albums / NP / Volume / Lights (preserves verified swipe-up Albums→NP).
- Light colour UI: sliders (hue/sat/colour-temp/brightness).
- Scope: everything now, including RP2040 firmware (uncompiled; accept same risk as angle-unwrap fix).
- Menu identity: keep 4-theme system; each menu visually distinct within theme.

**Build status:**
- Both waveshare targets compile cleanly after lights implementation.
- RP2040 code written but cannot be compiled in this environment (no PlatformIO).
- All P4-side changes (proto, knob_input, ui, main) are build-verified.

---

## Part 0 — Diagnose and fix lights toggle bug

### Hardware check (Lewis does this first)

1. In HA web UI: Developer Tools → Actions → run `light.toggle` on KAJPLATS entity.
2. Also toggle from device card directly.
3. Test DEVICES screen transfer (identical transport chain to lights).

### Diagnostic logging (added to codebase, permanent)

**File: `waveshare/esp-idf-ha/main/ha_client.c`**

1. In `call_service_entity()` (~line 193), before `return ws_send(buf);`:
   - Log: `ESP_LOGI(TAG, "call_service %s.%s -> %s%s", domain, service, entity_id, (service_data ? " (+data)" : ""));`

2. In `handle_message()` "result" branch (~line 490), before the id-match chain:
   - Parse success and log failures: `const char *succ = json_obj_get(msg, "success");` then check if the value is "true".
   - If failed, extract and log the error message from the result.

**File: `waveshare/components/p4_shared/ui.c`**

3. In `on_light_toggle()` (~line 3805):
   - Log: `ESP_LOGI(TAG, "light toggle tap -> %s", s_light_entries[i].entity_id);`

### Decision table (after flashing and testing)

| What serial shows | Root cause | Fix |
|---|---|---|
| No `light toggle tap` line (no click sound) | LVGL tap missed | Add `lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE)` in `ui_set_lights()` |
| Tap line, no `call_service` line | Queue/dispatch broken | Check `HCMD_LIGHT_TOGGLE` dispatch in `ha_task`; log xQueueSend result |
| `call_service` line + `result id=N FAILED: <msg>` | HA rejected call | Fix based on error message (entity renamed, auth, service) |
| `call_service` line, no FAILED, bulb unchanged | HA accepted, bulb didn't respond | HA/Thread side issue (not firmware) |
| `ws_send: not connected` | WS down at tap time | Check reconnect handling |

### Always apply (regardless of diagnosis)

1. **UI feedback**: After toggle/brightness commands in `ha_task()`:
   ```c
   vTaskDelay(pdMS_TO_TICKS(400));  /* Matter round-trip */
   ha_request_lights();
   ```
   (Part 1 replaces this with a live subscription. Remove the delay when Part 1 is in place.)

2. **Commit**: `fix(waveshare-ha): add lights toggle diagnostics and command feedback`

---

## Part 1 — Lights: full control (colour, temperature, live state)

### `ui_light_t` struct v2 (`include/ui.h`)

```c
#define LIGHT_CAP_DIM    (1 << 0)   /* brightness */
#define LIGHT_CAP_CT     (1 << 1)   /* colour temperature */
#define LIGHT_CAP_COLOR  (1 << 2)   /* hue/sat colour */

typedef struct {
    char name[40];
    char entity_id[96];
    bool is_on;
    uint8_t caps;            /* LIGHT_CAP_* from supported_color_modes */
    int  brightness_pct;     /* 0-100, -1 unknown */
    int  hue;                /* 0-359, -1 unknown */
    int  sat;                /* 0-100, -1 unknown */
    int  color_temp_k;       /* Kelvin, -1 unknown */
    int  min_ct_k, max_ct_k; /* slider range; fallback 2000/6500 */
} ui_light_t;
```

New seam functions (queue-posters, real in HA, no-op in non-HA):
- `void ui_request_light_color(const char *entity_id, int hue, int sat);`
- `void ui_request_light_ct(const char *entity_id, int kelvin);`

### `ha_client.c` — parsing, services, live subscription

**JSON helpers:**
- `json_arr_contains(const char *arr, const char *needle)` — check if array contains string.
- `json_arr_get_two(const char *arr, double *a, double *b)` — read first two array numbers.

**`build_light_list()` enhancements:**
- Parse `supported_color_modes` array: "brightness"→DIM, "color_temp"→CT, "hs"/"xy"/"rgb*"→COLOR.
- Fix: dimmable = `caps & LIGHT_CAP_DIM` (not presence of brightness attr; off lights omit it).
- Parse `hs_color [h, s]`, `color_temp_kelvin`, `min_color_temp_kelvin`, `max_color_temp_kelvin`.
- Factor into `parse_light_state(const char *obj, ui_light_t *out)` for reuse in live events.

**New services:**
- `bool ha_light_set_color(const char *entity_id, int hue, int sat);` — service_data: `"hs_color":[H,S]`
- `bool ha_light_set_ct(const char *entity_id, int kelvin);` — service_data: `"color_temp_kelvin":K`

**Live state push:**
- New statics: `s_lights_sub_id`, `s_light_ids[MAX_LIGHTS][96]`, `s_light_count`.
- After `build_light_list()` fetches n>0: unsubscribe old id, then subscribe to state change trigger with entity array.
- In `handle_message()` "event" branch: if entity starts with `"light."`, parse state and call `ui_update_light()`.
- On WS disconnect, zero `s_lights_sub_id` (re-subscribe on next fetch).

### `ui.c` — detail view + in-place updates

**Row widget caches:**
```c
static lv_obj_t *s_light_rows[MAX_LIGHTS];
static lv_obj_t *s_light_row_toggles[MAX_LIGHTS];
static lv_obj_t *s_light_row_names[MAX_LIGHTS];
```

**Row layout:** name + power toggle + (if caps != 0) chevron. Tap row body → detail view. Tap toggle → toggle only.

**Detail view:** hidden container on `s_screen_lights`, shown by `show_light_detail(int idx)`.
Contents top→bottom:
1. Light name + big ON/OFF toggle.
2. BRIGHTNESS header + slider 1..100 (shown when `caps & DIM`).
3. COLOUR header + HUE (0..359) + SAT (0..100) sliders (shown when `caps & COLOR`).
   - Live recolour the INDICATOR and KNOB of the hue slider as `lv_color_hsv_to_rgb(hue, 100, 100)`.
   - Show 40×40 preview swatch: `lv_color_hsv_to_rgb(hue, sat, 100)`.
4. WARMTH header + colour-temp slider `min_ct_k..max_ct_k` (shown when `caps & CT`).
   - Recolour knob warm→cool (orange→white→pale blue).

All sliders set `s_light_dragging` on PRESSED, clear on RELEASED/PRESS_LOST.

**New function:**
```c
void ui_update_light(const ui_light_t *l);  /* in-place row + slider updates */
```
Takes display lock, finds row by entity_id, updates: name colour, toggle colours, slider values (if detail view open and not dragging).

### `main.c` wiring

**HA build:** Two new HCMDs:
- `HCMD_LIGHT_COLOR`: `{ entity_id[96], hue, sat }` → dispatch to `ha_light_set_color()`
- `HCMD_LIGHT_CT`: `{ entity_id[96], kelvin }` → dispatch to `ha_light_set_ct()`

**Non-HA build:** Two more no-op seam functions (same pattern as existing three).

---

## Part 2 — Four-screen swipe stack: Albums / NP / Volume / Lights

### Stack navigation model

Order: 0=Albums, 1=Now Playing, 2=Volume, 3=Lights.

Swipe semantics: UP (`LV_DIR_TOP`) = down stack (+1); DOWN (`LV_DIR_BOTTOM`) = up stack (-1).
- Albums + swipe-up → NP (verified, unchanged).
- Swipe outside ends → Settings (swipe down on Albums, swipe up on Lights).

**Rewrite `on_gesture()`:**
- Keep `s_seeking || s_vol_dragging` guard, add `|| s_light_dragging`.
- Compute `idx = stack_index_of(active)`.
- LV_DIR_TOP: if idx==3 open settings, else `load_screen(stack_screen(idx+1), true)`.
- LV_DIR_BOTTOM: if idx==0 open settings, else `load_screen(stack_screen(idx-1), false)`.
- Keep NP LEFT/RIGHT next/prev unchanged.
- Attach `on_gesture` to VOLUME and LIGHTS builders (browser + NP already have it).

**`load_screen()` refactor:**
- Rename bool `to_np` → `slide_up` (true = enter from bottom).
- Extract screen-open side effects into helpers:
  - `lights_screen_prepare()`: placeholder + `ui_request_get_lights()`.
  - `volume_screen_prepare()`: sync dot grid / slider from `s_np_volume`.
- Call these from `load_screen()` when target matches.

### Volume screen for ALL themes

Today GLYPH-only. Make unconditional:
- Remove `if (is_glyph_theme())` gates in `ui_init` and `apply_theme_cb`.
- GLYPH keeps existing dot grid exactly.
- Other themes: title "VOLUME" + large % label (centred, font_lg) + vertical slider (56×280) + MUTE button.
- Slider events: `LV_EVENT_VALUE_CHANGED` updates label locally; `LV_EVENT_RELEASED` → `ui_request_volume()`.
- Set/clear `s_vol_dragging` on PRESSED/RELEASED|PRESS_LOST (guard gestures).

### Per-menu identity + indicator

Add `add_stack_indicator(lv_obj_t *screen, int idx)` helper:
- 4 dots (10 px, 16 px apart, `LV_ALIGN_RIGHT_MID`, x=-10).
- Current index filled `accent_color()`, others `s_th->track`.
- Each dot clickable (`lv_obj_set_ext_click_area(dot, 8)`) → `load_screen(stack_screen(i), i > idx)`.

Title chips already identify Volume/Lights; Albums and NP are visually distinct (carousel vs art).

### Knob/menu sync seam (`ui.h`)

```c
typedef enum { UI_MENU_ALBUMS = 0, UI_MENU_NOW_PLAYING, UI_MENU_VOLUME,
               UI_MENU_LIGHTS, UI_MENU_OTHER } ui_menu_t;
ui_menu_t ui_get_active_menu(void);
void      ui_goto_menu(ui_menu_t m);
void      ui_return_previous_menu(void);
```

`ui_goto_menu` records previous menu (single-level history for overlay). `ui_get_active_menu` returns OTHER for settings/devices.

In `knob_input.c` `_poll_timer_cb`: replace `ui_is_now_playing()` two-state with `ui_get_active_menu()` and map to `_activate_menu()`.

---

## Part 3 — Protocol v2 + RP2040 haptic engine + P4 input state machine

### Proto changes (`proto/home_controller.proto`)

**Append-only, never renumber fields 1-11 of KnobConfig or KnobState.**

Add to KnobConfig:
```proto
    uint32 haptic_mode            = 12; // 0=DETENT, 1=VISCOUS, 2=RATCHET
    float  viscous_damping_unit   = 13;
    float  breakout_strength_unit = 14;
    uint32 idle_timeout_ms        = 15;
    uint32 led_pattern            = 16; // 0=RAW 1=OFF 2=MENU_GLOW 3=PROGRESS 4=PULSE
    uint32 led_hue                = 17; // 0-359
    uint32 led_brightness         = 18; // 0-255
    float  led_param              = 19; // PROGRESS fraction
    uint32 active_button          = 20; // 0-3 bright, others dim, 255 none
```

Add to KnobState:
```proto
    bool   knob_pressed           = 7;  // live strain state
```

**Regenerate:**
```bash
pip install nanopb==0.4.9.1
python -m nanopb.generator.nanopb_generator proto/home_controller.proto
```
Copy `home_controller.pb.c/.h` to: `proto/`, `rp2040/src/proto_gen/`, `waveshare/components/p4_shared/include/`.

Verify: the RP2040 `static_assert(KnobState_size + 4 <= 68)` still holds; `ToKnob_size` < 256 (if over, raise `COBS_BUF_SIZE` to 320 in both `interface_task.cpp` and `knob.c`).

### RP2040 `motor_task.cpp` — haptic modes + idle lock

Add interaction state machine (TS_IDLE → TS_LOCKED → TS_FREE → TS_IDLE):
- TS_IDLE: torque 0, position frozen.
- TS_LOCKED: strong spring to anchor (anti-nudge breakout).
- TS_FREE: normal mode physics.
- Movement detect: velocity > 0.5 rad/s OR |unwrapped - anchor| > 0.02 rad.
- Breakout threshold: 0.6 × position_width_radians.

Mode physics in TS_FREE:
- DETENT (0): existing code unchanged.
- VISCOUS (1): `torque = -viscous_damping_unit * getVelocity() * MOTOR_DETENT_SCALE`.
- RATCHET (2): DETENT with snap_point forced to 0.5 (no hysteresis).

Publish frozen detent position while not TS_FREE (new static).

Add 3-sample moving average on velocity for noise immunity.

### RP2040 `interface_task.cpp` — strain hysteresis, debounce, LED patterns

1. `KnobState.knob_pressed = s_was_pressed;` in `_send_state()`.
2. Strain hysteresis: press on `raw > THRESHOLD`, release only on `raw < (THRESHOLD * 7/10)`.
3. MX debounce: per-button 3-sample agreement shift-register (~15 ms window).
4. LED pattern engine (`_led_tick()` every 33 ms):
   - OFF: clear both chains.
   - MENU_GLOW: ring solid at hue/brightness; buttons: active_button full, others 10%.
   - PROGRESS: ring LEDs 0..`(int)(param * 12)` lit, rest off.
   - PULSE: triangle-wave brightness over 1 s.
   Call `show()` only on frame change (memcmp state struct).

### P4 `knob_input.c` v2 — press state machine + menu mapping

**Press state machine (5 inputs: SW1-4, knob press):**
```c
typedef struct { bool down; TickType_t t_down; bool long_fired; } press_sm_t;
static press_sm_t s_press[5];
#define LONG_PRESS_MS 500
```

Fed from `_on_state()`: down-edge record `t_down` → `EV_KEYDOWN`; while down and `now - t_down >= LONG_PRESS_MS && !long_fired` → `EV_LONG_START`; up-edge → `EV_SHORT` or `EV_LONG_END`.

Central dispatcher `_input_event(int input, ev_t ev)`.

**Full interaction mapping (blog model, adapted):**

| Input | Context | EV_KEYDOWN | EV_SHORT | EV_LONG_START | EV_LONG_END |
|---|---|---|---|---|---|
| SW1/knob | NP | — | play/pause | TRACK-NAV entry | exit TRACK-NAV |
| SW2/knob | Volume | — | play/pause | PRECISION toggle | — |
| SW3/knob | Albums | — | play centred | (tracklist: N/A) | — |
| SW4/knob | Lights | — | submode adv. | back to SELECT | — |
| All buttons | other menu | goto that menu | — | same | — |

**Haptic configs per menu:**
- Albums: detents at album count.
- NP scrub: heavier breakout 1.5; `ui_show_scrub_arm(float)` shows force-build bar.
- TRACK-NAV (SW1 long in NP): RATCHET 20°, strength 1.0, unbounded, 0 breakout; each click → next/prev; re-anchor on exit.
- Volume: 3.6°/1%; PRECISION mode min 0/max 200 for 0.5% steps.
- Lights: submodes L_SELECT → L_BRI → L_HUE → L_BRI, EV_LONG_START anywhere → L_SELECT.
  Needs ui.h getters: `ui_lights_count()`, `ui_lights_get(int idx, ...)`.

All inside `#if KNOB_ENABLED`.

### P4 `knob.c`

No structural change. After proto regen, add compile-time guard: `ToKnob_size + 4` fits send buffer.

---

## Part 4 — Hardware feasibility audit (docs/KNOB-NOTES.md)

**Verdict:** Planned system is feasible on this hardware.

**Division of labour:** RP2040 = FOC 5 kHz + detent + strain/buttons/LEDs/lux/battery (core 1 motor, core 0 I/O); P4 = UI/LVGL/WiFi/HA (800×480 touch).

**Link budget:** KnobState ~45 B framed, 200 Hz → ~72 kbit/s (~8% of 921600 baud UART).

**Timing:** Haptics never round-trip (<1 ms local); UI reaction = 5 ms frame + LVGL tick; HA commands = 100-400 ms (normal for Matter).

**Corrections to blog:**
1. MAX17048 address is 0x36, not 0x32 (code already correct).
2. HX711 RATE pin must be HIGH (80 SPS) on PCB; default 10 SPS misses fast taps.
3. MT6701 is absolute-per-revolution (14-bit), not continuous; unwrap accumulator gives multi-rev continuity.
4. Volume steps: 1% standard, 0.5% precision (blog inconsistent).
5. Free RP2040 GPIOs: 7, 19, 22, 28, 29 (breakout choice affects 23/24/25).
6. Power: TMC6300 runs on 3.7V LiPo directly (SmartKnob-proven); 5V LED rail + shifter already chosen.
7. Gap: album tracklist has NO data source (needs Music Assistant WS API or HA REST).
8. Border router: ESP32-H2 or C6 works; note which is plugged in.
9. Strain mount edge-press unvalidated until PCB exists; thresholds are `#define`s for calibration.

Also update `docs/P4-TODO.md`, `CLAUDE.md`, `docs/PENDING.md` with new items + hardware-verify debt.

---

## Part 5 — Optional future improvements

1. Battery % + charge state on-screen (field already streams).
2. Idle clock/screensaver on auto-dim hooks.
3. HA light groups (already appear as `light.*`, badge would help).
4. OTA updates (esp_https_ota) once case is sealed.
5. Music Assistant WS API for tracklists.
6. Physical: prototype knob mass before machining (gimbal-motor detent crispness drops with heavy knobs).

---

## Implementation order + verification

**Order:**
1. **Part 0:** Diagnose + apply fixes alone, commit, verify on hardware.
2. **Part 1:** Implement lights v2, build both targets, commit.
3. **Part 2:** Four-screen stack, build, commit.
4. **Part 3:** Proto regen, RP2040, P4 code, build P4 targets only (RP2040 uncompiled), commit.
5. **Part 4/5:** Docs, commit.

Before each build: sync files to private folder (hash-verified), build from private with IDF 5.5.4.

**Hardware verification (Lewis does):**
- Part 0: decision table diagnostic output.
- Part 1: colour sliders work on KAJPLATS; live updates from HA app; subscription proof.
- Part 2: 4-screen swipe cycle + indicator dots + settings at ends; volume screen in all 4 themes.
- Part 3: RP2040 first `pio run` compiles; knob press/detents/LEDs work; NP scrub arm visible.

---

## Files modified

| File | Parts |
|---|---|
| `waveshare/esp-idf-ha/main/ha_client.c/.h` | 0, 1 |
| `waveshare/esp-idf-ha/main/main.c` | 0, 1 |
| `waveshare/esp-idf/main/main.c` | 1 (no-ops) |
| `waveshare/components/p4_shared/ui.c` + `include/ui.h` | 0, 1, 2, 3 (seams) |
| `waveshare/components/p4_shared/include/ui_tune.h` | 2 (indicator knobs) |
| `waveshare/components/p4_shared/knob_input.c` | 2.4, 3.4 |
| `waveshare/components/p4_shared/knob.c` | 3.5 |
| `proto/home_controller.proto` + regen `.pb.c/.h` (3 locations) | 3.1 |
| `rp2040/src/motor_task.cpp`, `interface_task.cpp` | 3.2, 3.3 |
| `docs/KNOB-NOTES.md`, `P4-TODO.md`, `CLAUDE.md`, `PENDING.md` | 4 |

---

**Date prepared:** 2026-07-05  
**User:** Lewis  
**Branch:** main
