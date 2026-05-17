# PORT-NOTES — ESP-IDF port gotchas

Record hardware-verified surprises here as they are discovered during Phase 2.
Each entry should note which migration step it affected and what the fix was.

---

## Template

**Step N — Short description**
- Symptom:
- Root cause:
- Fix:
- Verified:

---

**Step 1 — `esp_lcd_panel_dev_config_t` field renamed in IDF v6.0**
- Symptom: Build fails with `'esp_lcd_panel_dev_config_t' has no member named 'rgb_endian'` and `'LCD_RGB_ENDIAN_BGR' undeclared`.
- Root cause: ESP-IDF v6.0 renamed the panel BGR/RGB element-order field. Older v5.x examples and the `esp_lcd_ili9341` README still use the old name.
- Fix: `.rgb_endian = LCD_RGB_ENDIAN_BGR` → `.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR`. Definitions live in `esp_lcd/include/esp_lcd_panel_dev.h` and `esp_lcd/include/esp_lcd_types.h`.
- Verified: Step 1 colour cycle (red/green/blue) renders correctly with BGR element order on CYD ILI9341.

**Step 1 — Component manager only reads `idf_component.yml` from a component's own directory**
- Symptom: Added `espressif/esp_lcd_ili9341` to `cyd/esp-idf/idf_component.yml` (project root). Build failed with `esp_lcd_ili9341.h: No such file or directory`; no `managed_components/` folder was created.
- Root cause: The component manager looks for `idf_component.yml` inside each component directory, not at the project root. The root-level file is silently ignored.
- Fix: Move the manifest to `cyd/esp-idf/main/idf_component.yml`. Also: if a `build/` directory already exists from a configure pass that ran before the manifest was added, delete it — CMake won't re-resolve dependencies on a normal reconfigure once cached. `idf.py fullclean` works too.
- Verified: After moving the manifest and nuking `build/`, the component downloaded into `managed_components/espressif__esp_lcd_ili9341/` and compiled cleanly.

**Step 2 — CYD ILI9341 landscape orientation under `esp_lvgl_port`**
- Symptom: With `swap_xy=true, mirror_x=true, mirror_y=false` (the values that worked for the Step 1 colour cycle), text was rendered horizontally mirrored. Step 1 didn't expose this because solid-colour fills are symmetric.
- Root cause: `esp_lvgl_port`'s `lvgl_port_add_disp` re-applies the rotation from `disp_cfg.rotation`, overriding any earlier manual `esp_lcd_panel_swap_xy` / `esp_lcd_panel_mirror` calls. So orientation must be set via `disp_cfg.rotation` only — do not call swap/mirror manually after `esp_lcd_panel_init`.
- Fix: Drop the manual swap/mirror calls. In `disp_cfg.rotation` use `swap_xy=true, mirror_x=false, mirror_y=false` for the CYD's landscape orientation (USB on the right).
- Verified: "Hello CYD" label renders right-way-up, full-screen, no mirroring.

**Step 3 — XPT2046 touch: polling, calibration, and pipeline order**
- Three independent issues stacked on top of each other; record all three so we don't relearn them.
- (a) **Use polling, not the IRQ line.** With `int_gpio_num = GPIO_TOUCH_IRQ` the `esp_lvgl_port` driver waited on an interrupt that never fired and the chip was never read. Setting `int_gpio_num = GPIO_NUM_NC` makes the driver poll the chip over SPI on every LVGL tick and works reliably.
- (b) **`process_coordinates` runs BEFORE `mirror_x` / `mirror_y` / `swap_xy`.** In `esp_lcd_touch.c` the order is: driver maps raw ADC into `0..x_max` / `0..y_max`, then your callback fires, then mirror_x flips X using `x_max - x`, then mirror_y flips Y using `y_max - y`, then swap_xy swaps them. So `cal_x` is the pre-swap X — it ends up as the post-swap Y (in the 0..LCD_V range), not the post-swap X. Scaling `cal_x` to `LCD_H - 1` instead of `LCD_V - 1` overshoots the mirror's domain and the final X gets clamped to weird floors like 81.
- (c) **Don't try to combine flag-based rotation with `process_coordinates` math** unless you've drawn the pipeline on paper. Cleanest pattern: set `swap_xy = 0, mirror_x = 0, mirror_y = 0` in the touch flags and do the full raw -> final mapping inside the callback. That way the callback owns the orientation entirely and the constants are obvious.
- **CYD-specific raw range** (atanisoft driver, `x_max = LCD_V = 240`, `y_max = LCD_H = 320`):
    - `raw_x` ≈ 31..202 (the atanisoft driver's pre-scaled X axis, derived from the XPT2046's native X ADC)
    - `raw_y` ≈ 25..261 (pre-scaled Y axis)
- **Direction in the 180-deg landscape orientation** (USB on left):
    - `raw_y` HIGH -> screen LEFT;   `raw_y` LOW  -> screen RIGHT
    - `raw_x` HIGH -> screen TOP;    `raw_x` LOW  -> screen BOTTOM
- Mapping: `final_x = (raw_y_max - raw_y) * (LCD_H - 1) / (raw_y_max - raw_y_min)` and analogous for `final_y` using `raw_x`. Clamp both to screen bounds.
- **Disable screen scrolling.** In LVGL 9 `lv_screen_active()` is `LV_OBJ_FLAG_SCROLLABLE` by default. Press-and-drag therefore scrolls the screen, which offsets every subsequent `lv_indev_get_point()` result — a "screen drifts under finger" symptom. Call `lv_obj_remove_flag(lv_screen_active(), LV_OBJ_FLAG_SCROLLABLE)` once at setup. Same trap will hit any new screen we add later.
- **Build-system gotcha that bit hard:** when you edit a `.c` file via tooling while VS Code has the same file open in an editor tab, VS Code's tab still shows the pre-edit version. Ctrl+S (or autosave) then overwrites the on-disk edits with the stale tab content, and the next `idf.py build` compiles the stale source. Either close the tab while iterating, or `File -> Revert File` before saving. A unique `ESP_LOGI` canary line at boot is the cheapest way to confirm a fresh binary is on the chip.
- Verified: Step 3 red square follows the finger across the whole screen with a small dead-zone near the bezel.
