# ESP-IDF Port Notes

This document records verified porting problems and their solutions.

Use this format for a new entry:

| Field | Content |
|---|---|
| Symptom | Observable failure |
| Cause | Confirmed cause |
| Action | Required correction |
| Status | Build or hardware result |

## CYD ESP-IDF 6

### ILI9341 Element Order

| Field | Content |
|---|---|
| Symptom | The build rejects `rgb_endian` and `LCD_RGB_ENDIAN_BGR`. |
| Cause | ESP-IDF 6 renamed the panel element-order field. |
| Action | Use `rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR`. |
| Status | The hardware color cycle is correct. |

### Managed Component Manifest

| Field | Content |
|---|---|
| Symptom | The build cannot find the ILI9341 component header. |
| Cause | The component manifest is at the project root. |
| Action | Put the manifest in the component directory. |
| Status | The component manager downloads and builds the driver. |

After you move a manifest, remove the stale build cache.

```powershell
idf.py fullclean
idf.py reconfigure
```

### Display Orientation

| Field | Content |
|---|---|
| Symptom | Text appears horizontally mirrored. |
| Cause | The LVGL port applies rotation after manual panel changes. |
| Action | Configure rotation only through the display configuration. |
| Status | Landscape text is correct with the USB connector on the right. |

Use these values:

```text
swap_xy = true
mirror_x = false
mirror_y = false
```

Do not apply additional panel swap or mirror calls.

### XPT2046 Touch

The CYD touch path has these verified requirements:

- Poll the controller.
- Do not depend on the IRQ line.
- Perform the complete orientation mapping in one callback.
- Disable scrolling on every root screen.
- Clamp the final coordinates to the display limits.

The coordinate-processing callback runs before the driver mirror and swap
operations.

Do not combine callback mapping with driver rotation flags.

The measured pre-scaled ranges are:

| Axis | Approximate range |
|---|---|
| Raw X | 31 to 202 |
| Raw Y | 25 to 261 |

For the 180-degree landscape orientation:

- High raw Y maps to the left side.
- Low raw Y maps to the right side.
- High raw X maps to the top.
- Low raw X maps to the bottom.

LVGL root screens are scrollable by default.

Remove `LV_OBJ_FLAG_SCROLLABLE` from each new screen.

### Application Partition

| Field | Content |
|---|---|
| Symptom | Wi-Fi makes the application exceed the default 1 MB partition. |
| Cause | The default single-application partition is too small. |
| Action | Use the project partition table. |
| Status | The networked firmware builds and boots. |

Generated configuration can override changed defaults.

Regenerate the build configuration after a partition change.

### Spotify HTTPS

ESP-IDF 6 does not provide the previous bundled JSON component.

The CYD build uses a bounded project parser for the required Spotify fields.

The parser must allow whitespace around JSON colons.

List every required component explicitly in `PRIV_REQUIRES`.

Enable the ESP-IDF certificate bundle.

Set `crt_bundle_attach = esp_crt_bundle_attach` for each HTTPS client.

Do not disable certificate verification.

## Waveshare ESP32-P4 ESP-IDF 5.5

### Toolchain

Use ESP-IDF 5.5.x.

ESP-IDF 5.4 is too old for the adapter.

ESP-IDF 6.0 removed a component that the board package requires.

### Hosted Wi-Fi

The ESP32-P4 uses the onboard ESP32-C6 through SDIO.

The build requires both `esp_wifi` and `esp_wifi_remote`.

`esp_wifi_remote` supplies the remote implementation.

`esp_wifi` supplies the public API headers.

The build also requires `esp_hosted`, `esp_netif`, and `esp_event`.

### IRAM Overflow

| Field | Content |
|---|---|
| Symptom | The linker reports discarded sections or an IRAM overflow. |
| Cause | LVGL fast-memory functions consume the remaining IRAM. |
| Action | Set `CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM=n`. |
| Status | Hosted Wi-Fi links and runs. |

Do not treat this failure as a general heap shortage.

### Memory Classes

The ESP32-P4 has 768 KB of internal SRAM.

The board also has 32 MB of PSRAM.

Use PSRAM for these large objects:

- Network response buffers.
- Album-art buffers.
- Thumbnail pools.
- Cover Flow buffers.
- Display frame buffers.

Keep required DMA objects in internal memory.

The HA build reserves 64 KB of internal memory for DMA allocations.

Do not reduce this value without a long memory trace.

Use `idf.py size` and `idf.py size-components` after a large dependency change.

### PPA And LVGL Draw Units

PPA display acceleration is enabled.

Keep `CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT=1`.

Two software draw units conflict with the PPA transaction queue.

The conflict causes a boot loop.

### LVGL Card Transforms

Do not apply object-level scale or opacity transforms to album cards.

The display path can create an incorrect intermediate layer.

Do not enable `LV_USE_MATRIX`.

Transform image data through the established Cover Flow renderer.

### Fonts

Do not use runtime Tiny TTF.

The previous Tiny TTF path caused cache failures and rasterizer assertions.

Use the compiled project fonts.

### JPEG Decoder State

Allocate the JPEG decoder working structure in internal SRAM.

Do not allocate this structure in PSRAM.

The incorrect memory class caused intermittent store faults.

Large decoded pixel buffers can remain in PSRAM.

### Cover Flow

Cover Flow uses a row-major PSRAM compositor.

It prepares per-column trapezoid data before composition.

It skips pixels behind nearer covers.

Do not restore the previous column-major destination writes.

Do not clear the complete output buffer before every frame.

Profile both preparation and composition before a geometry change.

### Network Clients

The player-state request uses a persistent HTTPS client.

The command path also reuses its client.

Close the client after a transport failure.

Observe Spotify `Retry-After` values after rate limiting.

Use the slower request interval during paused or idle states.

### Wi-Fi Recovery

The shared application core owns the reconnect timer.

Initial connection failure must not prevent interface startup.

The reconnect timer continues after fast retries finish.

### Home Assistant WebSocket

Handle both abnormal disconnect and clean close events.

Restart a connection after `auth_invalid`.

Use the authentication watchdog for an incomplete handshake.

Use the idle ping and pong timeout for a half-open link.

Do not stop or restart the client from its event callback.

Schedule that work from the application tick.

### OTA Partitions

The Waveshare builds use two 10 MB OTA slots.

They also use an OTA data partition and a crash-dump partition.

The NVS partition remains at its established address.

Do not change the partition layout without a USB recovery test.

> WARNING: `idf.py erase-flash` removes saved device settings.

### Build Cache

`sdkconfig` overrides `sdkconfig.defaults`.

Delete generated configuration only when a default change must take effect.

Do not delete the hand-maintained defaults file.

### Editor Cache

An open editor can overwrite a newer external file change.

Reload the file before you save it.

Use a unique boot log line when you must confirm the flashed source version.
