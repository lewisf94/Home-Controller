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

**Step 4 — WiFi STA pushes the binary past the default 1 MB partition**
- Symptom: After adding `esp_wifi_*` + event-group connect logic, the link succeeded but `idf.py build` failed at the partition size check: `app partition is too small for binary music_controller.bin size 0x103950: Part 'factory' ... size 0x100000 (overflow 0x3950)`. Once WiFi + mbedTLS + lwIP are linked in, the default single-app partition table (1 MB factory) overflows by ~14 KB.
- Root cause: ESP-IDF's default partition table is `partitions_singleapp.csv`, which gives the app exactly 1 MB. Fine for the LCD/LVGL-only Step 2/3 builds; not enough for any networked app.
- Fix: Add a custom `partitions.csv` at the project root (alongside `CMakeLists.txt` and `sdkconfig.defaults`) with a 2 MB factory partition (offsets: nvs @ 0x9000 / 0x6000, phy_init @ 0xf000 / 0x1000, factory @ 0x10000 / 0x200000). Enable it via `CONFIG_PARTITION_TABLE_CUSTOM=y` and `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"` in `sdkconfig.defaults`.
- Cache trap: `sdkconfig` already exists from earlier steps and overrides `sdkconfig.defaults`. After editing defaults, delete `sdkconfig`, `dependencies.lock`, and `build/`, then `idf.py reconfigure`. Otherwise the build keeps using the stale 1 MB partition.
- Credential handling: WiFi SSID/password live in `cyd/esp-idf/main/secrets.h` (`#define WIFI_SSID ...` / `#define WIFI_PASS ...`). The file is gitignored; `secrets.h.example` is the committed template.
- Verified: Boot log shows `wifi connected, IP: 192.168.0.48` on the home network; LVGL + touch continue to run alongside WiFi without interference.

**Step 5 — HTTPS to Spotify, with three IDF v6.0 gotchas**
- (a) **The bundled `json` component is gone.** ESP-IDF v5.x shipped `components/json` with cJSON; v6.0 removed it. `idf_component_register(... REQUIRES json)` fails configure with `Failed to resolve component 'json'`. Either add cJSON as a managed dependency, or parse the few fields we actually need by hand. We did the latter -- `spotify.c` has a small `json_find_key` / `json_copy_string` pair that's sufficient for `access_token`, `expires_in`, and `item.name`.
- (b) **Spotify pretty-prints its JSON with whitespace around the colons.** Bodies look like `"name" : "iPhone"`, not `"name":"iPhone"`. A naive `strstr(buf, "\"name\":")` returns NULL. Any custom parser must tolerate `[ \t\n\r]*` either side of the colon. Same trap applies if you grep for the start of a nested object: `strstr(buf, "\"item\":")` misses Spotify's `"item" : {`. Easiest fix: route every key lookup through one `json_find_key()` helper that handles the whitespace.
- (c) **`main` component does NOT implicitly REQUIRE every other component in v6.0.** The old folklore that "main pulls everything in for free" doesn't hold here. You have to list `esp_http_client`, `esp-tls`, `mbedtls`, `nvs_flash`, `esp_wifi`, `esp_event`, `esp_netif` explicitly in `PRIV_REQUIRES`. Symptom is a header-not-found error like `cJSON.h: No such file or directory` even though the component exists -- linking might still work via transitive deps, but the header search path is governed strictly by REQUIRES/PRIV_REQUIRES.
- **TLS uses the IDF cert bundle.** No need to pin DigiCert manually like the Arduino build did. Just set `.crt_bundle_attach = esp_crt_bundle_attach` on the `esp_http_client_config_t` and enable `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y` (+ the CMN subset is enough; covers Spotify's DigiCert chain). Boot log will print `esp-x509-crt-bundle: Certificate validated` on each successful TLS handshake.
- **Secrets layout.** Move WiFi + Spotify credentials into `cyd/esp-idf/include/secrets.h` (gitignored) so the entire `cyd/esp-idf/main/` directory can be copied between machines without touching credentials. `main/CMakeLists.txt` adds `"../include"` to `INCLUDE_DIRS` so the header is reachable. If both `main/secrets.h` and `include/secrets.h` exist (e.g. from a half-finished migration), the `main/` one shadows the new one and you'll get a confusing "WIFI_PASSWORD undeclared, did you mean WIFI_PASS?" -- delete the stale `main/secrets.h`.
- Verified: Step 5 boot log shows `access token refreshed (expires in 3600 s)` followed by `now playing: <track title>` every 5 s while a song plays on the linked Spotify account.

---

# Waveshare ESP32-P4 port notes

The CYD entries above are ESP-IDF 6.0 on ESP32-WROOM. The Waveshare build is
ESP-IDF 5.5.4 on ESP32-P4 (RISC-V) with PSRAM and a MIPI-DSI panel — different
enough that its gotchas get their own section.

**cp2 — WiFi via `esp_wifi_remote` + `esp_hosted` (onboard ESP32-C6 over SDIO)**
- Symptom A: `fatal error: esp_wifi.h: No such file or directory`, even with
  `esp_wifi_remote` in `PRIV_REQUIRES` and its include paths present.
- Root cause A: `esp_wifi_remote` only *redirects the implementation* of the
  `esp_wifi_*` API to the C6; the **header still comes from the native `esp_wifi`
  component**. The P4 has no radio but IDF 5.5 still ships `esp_wifi` (header +
  stubs) for exactly this. You must list **both** `esp_wifi` and
  `esp_wifi_remote` in `PRIV_REQUIRES`.
- Fix A: add `esp_wifi esp_wifi_remote esp_netif esp_event` to `PRIV_REQUIRES`;
  add `espressif/esp_wifi_remote: "0.14.*"` + `espressif/esp_hosted: "1.4.*"` to
  `main/idf_component.yml`; set `CONFIG_ESP_WIFI_REMOTE_ENABLED=y`.
- Symptom B: after A, the link fails — `--enable-non-contiguous-regions discards
  section ...`, `Total discarded sections size is 2095 bytes`,
  `HINT: binary size has exceeded the limit`.
- Root cause B: this is an **IRAM** (instruction-RAM segment) overflow, *not*
  general SRAM exhaustion. `CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM=y` force-places
  a pile of LVGL hot functions (`lv_color_mix`, `lv_trigo_cos`, …) into IRAM;
  adding the WiFi path's IRAM code tipped the fixed IRAM segment over by ~2 KB.
  (The same setting also produced all the `.iram1` section-conflict warnings.)
- Fix B: `CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM=n`. Frees several KB of IRAM,
  silences the warnings, negligible render-speed cost for this UI. `-Os`
  (`CONFIG_COMPILER_OPTIMIZATION_SIZE=y`) is the bigger hammer held in reserve if
  mbedTLS's IRAM tips it over again at cp3.
- Cache trap (same as CYD Step 4): editing `sdkconfig.defaults` does nothing if a
  generated `sdkconfig` already exists. Delete `sdkconfig` (it regenerates from
  defaults) — do **not** confuse it with the hand-authored `sdkconfig.defaults`.
- Verified: boot log shows ESP-Hosted slave INIT over SDIO (40 MHz, 4-bit), then
  `wifi connected, IP: <addr>` and `checkpoint 2: WiFi OK`.

**cp2 — On-chip memory budget (measured from the cp2 boot log)**

This is the reference for planning every later checkpoint. Two *independent*
problems, different fixes:

| | Link-time fit (A) | Runtime heap (B) |
|---|---|---|
| Symptom | linker `region overflowed` / `discards section` | boots then `heap_caps_malloc failed` / crash |
| Cause | IRAM code + static `.data`/`.bss` | big buffers: TLS, album art, WiFi/lwip |
| Lever | `-Os`, keep code out of IRAM | direct big allocations to PSRAM |

- **Internal SRAM: 768 KB total** (0x4ff00000–0x4ffc0000) — the scarce resource,
  shared by IRAM + static DRAM + internal heap.
- **`heap_init` after cp2 (WiFi up):** ~390 KiB internal heap free
  (256 KiB largest contiguous block + 117 KiB RETENT + 18 KiB) **plus 31 MB of
  PSRAM in the allocator.** `.text` and `.rodata` XIP from PSRAM, so code/consts
  cost ~0 internal SRAM — which is why the link overflow was IRAM-specific.
- **Display framebuffers are NOT the problem:** 3 × 480×800×2 = **2.25 MB lives
  in PSRAM** (MIPI-DSI DPI driver allocates them there). `num_fbs` default 3
  (triple-partial tear-avoid).
- **The one sharp edge for cp3:** the Spotify response buffer is
  `RESP_MAX_CAP = 262144` (256 KB) — *larger than the biggest contiguous internal
  block*. If it lands in internal heap it fails or fragments everything. It and
  the album-art buffer are CYD holdovers allocated PSRAM-agnostically (the CYD had
  no PSRAM). **Must** become explicit PSRAM allocations on the P4.
- **PSRAM-first policy (set once, avoids per-checkpoint whack-a-mole):**
    - `CONFIG_SPIRAM_USE_MALLOC=y`,
      `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096` (allocs >4 KB prefer PSRAM),
      `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`.
    - In ported app code: `spotify.c` response buffer `malloc` →
      `heap_caps_malloc(cap, MALLOC_CAP_SPIRAM)`; `main.c` art buffer
      `MALLOC_CAP_DEFAULT` → `MALLOC_CAP_SPIRAM`.
    - cp3 mbedTLS: `CONFIG_MBEDTLS_DYNAMIC_BUFFER=y`; consider lowering
      `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN` from 16384 if Spotify's TLS allows.
- **Per-checkpoint forecast:** cp3 (Spotify+TLS) is the wall — peaks both A and B
  at once (mbedTLS code + 256 KB buffer + handshake). cp4 (UI) / cp5 (JPEG+art) /
  cp6–7 are comparatively easy: mostly LVGL objects + the art buffer, all
  PSRAM-eligible. Run `idf.py size` / `size-components` after each checkpoint to
  watch the internal-SRAM trend rather than be surprised at cp7.
