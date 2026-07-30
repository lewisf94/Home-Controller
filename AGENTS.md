# Repository Instructions for Codex

Read this file before you change the repository. Read
[CLAUDE.md](CLAUDE.md) before a non-trivial change.

## Repository Locations

The normal repository is:

```text
C:\Users\lewis\Documents\home-controller
```

The private build and flash folder is:

```text
C:\Users\lewis\Documents\home-controller - Private
```

Use the normal repository as the source for all edits and Git operations. Use
the private folder only for builds that require private credentials.

## Mandatory Folder Rule

> **WARNING:** Never enter, list, read, search, copy, or change a folder named
> `include`.

This rule has no exceptions. Use explicit file paths and exclusion pathspecs.
Do not run a recursive command that can enter an `include` folder.

Do not copy an `include` folder between repositories. Do not stage an `include`
folder.

## Private Folder Rules

1. Make the source change in the normal repository.
2. Copy only the necessary non-`include` files to the private folder.
3. Build or flash from the private folder when credentials are necessary.
4. Commit from the normal repository.
5. Push from the normal repository.

Never push from the private folder.

## Worktree Safety

The worktree can contain changes from Lewis or another tool. Do not discard,
restore, or overwrite an unrelated change.

Before an edit, inspect the applicable file and its Git diff. Stage only the
intended files or hunks.

Do not use these commands without an explicit request:

- `git reset --hard`
- `git checkout -- <path>`
- `git restore <path>`
- A force push

## Technical Writing

All first-party technical prose must follow ASD-STE100 Simplified Technical
English, Issue 9.

Follow [WRITING-STANDARD.md](docs/WRITING-STANDARD.md). Apply the standard to
existing text when you edit it.

Keep exact code, commands, paths, logs, quotations, licenses, generated text,
and third-party text unchanged.

## Code Rules

- Do not add emojis to code or commit messages.
- Delete dead code. Do not add a comment that says code was removed.
- Use the existing framework and helper functions.
- Keep changes inside the applicable component boundary.
- Add an abstraction only when it removes real complexity.
- Keep interface layout constants with the existing layout constants.
- Use generated album files only as generated output.
- Do not edit `albums.c`, `albums.cpp`, or embedded thumbnail data manually.

## LVGL Rules

LVGL is not thread-safe. Access an LVGL object only from its owner task or while
the applicable LVGL lock is held.

Do not run a blocking network operation while an LVGL lock is held.

For the Waveshare ESP32-P4:

- Do not apply an object-level scale or opacity transform to an album card.
- Transform the child image when a safe image transform is necessary.
- Do not enable `LV_USE_MATRIX`.
- Do not enable runtime Tiny TTF.
- Keep `CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT=1`.

## Network Rules

Spotify and Home Assistant operations can block. Send playback commands through
the applicable command queue.

Do not call a blocking network function from an input task or render callback.

Keep persistent clients single-owner. Close a persistent client after a
transport failure.

## Input Rules

The MCP23017 polling task can access only its driver state. It must not modify
player state or an LVGL object.

Use consume-on-read event latches for button presses. A network delay must not
discard an input event.

## Build Rules

Use ESP-IDF 5.5.x for the Waveshare ESP32-P4 builds.

Use ESP-IDF 6.0 for the CYD ESP-IDF builds.

Use PlatformIO for the CYD Arduino build and the production RP2040 firmware.

Use the Pico SDK build script for `rp2040/bringup/`.

## Verification

Run the smallest test that proves the change. Increase the test scope when a
change affects shared code or a user workflow.

For a Waveshare build, run the P4 reliability check before and after the build:

```powershell
python scripts/check_p4_reliability.py both
python scripts/check_p4_reliability.py both --post-build
```

Do not state that hardware is verified unless Lewis completed the hardware
test.

## Git Policy

Use this author:

```text
Lewis <lewisf94@users.noreply.github.com>
```

Push directly to `main` for normal solo work. Create a branch only when Lewis
requests one.

Never use `--no-verify`. Never force-push without an explicit instruction.

Do not add an AI session link to a commit message.

## Common Commands

Waveshare:

```powershell
cd waveshare/esp-idf
idf.py build
```

CYD ESP-IDF:

```powershell
cd cyd/esp-idf
idf.py build
```

CYD Arduino:

```powershell
cd cyd/platformio
pio run
```

RP2040 native bring-up:

```powershell
cd rp2040/bringup
.\build.ps1
```
