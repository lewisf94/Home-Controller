# cyd/platformio — Arduino/PlatformIO build (Phase 1, frozen)

The original PlatformIO + Arduino-framework build of the Music Controller. Reached feature parity at the end of Phase 1.5 and is now in maintenance mode.

**Status:** frozen — bug fixes only. No new features. Active development has moved to [`../esp-idf/`](../esp-idf/).

## Build

Open this directory in VS Code (PlatformIO extension), or from the command line:

```bash
cd cyd/platformio
pio run                      # build
pio run -t upload            # build + flash
pio device monitor -b 115200 # serial monitor
```

## Project memory

See [`../../CLAUDE.md`](../../CLAUDE.md) at the repo root for full hardware details, pin mapping, architecture notes, and coding conventions.
