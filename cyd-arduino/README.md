# cyd-arduino — Arduino/PlatformIO build (Phase 1, frozen)

This is the original PlatformIO + Arduino-framework build of the Music Controller.
It reached feature parity at the end of Phase 1.5 and is now in maintenance mode.

**Status:** frozen — bug fixes only. No new features. Active development has moved to `../cyd-idf/`.

## Build

Open this directory in VS Code (PlatformIO extension), or from the command line:

```bash
cd cyd-arduino
pio run                      # build
pio run -t upload            # build + flash
pio device monitor -b 115200 # serial monitor
```

## Project memory

See `../CLAUDE.md` at the repo root for full hardware details, pin mapping, architecture notes, and coding conventions.
