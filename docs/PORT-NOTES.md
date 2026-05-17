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
