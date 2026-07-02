---
name: build-p4
description: Build the Waveshare ESP32-P4 firmware(s) from the private folder with the correct ESP-IDF 5.5.4 activation. Args - "spotify", "ha", or "both" (default both). Use after /sync-private.
---

# Build the Waveshare P4 targets

Builds happen in the PRIVATE folder (it has the real `secrets.h` and
`album_thumbs.bin`): `C:\Users\User\Documents\home-controller - Private`.
Run /sync-private first so private matches the working repo.

Targets:
- `spotify` -> `waveshare\esp-idf` (direct Spotify, lead build) -> `music_controller_p4.bin`
- `ha` -> `waveshare\esp-idf-ha` (Home Assistant backend) -> `music_controller_p4_ha.bin`
- `both` (default) -> build the two in sequence

## Environment facts (do not re-derive)

- Toolchain is **ESP-IDF 5.5.4** (NOT 5.4, NOT 6.0).
- Activate with:
  `. "C:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1"`
  Do NOT use `C:\esp\v5.5.4\esp-idf\export.ps1` — its Python venv path
  (`idf5.5_py3.13_env`) is missing and it fails.
- **PowerShell state does not persist between tool calls** — the activation
  and `idf.py build` MUST be chained in a single PowerShell call.
- The board enumerates as a CH343 USB-serial (typically COM3/COM4).

## Steps (per target)

One PowerShell call, e.g. for the HA build:

```powershell
. "C:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1"; Set-Location "C:\Users\User\Documents\home-controller - Private\waveshare\esp-idf-ha"; idf.py build
```

(Use a 600000 ms timeout; a clean rebuild takes several minutes.)

Then report per target: exit code, binary size, and the "x% free" partition
headroom from the build output. Both must end EXITCODE:0.

## Flashing (Lewis usually does this himself)

```powershell
. "C:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1"; Set-Location "<private build dir>"; idf.py -p COM4 flash monitor
```

Only flash when explicitly asked; confirm the COM port if unsure.
