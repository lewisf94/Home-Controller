# ESP32-P4 Reliability Budget

This document states the release gate for both Waveshare P4 firmware
targets. This document exists because a successful compile, and abundant
PSRAM, do not prove that the ESP32-C6/ESP-Hosted network path can survive a
runtime burst.

No firmware can give a literal guarantee against a hardware fault or an
upstream driver defect. The project standard states three rules instead:
the design must prevent each known failure mode; a resource regression must
fail before flashing; nonessential work must degrade in a controlled way,
before a critical allocation can fail.

## Root cause of the July 2026 crashes

Each captured crash was an allocation failure in the SDIO receive driver of
ESP-Hosted. The crash log recorded these two assert failures:

- `sdio_rx_get_buffer ... assert(*buf)`, during receipt of a large HA
  response.
- `sdio_push_data_to_queue ... assert(pkt_rxbuff)`, during a stream split
  into packets, after Sendspin became active.

At each crash, the device still had approximately 25 MB of PSRAM. The
device had only approximately 25 to 26 KB of internal SRAM remaining.
ESP-Hosted needs internal memory, or DMA-capable memory, or both, for its
packet and mempool buffers. PSRAM cannot replace this pool. The configured
SDIO RX queue holds 20 packets. A burst can therefore need tens of KB of
internal memory, even when the application payload itself sits in PSRAM.

The trigger was a cumulative memory pressure, not one feature:

| Consumer | Previous placement | Approximate pressure |
|---|---:|---:|
| HA worker stack | internal | 8 KB |
| HA media/art worker stack | internal | 8 KB |
| UI sound worker stack | internal | 4 KB |
| HA command + media queues | internal | about 3 KB |
| HA WebSocket worker | internal | 8 KB configured |
| ESP-Hosted/RPC/SDIO workers and packet pool | internal/DMA | driver-owned, burst-dependent |
| TLS/lwIP control allocations | mixed | connection-dependent |

The application owns the first four rows in this table. These four rows now
live in PSRAM, by explicit placement. The WebSocket row and the transport
row stay in internal memory, because a library or a driver owns each of
these rows. This placement keeps a memory reserve, for the memory that
cannot move.

## Memory domains and hard limits

### Internal and DMA-capable SRAM

This memory is the critical resource. The runtime checks apply these
values:

- Desired steady state: at least **48 KB free**, and a **24 KB largest
  block**.
- Minimum before a nonessential installation-wide HA snapshot: **40 KB
  free**, and a **16 KB largest block**.
- Heavy optional HTTPS/JPEG cover work: **64 KB free**, and a **32 KB
  largest block**. This work must never run while Sendspin music plays.

A value below the desired line produces a warning. This warning needs
investigation during a soak test. A value below the minimum for an
operation must defer that operation. Do not lower a threshold to remove a
warning. Instead, account for the new allocation, and move eligible
application data to PSRAM.

### PSRAM

PSRAM holds display buffers, album pools, art buffers, large HTTP and
WebSocket bodies, application task stacks, and queues. PSRAM capacity is
large, but PSRAM is slower, and PSRAM cannot replace each DMA or internal
allocation. A feature that adds a large pool must meet four conditions:

1. Use an explicit `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT` allocation.
2. Check each allocation failure, and keep a functional fallback path.
3. Free and rebuild the pool in a predictable way, across a theme change or
   a screen change.
4. Report the pool in diagnostics, when the pool persists.

### Flash and application partition

Each P4 target has an 8 MB factory application partition. The build gate
requires at least **512 KB and 8% free** in this partition. Ten percent
free is the preferred planning margin. Generated album data, fonts, codec
roles, and a new library can grow the image quickly, even with no effect
on runtime memory.

The bootloader occupies a separate 24 KB region. This region currently has
only **624 bytes (3%) free**. A normal application feature has no effect on
this region. A change to ESP-IDF, boot security, flash configuration, or
partition configuration can affect this region. The post-build gate
requires at least 256 bytes free in this region. The size check inside
ESP-IDF remains the final, hard stop against an overflow.

## Network architecture rules

1. The HA `get_states` call covers the full installation, and the response
   can reach hundreds of KB. This call is not a normal refresh action.
2. One mandatory startup snapshot fills typed caches, for devices, lights,
   and the media library. Each page open then renders from these caches
   immediately.
3. Devices and lights share one inventory refresh action. This action runs
   single-flight, at a rate limit of once per 15 seconds. This action stops
   when the internal-memory reserve is below its limit. Album library
   browsing reuses the cached media-player IDs.
4. The firmware downloads one cover at a time, in sequence. The firmware
   waits longer after each failure. The firmware stops cover downloads
   during Sendspin playback.
5. A UI callback must only add work to a queue. TLS work, WebSocket work,
   Spotify work, HA work, JPEG work, and I2S work must never run while the
   code holds the LVGL lock.
6. An output transfer to a Spotify Connect device can need a Spotify
   catalogue search. This search is a blocking HTTPS request on the HA task.
   This search therefore uses the heavy-work limits above, and this search
   never runs during Sendspin playback. A blocked search retries four times,
   at 1.5-second intervals, then reports a failure to the user. The transfer
   has already paused the old output at that point, so a short retry is
   better than immediate silence.
7. A new streaming or metadata role stays disabled until it has a written
   budget for payload cost, stack cost, and transport cost. This role also
   needs a soak-test result before it becomes active.

## Task and queue rules

- An application-owned worker stack, and a non-ISR queue, must use an
  ESP-IDF `...CreateWithCaps()` function, with PSRAM capability flags.
- A driver task, an ISR-facing object, a DMA descriptor, and an object
  documented as internal, must each stay in internal memory.
- The code must check the result of each task-create call and each
  queue-create call. A failed optional feature must disable itself. A
  failed core command path must stop initialization, with an explicit
  error.
- A stack size must never decrease from a guess. Use this sequence to set a
  stack size:
  1. Enable diagnostics.
  2. Exercise the worst-case path of the task.
  3. Record the high-water mark.
  4. Keep a margin of at least 2 KB, or 30%, whichever value is larger.

## Build gates

Run this command from the repository root, before you sync the private
folder:

```powershell
powershell.exe -ExecutionPolicy Bypass -NoProfile -Command ". 'C:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1'; Set-Location 'C:\Users\User\Documents\home-controller'; python scripts\check_p4_reliability.py both"
```

Run this command in the private repository root, after each private build:

```powershell
powershell.exe -ExecutionPolicy Bypass -NoProfile -Command ". 'C:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1'; Set-Location 'C:\Users\User\Documents\home-controller - Private'; python scripts\check_p4_reliability.py both --post-build"
```

The check currently enforces these items:

- PSRAM and WiFi configuration.
- One LVGL software draw unit.
- PSRAM-backed application workers and queues.
- The HA snapshot call-site limit and memory gate.
- Minimal Sendspin roles.
- The WebSocket reassembly cap.
- Application partition headroom.

Add a new
guard whenever a future regression has a static, recognizable pattern.

## Runtime diagnostics and acceptance test

The `app_core_reliability` component registers the failed-allocation
callback from ESP-IDF. This component logs these values:

- the current and minimum internal free memory;
- the largest internal free block;
- the DMA-capable free memory and largest DMA-capable block;
- the PSRAM free memory;
- the stack high-water mark of each registered application task.

Complete these seven steps before you mark a network, audio, or UI feature
as hardware-verified:

1. Start from a cold boot, with HA and each speaker offline. Then bring HA
   and each speaker online.
2. Open the Devices page and the Lights page repeatedly, while their
   5-second UI timers run.
3. Start Sendspin playback. Move the volume control. Open each page. Change
   the output target.
4. Search for an album. Add the album. Allow the runtime art repair
   process to run after playback stops.
5. Change a light state, and move a light control, while a device
   inventory refresh is due.
6. Run an interactive session for at least 60 minutes. Then run an 8-hour
   soak test, of playback and idle time combined.
7. Reject the change if you observe any of these events:
   - An allocation failure.
   - A watchdog reset.
   - A stack high-water mark under its required margin.
   - A declining minimum heap value.
   - A repeated reconnect loop.
   - An audio underrun.
   - An internal reserve below the desired line.

For a suspected memory-overwrite bug, or a suspected use-after-free bug,
build a temporary debug build with ESP-IDF heap corruption detection. Use
this build to narrow down the failing operation. Do not release a build
with full heap poisoning enabled, unless you first measure the new timing
and the new memory cost.

References: [ESP-IDF heap capabilities and allocation-failure callback](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32p4/api-reference/system/mem_alloc.html),
[ESP-IDF external-memory FreeRTOS objects](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32p4/api-reference/system/freertos.html).
