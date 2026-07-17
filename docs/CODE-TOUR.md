# Code Tour — how this firmware works (start here)

A plain-language guide for someone new to the project, or new to embedded C.
Read this **before** diving into the source. It explains what the device is, the
handful of big ideas you need, what happens as the program runs, and what each
file does — then points you at where to start reading.

This tour describes the **lead build**, `waveshare/esp-idf/` (the Waveshare
ESP32-P4 board, talking straight to Spotify). The other build folders are
variations on the same ideas — see [the multi-build note](#9-the-other-build-folders).

---

## 1. What the device is

A **handheld Spotify remote**. It has a colour touch screen that shows your
album collection as a browsable carousel, and a "now playing" screen with the
cover art, title, artist, and a progress bar. You tap an album to play it, and
tap on-screen buttons (later: a physical knob and buttons) to play/pause, skip,
seek, and change volume. The music itself plays on a phone, a laptop, a Spotify
Connect speaker, or a Sonos — the device is the *remote control*, not the
speaker.

It controls Spotify over your home WiFi using Spotify's web API (the same API a
phone app uses).

---

## 2. The hardware, in one paragraph

The brain is an **ESP32-P4**: a small, cheap dual-core microcontroller. Unlike a
laptop it has **no operating system** like Windows or Linux — our program *is*
the only thing running, from power-on to power-off. The board adds a **4.3" touch
screen**, a second chip (an **ESP32-C6**) that provides the **WiFi**, and two
kinds of memory: a *small, fast* one (768 KB of "internal SRAM") and a *large,
slower* one (about 32 MB of "PSRAM"). There's also 32 MB of flash storage holding
the program and the album thumbnail images. Keep the two memory types in mind —
several design choices exist purely to put big things in the slow-but-roomy PSRAM
and keep the scarce fast memory free.

---

## 3. Five big ideas

If you understand these five, the rest of the code makes sense.

### a) Tasks — doing several things at once
We use **FreeRTOS**, a tiny operating system that gives us **tasks**. A task is
like a lightweight thread: an independent stream of work the chip rapidly
switches between, so several things appear to happen at once. We split the work
so a slow job never freezes a fast one:
- the **LVGL task** does all the on-screen drawing (LVGL is the graphics library);
- **`spotify_task`** does the slow internet work (talking to Spotify);
- a small **audio task** plays the UI beeps/clicks.

### b) The command queue — the "mailbox"
When you tap something, the UI does **not** immediately call Spotify (that would
freeze drawing for a second or two). Instead it writes a small message — a
command, type `scmd_t`, such as `SCMD_PLAY_ALBUM` — and drops it into a **queue**
(`s_cmd_queue`), a thread-safe mailbox. `spotify_task` takes messages out one at
a time and does the real work. Information flows **one way**:
`UI / input → queue → spotify_task`. This is the trick that keeps taps instant.

### c) The display lock — only one painter at a time
LVGL can only be touched by one task at a time. So any code **outside** the LVGL
task that wants to change the screen (e.g. `spotify_task` showing a new song
title) must first **take a lock**, make its change, then release it. You rarely
do this by hand: the `ui_*()` helper functions in `ui.c` take the lock for you.
**Rule: never poke LVGL objects directly from outside the LVGL task — call a
`ui_*()` helper.**

### d) "Blocking" calls — why the network lives on its own task
Talking to Spotify means an HTTPS request over WiFi, which can take 0.5–2
seconds. A function that sits and waits like that is called **blocking**. If a
blocking call ran on the drawing task, the screen would freeze for those seconds.
That's the whole reason `spotify_task` exists — it's where the blocking is
allowed to happen, out of sight of the UI.

### e) Fast memory vs big memory (SRAM vs PSRAM)
The big buffers — the 256 KB web-response buffer, the decoded album art, the
thumbnail pools — are steered into the roomy **PSRAM** by a rule in
`sdkconfig.defaults`, so they don't use up the scarce **fast SRAM** (which WiFi
and the network encryption need). When you see `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`
that means "give me PSRAM specifically."

---

## 4. What happens when… (follow the flow)

### …the device powers on
`app_main()` (bottom of `main.c`) runs once and sets everything up in order:
load saved settings (NVS) → reserve the album-art memory → start the display →
start the UI-sound system → connect to WiFi → build the UI screens → create the
command queue → launch `spotify_task`. After that, `app_main` is done and the
tasks keep the device running forever.

### …you tap an album
1. The touch lands on a card in `ui.c`, which calls `ui_request_play(uri)`.
2. That drops an `SCMD_PLAY_ALBUM` command into the queue (instant) and returns.
3. `spotify_task` picks it up and calls `spotify_play_album()` — the slow HTTPS
   call — which tells Spotify to start that album on your active device.
4. On its next poll (below), `spotify_task` sees the new track and calls
   `ui_set_track_info()`, which (under the display lock) updates the now-playing
   screen.

### …a song is playing (the poll loop)
`spotify_task` asks Spotify "what's playing?" (`GET /me/player`) every 5 seconds
while playing (15 s when paused, to be gentle). Each answer updates the UI. When
the **track changes**, it downloads the new cover (a ~640 px JPEG), shrinks and
decodes it to 320×320, and shows it. Between polls, a small timer ticks the
progress bar forward so it moves smoothly instead of jumping every 5 seconds.

### …you control a Sonos
Spotify's API refuses to control a Sonos speaker directly. So when a Sonos is the
chosen target, the firmware talks to it **itself**, over your local network,
using Sonos's older control protocol (see `sonos.c`). To the rest of the code it
looks the same — the UI still just posts commands; `spotify_task` decides whether
to route them to Spotify or to the Sonos.

---

## 5. The files — what each does, and where to start

**Read in roughly this order.** Start with `main.c`'s header comment, then this
list.

| File | What it is |
|---|---|
| **`main.c`** | **Start here.** The entry point and the conductor: boot setup, the three tasks, the command queue, the WiFi connection, the poll loop, and the Spotify-vs-Sonos routing. Its header comment is a mini version of this tour. |
| **`ui.c`** (+ `ui.h`) | The whole user interface, built with LVGL: the album carousel, the now-playing screen, the Settings screens, the themes, the Cover Flow effect, the volume controls, the WiFi bars. Lives in the shared `waveshare/components/p4_shared/` component (both P4 builds use it). It's the biggest file by far. The `ui_*()` functions in `ui.h` are the "public" way other code talks to the screen. |
| **`ui_tune.h`** | A header full of "tweak these by eye" numbers — text positions, colours, spacings — gathered in one place per theme so you can adjust the look without hunting through `ui.c`. |
| **`spotify.c`** (+ `spotify.h`) | The Spotify web-API client: log in / refresh the access token, ask what's playing, send play/pause/next/seek/volume, list devices, download cover art. Includes a tiny hand-written JSON reader (we only need a few fields). |
| **`sonos.c`** (+ `sonos.h`) | Local control of a Sonos speaker over the network (the SOAP/UPnP protocol on port 1400): play/pause/seek/volume, read what it's playing, and start an album on it. |
| **`album_art.cpp`** (+ `album_art.h`) | Decodes a downloaded JPEG cover into the pixel format the screen uses. (C++ because the JPEG library is C++.) |
| **`albums.c`** (+ `albums.h`) | The list of your albums (title, artist, Spotify id). **Generated** from `spotify-albums-list.txt` by a script — don't hand-edit it. |
| **`album_thumbs.c`** (+ `.h`) | The small browser thumbnail images, baked into the program. Also generated. |
| **`littlefs.c`** (+ `.h`) | A tiny filesystem on the flash chip, used as scratch space for the downloaded cover before it's decoded. |
| **`audio.c`** (+ `audio.h`) | Synthesises the little UI sound effects (clicks, chimes) and plays them through the board's speaker. |
| **`lv_font_*.c`** | **Generated** font files (the pixel/dot/mono/slab fonts the themes use). You won't read these; they're produced by a script. |
| **`secrets.h`** | Your WiFi and Spotify credentials. **Not in git** — you copy `secrets.h.example` and fill it in. Never commit it. |

---

## 6. The big-picture map (one diagram)

```
   [you tap / turn]                         [the internet]
         |                                        |
         v                                        v
   ui.c (LVGL task) --ui_request_*()-->  s_cmd_queue  --->  spotify_task (main.c)
         ^                                                     |  |
         |  ui_set_track_info() / ui_art_refresh()             |  |  HTTPS
         |  (taken under the display lock)                     |  v
         +-----------------------------------------------------+  spotify.c  --> Spotify
                                                               |
                                                               +> sonos.c   --> Sonos (LAN)
```

The UI and the network never call each other directly — they pass messages
through the queue one way, and the network updates the screen only through the
locked `ui_*()` helpers.

---

## 7. A few rules you must not break

These are "looks fine, compiles fine, then misbehaves later" traps. The full
list with reasons is in `CLAUDE.md`; the beginner-relevant ones:

- **Never touch LVGL objects from outside the LVGL task without the lock.** Use a
  `ui_*()` helper, which locks for you. Breaking this causes rare, impossible-to-
  reproduce screen glitches or crashes.
- **`spotify_task` must stay the only writer of the "what's playing" state**
  (`s_track`, the album-art buffers, the Sonos state). Other code requests changes
  by posting to the queue — it doesn't reach in and edit that state.
- **Cover Flow has special drawing constraints** (see the big comments in `ui.c`):
  certain LVGL transforms cause the rotated display to mis-paint, so the effect is
  hand-drawn instead. Don't "simplify" it back to the obvious approach.
- **Album lists and fonts are generated** — edit the source text / run the script,
  don't hand-edit `albums.c` or the `lv_font_*.c` files.

---

## 8. Where to read next

- **`CLAUDE.md`** (repo root) — the authoritative architecture + decisions doc,
  and the full list of constraints. The "source of truth" across the project.
- **`README.md`** in `waveshare/esp-idf/` — how to build and flash, and the
  hardware bring-up checkpoints.
- **`docs/P4-TODO.md`** — what's planned / still to do.
- **`docs/PORT-NOTES.md`** — the tricky chip/toolchain gotchas discovered along
  the way.
- **`docs/ROADMAP.md`** / **`docs/PENDING.md`** — the longer-term plan and the
  "done but not yet hardware-checked" list.

---

## 9. The other build folders

The project targets a few hardware variants, each in its own folder, sharing the
same ideas:

- **`waveshare/esp-idf/`** — this build (ESP32-P4, direct Spotify). The simplest
  to learn from, so this tour uses it.
- **`waveshare/esp-idf-ha/`** — the same board and UI, but controlling music
  through **Home Assistant** + Music Assistant instead of Spotify directly. This
  is the everyday build (it can also play music on the device's own speaker). It
  shares all its UI/audio/album code with `waveshare/esp-idf/` via the common
  `waveshare/components/p4_shared/` folder — only the backend (`ha_client.c`
  instead of `spotify.c`) differs.
- **`cyd/esp-idf/`** — an earlier, smaller board ("CYD") with physical knobs and
  buttons, also direct Spotify. Its UI/input code is *shared* via a common folder
  (`cyd/components/cyd_shared/`).
- **`cyd/esp-idf-ha/`** — the CYD build but controlling music through **Home
  Assistant** (a home-automation hub) instead of Spotify directly. Future work.
- **`cyd/platformio/`** — the original CYD build using the Arduino toolchain.

If you're learning the project, stay in `waveshare/esp-idf/` — everything in this
tour lives there.
