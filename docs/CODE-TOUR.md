# Code Tour: How This Firmware Works

This guide is for a person new to the project, or new to embedded C code.
Read this document before you read the source code. This document explains
four items: what the device is, the main ideas behind the code, the events
that happen while the program runs, and the purpose of each file. The document
then points to further reading.

This tour describes the lead build, `waveshare/esp-idf/`. This build runs on
the Waveshare ESP32-P4 board and connects straight to Spotify. The other build
folders use the same ideas, with different network backends or different
hardware. See [section 9](#9-the-other-build-folders).

---

## 1. What the device is

Home Controller is a handheld music remote control. The device shows a
touch-screen browser of the album collection. The device also shows a
now-playing screen, with cover art, the track title, the artist name, and a
progress bar. The user taps an album to play it. The user taps on-screen
controls, or turns a physical knob, to play, pause, skip, seek, and change the
volume.

The music itself plays through a phone, a laptop, a Spotify Connect speaker,
a Sonos speaker, or the speaker built into the device. The device is a remote
control. The device is not the primary speaker in most setups.

The direct Spotify build controls Spotify over the home WiFi network, through
the Spotify web API. This is the same API that the Spotify phone app uses.

---

## 2. The hardware in one paragraph

The main processor is an ESP32-P4, a dual-core microcontroller. The device
has no general operating system, unlike a laptop. The firmware is the only
program that runs, from power-on to power-off. The board has a 4.3-inch touch
screen. A second chip, an ESP32-C6, provides the WiFi connection over an SDIO
link. The board has two kinds of memory: a small, fast memory (internal SRAM),
and a large, slower memory (PSRAM, about 32 MB on the verified board). The
board also has 32 MB of flash storage, for the program and the album
thumbnail images. Keep the two memory types in mind while you read the code.
Several design choices exist only to place large data in the slower PSRAM,
and keep the scarce fast memory free. See `docs/P4-RELIABILITY.md` for the
exact memory budget and the reasons for each rule.

---

## 3. Five main ideas

A reader who understands these five ideas can follow the rest of the code.

### a) Tasks: doing several things at the same time

The firmware uses FreeRTOS, a small operating system that provides tasks. A
task is like a lightweight thread. Each task is an independent stream of
work, and the chip switches rapidly between tasks. This design makes several
jobs appear to run at the same time. The firmware splits the work this way,
so that a slow job never freezes a fast job:

- The LVGL task draws the whole screen. LVGL is the graphics library.
- The `spotify_task` task does the slow network work, for calls to Spotify
  and to Sonos.
- A small audio task plays the user-interface audio effects.

### b) The command queue: a one-way mailbox

A screen tap does not call Spotify directly. A direct call would freeze the
screen for one or two seconds. Instead, the UI writes a small message, a
command of type `scmd_t`, for example `SCMD_PLAY_ALBUM`. The UI places this
message into a queue, named `s_cmd_queue`. This queue is a thread-safe
mailbox. The `spotify_task` task takes each message from the queue in turn,
and does the real work. Information flows in one direction only: from the UI
and the input code, through the queue, to `spotify_task`. This queue is the
reason a screen tap responds right away.

### c) The display lock: one task draws at a time

Only one task may touch an LVGL object at any time. Code outside the LVGL
task must take a lock first, make its change, then release the lock. An
example is `spotify_task` when it shows a new track title. A person rarely
takes this lock by hand. The `ui_*()` helper functions in `ui.c` take the
lock for the code that calls them.

Rule: do not change an LVGL object from outside the LVGL task without the
lock. Call a `ui_*()` helper function instead.

### d) Blocking calls: why the network work has its own task

A call to Spotify uses an HTTPS request over WiFi. This request can take from
half a second to two seconds. A function that waits for this length of time
is a blocking call. A blocking call on the drawing task would freeze the
screen for that time. This is the reason the `spotify_task` task exists. This
task is the place where a blocking call may run, away from the UI code.

### e) Fast memory and large memory: SRAM and PSRAM

The large buffers, for example the web-response buffer, the decoded album
art, and the thumbnail pools, go into the roomy PSRAM. A rule in
`sdkconfig.defaults` sets this placement. This placement keeps the scarce,
fast SRAM free for the WiFi stack and the network encryption code. A call to
`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` means "allocate this block in
PSRAM."

---

## 4. Events during a run

### Power-on

The `app_main()` function, near the bottom of `main.c`, runs once. This
function sets up the device in this order:

1. Load the saved settings from NVS (non-volatile storage).
2. Reserve the memory for the album art.
3. Start the display.
4. Start the user-interface audio system.
5. Connect to the WiFi network.
6. Build the UI screens.
7. Create the command queue.
8. Start the `spotify_task` task.

After this sequence, `app_main()` exits. The tasks keep the device running.

### A user taps an album

1. The touch event occurs on a card in `ui.c`. This code calls
   `ui_request_play(uri)`.
2. This call places an `SCMD_PLAY_ALBUM` command into the queue, and returns
   right away.
3. The `spotify_task` task takes the command from the queue, and calls
   `spotify_play_album()`. This function makes the slow HTTPS call that tells
   Spotify to start the album on the active device.
4. On its next poll, described below, `spotify_task` sees the new track, and
   calls `ui_set_track_info()`. This function updates the now-playing screen,
   under the display lock.

### A song plays: the poll loop

The `spotify_task` task asks Spotify for the current playback state, through
a `GET /me/player` call, every 5 seconds during playback. The task asks every
15 seconds while playback is paused, to reduce network load. Each answer
updates the UI. When the track changes, the firmware downloads the new cover
image, at about 640 pixels. The firmware shrinks and decodes this image to
320 by 320 pixels, then shows the image. Between polls, a small timer moves the progress
bar forward. This timer makes the bar move smoothly, instead of jumping only
once every 5 seconds.

### A user controls a Sonos speaker

The Spotify API does not support direct control of a Sonos speaker. The user
can select a Sonos speaker as the playback target. In this case, the firmware
talks to that speaker directly, over the local network, through the Sonos
SOAP and UPnP control protocol. See `sonos.c`. The rest of the code does not see this
difference. The UI still posts the same commands. The `spotify_task` task
decides whether to route each command to Spotify or to the Sonos speaker.

---

## 5. The files: purpose and a reading order

Read the files in roughly this order. Start with the header comment in
`main.c`, then read this table.

| File | Purpose |
|---|---|
| `main.c` | Start here. The entry point and the main sequence: startup, the three tasks, the command queue, the WiFi connection, the poll loop, and the routing between Spotify and Sonos. The header comment is a short version of this tour. |
| `ui.c` (with `ui.h`) | The full user interface, built with LVGL: the album browser, the now-playing screen, the Settings screens, the five interface modes (BASIC, GLYPH, PIXEL, PAPER, and BOLD), the DEVELOPER settings tab, the Cover Flow effect, the volume controls, and the WiFi signal display. This file lives in the shared `waveshare/components/p4_shared/` component, so both P4 builds use it. This is the largest file in the project. Other code calls the screen only through the `ui_*()` functions declared in `ui.h`. |
| `ui_tune.h` | A header of adjustable numeric values: text positions, colours, and spacing, grouped by interface mode. The DEVELOPER settings tab can override many of these values at runtime. The `EXPORT TO SERIAL` control in that tab prints values in this header's format, for a developer to copy into this file. |
| `spotify.c` (with `spotify.h`) | The Spotify web-API client: sign-in and access-token refresh, the playback-state poll, the play, pause, next, seek, and volume commands, the device list, and the cover-art download. This file includes a small hand-written JSON reader, since the code needs only a few fields from each response. |
| `sonos.c` (with `sonos.h`) | Local control of a Sonos speaker over the network, through the SOAP and UPnP protocol on port 1400: play, pause, seek, volume, the current playback state, and the command to start an album. |
| `album_art.cpp` (with `album_art.h`) | Decodes a downloaded JPEG cover image into the pixel format the screen uses. This file uses C++ because the JPEG library is a C++ library. |
| `albums.c` (with `albums.h`) | The list of albums: title, artist, and Spotify ID. A script generates this file from `spotify-albums-list.txt`. Do not edit this file by hand. |
| `album_thumbs.c` (with the matching header) | The small thumbnail images shown in the browser, built into the program. A script also generates this file. |
| `littlefs.c` (with the matching header) | A small filesystem on the flash chip. The firmware uses this filesystem as scratch space for a downloaded cover image, before the image is decoded. |
| `audio.c` (with `audio.h`) | Generates the small user-interface audio effects, for example clicks and chimes, and plays the effects through the speaker on the board. |
| `lv_font_*.c` | Generated font files, for the pixel, dot, mono, slab, and Jost Bold fonts that the interface modes use. A script produces these files. Do not edit these files by hand. |
| `secrets.h` | The WiFi and Spotify credentials for one build. This file is not in the git repository. Copy `secrets.h.example`, and fill in the real values. Never commit this file. |

---

## 6. The big-picture diagram

```
   [user taps or turns a control]           [the internet]
         |                                        |
         v                                        v
   ui.c (LVGL task) --ui_request_*()-->  s_cmd_queue  --->  spotify_task (main.c)
         ^                                                     |  |
         |  ui_set_track_info() / ui_art_refresh()             |  |  HTTPS
         |  (each call takes the display lock)                 |  v
         +-----------------------------------------------------+  spotify.c  --> Spotify
                                                               |
                                                               +> sonos.c   --> Sonos (local network)
```

The UI code and the network code never call each other directly. The two
sides pass messages through the queue, in one direction. The network code
updates the screen only through the locked `ui_*()` helper functions.

---

## 7. Rules the code must not break

Each of these rules protects against a fault that compiles without warning,
but causes a failure later. `CLAUDE.md` has the full list, with the reason
for each rule. The rules most relevant to a new reader are these:

- Do not change an LVGL object from outside the LVGL task without the
  display lock. Call a `ui_*()` helper function instead. A violation of this
  rule causes a rare screen fault or crash, which is difficult to reproduce.
- The `spotify_task` task must stay the only writer of the current playback
  state: the track record, the album-art buffers, and the Sonos state. Other
  code requests a change by posting a command to the queue. Other code must
  not change this state directly.
- Cover Flow has display constraints described in the comments near its code
  in `ui.c`. Certain LVGL object transforms cause a paint fault on the
  rotated display, so the effect uses direct drawing instead. Do not replace
  this direct drawing with an object transform.
- The album list and the fonts are generated files. Edit the source list or
  run the matching script. Do not edit `albums.c` or an `lv_font_*.c` file by
  hand.
- The BOLD interface mode is not hardware-verified. Treat a change near this
  mode with the same care as an unverified feature.

---

## 8. Where to read next

- `CLAUDE.md`, in the repository root: the main architecture and decision
  record, and the full list of constraints, for the whole project.
- `README.md`, in `waveshare/esp-idf/`: the build and flash steps, and the
  hardware bring-up checkpoints.
- `docs/P4-TODO.md`: the planned and pending work items.
- `docs/PORT-NOTES.md`: the chip and toolchain issues found during
  development.
- `docs/ROADMAP.md` and `docs/PENDING.md`: the longer-term plan, and the
  list of work that is complete but not yet hardware-verified.
- `tools/README.md` and `tools/theme-bench.html`: the browser design bench
  for the interface modes and the DEVELOPER settings tab.

---

## 9. The other build folders

The project targets several hardware variants. Each variant has its own
folder, and each variant shares the main ideas above.

- `waveshare/esp-idf/`: this build, on the ESP32-P4 board, with a direct
  Spotify connection. This tour uses this build, since it is the simplest
  starting point.
- `waveshare/esp-idf-ha/`: the same board and the same user interface, but
  this build controls music through Home Assistant and Music Assistant,
  instead of a direct Spotify connection. This is the verified daily build.
  This build can also play music through the speaker built into the device. This
  build shares its interface, audio, and album code with `waveshare/esp-idf/`
  through the common `waveshare/components/p4_shared/` folder. Only the
  network backend differs: `ha_client.c` in place of `spotify.c`.
- `cyd/esp-idf/`: an earlier, smaller board, named CYD, with physical knobs
  and buttons, and a direct Spotify connection. This build shares its
  interface and input code through a common folder,
  `cyd/components/cyd_shared/`.
- `cyd/esp-idf-ha/`: the CYD board, but with a Home Assistant connection in
  place of a direct Spotify connection.
- `cyd/platformio/`: the original CYD build, on the Arduino toolchain. This
  build is in maintenance mode.

A reader new to the project should stay inside `waveshare/esp-idf/`. Every
file in this tour is in that build or in its shared component.
