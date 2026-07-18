# Host Test Coverage

The repository has a host-runnable regression suite for logic that does not
need an ESP32, RP2040, display, Spotify account, or Home Assistant instance.
It uses Python's standard `unittest` runner and small native-C harnesses with
fake ESP-IDF services.

## Run the suite

From the repository root:

```powershell
python -m unittest discover -s tests -p "test_*.py" -v
python scripts/check_p4_reliability.py both
```

Pillow is the only test dependency. Native-C cases run when `cc`, `gcc`, or
`clang` is available; otherwise those cases are reported as skipped. GitHub
Actions runs them on Ubuntu, where a compiler is preinstalled.

## Coverage map

| Area | Automated coverage | Still requires target/integration testing |
|---|---|---|
| Album metadata generation | parsing, Unicode normalization/folding, article-aware sorting, C/C++ escaping, all output modes | visual review of unusual scripts not present in compiled fonts |
| Album art tools | RGB565 byte order, placeholders, cover lookup, metadata preservation, preview decoding, LVGL font generation | visual judgement of resize/dither quality |
| Add-album pipeline | link/URI recognition, metadata normalization, API-result caching, cover download, pipeline flags | a live Spotify client-credentials request |
| P4 reliability | every checked sdkconfig/source contract, HA inventory limits, application and bootloader headroom, runtime SRAM gating | post-build binary checks and the hardware soak in `P4-RELIABILITY.md` |
| Direct Spotify + HA shared parsing | JSON escapes, raw UTF-8, Unicode and surrogate pairs, truncation/error paths; source contracts ensure both backends use it | live HTTP/WebSocket response fixtures, authentication, reconnects, service calls |
| CYD physical input | quadrature direction/glitch rejection, debounce/event latching, browser/transport mapping, volume debounce/mute, short/long SW4 presses | MCP23017 I2C timing and real encoder/button electrical behaviour |
| P4 UI-adjacent state | double-buffer publication/allocation rollback, runtime album sorting/persistence/art repair, credential override/fallback behaviour | LVGL rendering, gestures, Cover Flow geometry/performance, audio output |
| RP2040/P4 knob protocol | version, CRC and COBS implementation contracts on both endpoints | motor control, UART framing under load, strain gauge, sensors, LEDs |
| Repository contracts | generated catalogue URI order across all four builds, shared component wiring, CI presence | complete ESP-IDF/PlatformIO builds |
| WiFi and OTA | guarded indirectly by reliability/build checks | network integration on hardware |

## Test design rules

- Host tests never contain or load private credentials.
- Native harnesses fake ESP-IDF APIs but compile the real production `.c`
  implementation into the test executable.
- Hardware behaviour is not simulated when a simulation would give false
  confidence; those gaps remain explicit in the table above.
- The existing ESP-IDF workflow remains the compile/link gate for the lead P4
  firmware. The host-test workflow is complementary and runs on every push and
  pull request.
