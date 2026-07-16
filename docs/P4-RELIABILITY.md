# ESP32-P4 Reliability Budget

This document is the release gate for both Waveshare P4 firmware targets. It
exists because a successful compile and abundant PSRAM do not prove that the
ESP32-C6/ESP-Hosted network path can survive runtime bursts.

No firmware can make a literal guarantee that a hardware fault or upstream
driver defect will never crash. The project standard is instead: known failure
modes are prevented by design, resource regressions fail before flashing, and
nonessential work degrades cleanly before a critical allocator can fail.

## Root cause of the July 2026 crashes

Both captured panics were allocation failures in ESP-Hosted's SDIO receive
driver:

- `sdio_rx_get_buffer ... assert(*buf)` while receiving a large HA response.
- `sdio_push_data_to_queue ... assert(pkt_rxbuff)` while splitting the stream
  into packets after Sendspin was active.

The device still had about 25 MB of PSRAM, but only about 25-26 KB of internal
SRAM remained. ESP-Hosted's packet/mempool buffers need internal and/or
DMA-capable memory; PSRAM cannot substitute for that pool. The configured SDIO
RX queue is 20 packets, and bursts can therefore need tens of KB of internal
memory even when the application payload itself is assembled in PSRAM.

The trigger was cumulative rather than one bad feature:

| Consumer | Previous placement | Approximate pressure |
|---|---:|---:|
| HA worker stack | internal | 8 KB |
| HA media/art worker stack | internal | 8 KB |
| UI sound worker stack | internal | 4 KB |
| HA command + media queues | internal | about 3 KB |
| HA WebSocket worker | internal | 8 KB configured |
| ESP-Hosted/RPC/SDIO workers and packet pool | internal/DMA | driver-owned, burst-dependent |
| TLS/lwIP control allocations | mixed | connection-dependent |

The first four rows are application-owned and now explicitly live in PSRAM.
The WebSocket and transport rows stay internal because they are library/driver
owned. This creates reserve for the memory that genuinely cannot move.

## Memory domains and hard limits

### Internal and DMA-capable SRAM

This is the critical resource. Runtime checks use:

- Desired steady state: at least **48 KB free** and a **24 KB largest block**.
- Minimum before a nonessential installation-wide HA snapshot: **40 KB free**
  and a **16 KB largest block**.
- Heavy optional HTTPS/JPEG cover work: **64 KB free** and a **32 KB largest
  block**, and never while Sendspin music is active.

Falling below the desired line is a warning and must be investigated during a
soak. Falling below an operation's minimum must defer that operation. Do not
lower a threshold to make a warning disappear; first account for the new
allocation and move eligible application data to PSRAM.

### PSRAM

PSRAM carries display buffers, album pools, art buffers, large HTTP/WebSocket
bodies, application task stacks, and queues. It is plentiful but slower and not
a replacement for all DMA/internal allocations. A feature that adds a large
pool must:

1. use an explicit `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT` allocation;
2. check failure and retain a functional fallback;
3. free/rebuild it predictably across theme or screen changes;
4. report it in diagnostics if it is persistent.

### Flash/application partition

Each P4 target has an 8 MB factory application partition. The hard build gate
requires at least **512 KB and 8% free**. Ten percent is the preferred planning
margin. Generated album data, fonts, codec roles, and new libraries can grow
the image quickly even when runtime memory is unaffected.

The bootloader occupies a separate 24 KB region and currently has only **624
bytes (3%) free**. Normal application features do not affect it, but ESP-IDF,
boot security, flash, or partition configuration can. The post-build gate
requires at least 256 bytes free; ESP-IDF's own size check remains the final
hard overflow stop.

## Network architecture rules

1. HA `get_states` is installation-wide and may be hundreds of KB. It is not a
   normal refresh primitive.
2. One mandatory startup snapshot seeds typed device, light, and media-library
   caches. Page opens render those caches immediately.
3. Devices and lights share one inventory refresh. It is single-flight,
   rate-limited to once per 15 seconds, and blocked below the internal-memory
   reserve. Album library browsing reuses cached media-player IDs.
4. Cover downloads are serialized one album at a time, back off after failure,
   and stop during Sendspin playback.
5. UI callbacks enqueue work only. TLS, WebSocket, Spotify, HA, JPEG, and I2S
   work must never run under the LVGL lock.
6. New streaming/metadata roles are disabled until their payload, stack, and
   transport costs have a written budget and a soak result.

## Task and queue rules

- Application-owned worker stacks and non-ISR queues use the ESP-IDF
  `...CreateWithCaps()` APIs with PSRAM capabilities.
- Driver tasks, ISR-facing objects, DMA descriptors, and objects explicitly
  documented as internal stay internal.
- Every task-create and queue-create result is checked. A failed optional
  feature disables itself; a failed core command path stops initialization
  with an explicit error.
- Stack sizes are not reduced from guesswork. Enable diagnostics, exercise the
  task's worst path, record its high-water mark, then preserve at least 2 KB or
  30% margin, whichever is larger.

## Build gates

Run from the repository root before syncing:

```powershell
powershell.exe -ExecutionPolicy Bypass -NoProfile -Command ". 'C:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1'; Set-Location 'C:\Users\User\Documents\home-controller'; python scripts\check_p4_reliability.py both"
```

After each private build, run in the private repository root:

```powershell
powershell.exe -ExecutionPolicy Bypass -NoProfile -Command ". 'C:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1'; Set-Location 'C:\Users\User\Documents\home-controller - Private'; python scripts\check_p4_reliability.py both --post-build"
```

The check currently enforces PSRAM/WiFi configuration, one LVGL software draw
unit, PSRAM-backed application workers and queues, the HA snapshot call-site
limit and memory gate, minimal Sendspin roles, WebSocket reassembly cap, and app
partition headroom. Add a guard whenever a future regression can be recognized
statically.

## Runtime diagnostics and acceptance test

`app_core_reliability` registers ESP-IDF's failed-allocation callback and logs:

- current/minimum internal free memory;
- largest internal block;
- DMA-capable free/largest blocks;
- PSRAM free memory;
- registered application task stack high-water marks.

Before marking a network/audio/UI feature hardware-verified:

1. Cold boot with HA and speakers initially offline, then bring them online.
2. Open Devices and Lights repeatedly while their 5-second UI timers run.
3. Start Sendspin playback, scrub volume, navigate every page, and switch output.
4. Search/add albums and allow runtime art repair after playback stops.
5. Toggle and scrub lights while device inventory refresh is due.
6. Run at least 60 minutes interactively, then an 8-hour playback/idle soak.
7. Reject the change on any allocation failure, watchdog, stack HWM under the
   required margin, declining minimum heap, repeated reconnect loop, audio
   underrun, or internal reserve below the desired line.

For suspected overwrite/use-after-free bugs, use a temporary debug build with
ESP-IDF heap corruption detection and narrow the failing operation. Do not ship
comprehensive heap poisoning enabled without re-measuring timing and memory.

References: [ESP-IDF heap capabilities and allocation-failure callback](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32p4/api-reference/system/mem_alloc.html),
[ESP-IDF external-memory FreeRTOS objects](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32p4/api-reference/system/freertos.html).
