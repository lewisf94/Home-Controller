# Daily deep-dive review log

Each entry records one day's focused review so future runs cover fresh ground
instead of repeating. Newest entries at the bottom.

> **Maintained on `main`.** Earlier daily runs each branched off `main`, wrote
> this log to their own branch (`claude/dazzling-bell-odxxe`,
> `claude/pensive-davinci-x3aUp`, ...), and so never saw prior entries — which
> is why 2026-05-22 and 2026-05-23 *both* independently re-reviewed Security.
> Those entries are consolidated here. Future runs: read this file on `main`,
> and **rotate to a new area** rather than repeating one already covered.

**Coverage so far:** Security ×2 (05-22, 05-23), Performance ×2 (05-24, 05-31),
Code quality ×1 (05-26), Testing/reliability ×1 (05-28), Architecture ×1 (05-29),
Missing features ×1 (05-30), Implementation fixes ×1 (06-01).
Security findings fixed. Most 05-24 Performance items addressed (poll keep-alive,
cmd keep-alive on CYD). 05-26 Code-quality and 05-28 Testing/reliability findings
largely addressed through 05-31. **Next run should cover Code quality** — oldest
unrefreshed area (last visited 05-26). After that restart rotation.

---

2026-05-22 - Area covered: Security. Key findings: The PlatformIO build disabled
TLS certificate checking everywhere (`setInsecure()`), so a network attacker
could read the Spotify client secret, refresh token, and access token in transit;
the embedded CA string meant to prevent this was a non-functional placeholder and
unused. Setup docs told users to paste live credentials into `src/main.cpp` (NOT
gitignored) and called it "completely safe", while the code actually reads from a
gitignored `secrets.h` - following the docs would commit secrets. Token/error
response bodies were logged to serial in both builds (could leak the access
token), `get_spotify_token.py` left a `.cache` token file in the repo root that
was not gitignored, and firmware secrets are plaintext in flash. Suggestions
made: 6. (Status: TLS, docs, log redaction, and `.cache` gitignore now fixed on
`main`; flash encryption deferred by choice.)

2026-05-23 - Area covered: Security. Key findings: `get_spotify_token.py` told
users to paste their refresh token into the tracked file `src/main.cpp` (a real
credential-leak path, now fixed to point at the gitignored `secrets.h`); an
unchecked `snprintf` return in the IDF token-refresh could send out-of-bounds
memory over the network for an over-long refresh token (now guarded). Confirmed
TLS is verified against the CA bundle on both builds, logs redact tokens, and no
real `secrets.h` was ever committed. Remaining recommendations: ESP32 flash + NVS
encryption, album-art URL `https://` scheme validation, JSON-body URI validation
(only matters if album metadata becomes externally sourced again). Suggestions
made: 5 (2 fixed, 3 left as recommendations).

2026-05-24 - Area covered: Performance. Key findings: Both builds open a brand-new
TLS connection (full handshake, ~0.5-2 s, ~30-40 KB heap) for *every* Spotify API
call -- the player poll and each button press -- instead of reusing a kept-alive
connection or a resumed TLS session, which dominates CPU/heap/latency and adds a
visible delay before transport buttons act. The Arduino build also parses the
whole `/me/player` response into an ArduinoJson heap tree when only ~10 fields are
used (a `Filter` would slash heap + parse time; the IDF streaming scanner already
avoids this), and album art is written to internal flash then re-read to decode
(slow + flash wear) when a RAM decode path already exists but is dead code (its
comment also wrongly says the cap is 16 KB; it is 256 KB). Lower-priority: fixed
poll cadence never backs off when paused/idle; the IDF JSON scanner re-scans from
the top per field; `find_centered_card()` is O(n) per scroll event; LVGL progress
+ WiFi timers run on a fixed cadence regardless of which screen is visible.
Verified prior run's two fixes are in place (`get_spotify_token.py` -> `secrets.h`,
IDF refresh `snprintf` guard). Suggestions made: 7.

2026-05-26 - Area covered: Code quality. Key findings: Three of the four build
folders are near-identical copies maintained by hand -- `cyd/esp-idf` and
`cyd/esp-idf-ha` share six byte-for-byte-identical files (ui.c, input.c,
mcp_input.c, album_art.cpp, albums.c, littlefs.c) and `waveshare` differs from
the lead build by only ~13-15 lines in two files, so every bug fix must be
copy-pasted N times (CLAUDE.md even says so); a shared ESP-IDF component would
remove the drift risk. The Arduino `download_album_art()` has ~24 lines of
unreachable code after an unconditional `return`, and `ui_fancy_backup.cpp` is
1004 lines of dead file -- both violate the repo's own "delete dead code
cleanly" rule. The lead IDF build assumes the speaker volume starts at 50 and
never reads the real device volume (the Arduino build does), so the volume HUD
and first nudge can be wrong; the IDF command path also silently swallows a 401
(expired token) that the poll path handles. Plus a stale comment claiming the
HTTP buffer caps at "16 KB" when `RESP_MAX_CAP` is 256 KB (carried over,
unfixed), and silent album truncation at `MAX_CARDS` (32 on CYD, 64 on
waveshare) with no warning log. Verified prior Security fixes still hold; most
05-24 Performance items remain open by design. Suggestions made: 7.

2026-05-28 - Area covered: Testing & reliability. Key findings: All three
ESP-IDF builds give up on WiFi permanently after a short burst of back-to-back
retries (set WIFI_FAIL_BIT and never call esp_wifi_connect again), so an
ordinary router reboot strands the always-on device offline until a manual
power-cycle -- the clearest single point of failure in the tree. The lead IDF
build also regressed several behaviours the Arduino build already had: it can't
wake an idle Spotify device on a 404 (so "Play" silently does nothing after the
phone idles), never reads the real device volume (assumes 50), swallows 401s in
the playback-command path (the poll path handles them), drops all command
failures with no retry or UI feedback, and leaves the physical controls
permanently dead with no re-probe or on-screen hint if the MCP23017 isn't found
at boot. Lower priority: silent album truncation at MAX_CARDS and an unchecked
esp_littlefs_info return. Verified all seven 05-26 Code-quality findings are
still open (dead ui_fancy_backup.cpp, Arduino dead code after return, stale
"16 KB" comment, shared-component duplication, plus the volume-50/401/truncation
items that overlap with today). Suggestions made: 8.

2026-05-29 - Area covered: Architecture. Key findings: The per-board "wiring"
layer is copy-pasted, not shared -- the three ESP-IDF `main.c` files (soon four
with the planned P4-HA build) each re-implement the same ~290 lines of WiFi
state machine, SPI/LCD/touch bring-up, NVS init, the `scmd_t` command-queue type,
`_post_cmd`, and the six `ui_request_*()` posters; the two CYD `main.c` files
differ by only ~50 non-comment lines (the backend task body) yet share zero code,
so the queue contract and bring-up must be hand-synced N ways. The backend
abstraction itself is sound (`ui_request_*()` + `spotify_track_t` +
`ui_set_track_info()` cleanly decouple UI from Spotify-vs-HA), but it is informal:
the HA build keeps a file literally named `spotify.h` that contains no Spotify
client at all, purely so `ui.c`'s `#include "spotify.h"` still compiles -- a
fragile naming hack where a neutral `player.h`/`track_info.h` + a documented
backend contract belongs. `input.c` also owns playback state (`s_current_vol`,
`s_is_muted`) that conceptually belongs to the player model, which is the root of
the "assumes volume 50" bug. And there is no connectivity-supervisor layer: WiFi
recovery is inlined in an event handler that has a dead-end (WIFI_FAIL_BIT, never
retries again) -- the single biggest structural point of failure, carried over
from 05-28. Verified all eight 05-28 Testing/reliability findings are still open
(WiFi give-up, 404 wake, volume-50, 401-in-command-path, no command retry/feedback,
MCP no re-probe, album truncation, unchecked esp_littlefs_info). Suggestions made: 6.

2026-05-30 - Area covered: Missing features / edge cases. Key findings: The lead
CYD ESP-IDF build is BROKEN at HEAD -- the 05-29 shared-component extraction
(commit 2f7accd) moved ui.c/input.c/mcp_input.c/album_art.cpp/littlefs.c to
cyd/components/cyd_shared/ but never updated the two main/CMakeLists.txt (still
list the moved files as SRCS) or the top-level CMakeLists.txt (no
EXTRA_COMPONENT_DIRS). `idf.py build` fails configure for both cyd/esp-idf and
cyd/esp-idf-ha. Feature regressions vs the shipped Arduino build: shuffle
toggle gone (spotify_toggle_shuffle/shuffle_state both removed), the wake-idle
-device 404->PUT /me/player rescue in play/pause gone (also covered as 05-28
finding #2), and the SW4 + RE1 "seek preview" gesture documented as "shipped"
in CLAUDE.md was never ported -- SW4 just toggles the view. Edge-case gaps:
empty album list shows a blank carousel with no message, MAX_CARDS truncation
only logs to serial, WiFi/Spotify outages keep the last track on screen
indefinitely with no "offline" cue, volume HUD falsely shows "VOL 50%" before
the first poll fills the real device volume, tapping a card with no active
Spotify device silently fails, the browser never auto-scrolls to the album of
the currently-playing track, and spotify_download_to_file accepts any
response body as JPEG (no FFD8 magic check). Status updates on prior findings:
05-28 #3 (volume-50) and #4 (401-in-command-path) ARE NOW FIXED on lead;
adaptive poll backoff (5s/15s) shipped on lead too. The other six 05-28
findings remain open. Suggestions made: 9.

2026-05-31 - Area covered: Performance. Key findings: The single biggest
carryover from 05-24 is still open -- every transport button press opens a
fresh TLS connection in `_do_cmd` on both ESP-IDF builds (CYD `spotify.c:740`,
waveshare `spotify.c:754`), even though the matching poll path has used a
persistent keep-alive client since `db5501b`; that's ~0.5-1.5 s of handshake
plus ~30-40 KB heap spike per press, the most user-visible perf win available.
Two new hot-path issues introduced by recent work: Sonos
`sonos_fetch_now_playing()` opens THREE separate HTTP connections per poll
cycle (GetPositionInfo + GetTransportInfo + GetVolume in `sonos.c:398-429`, all
cleanup-on-each-call), and waveshare `apply_card_transforms()`
(`ui.c:1907-1945`) rewrites all 64 cards on every scroll frame (~10k style
mutations/sec during inertia) when only ~9 around the centre actually change.
Carryovers from 05-24 still open: CYD `find_centered_card()` still does O(n)
coord-walks every scroll event (waveshare already moved to O(1) index math via
`(scroll_x + step/2) / step`), the `/me/player` poll doesn't use Spotify's
`?fields=` filter so it parses 6-15 KB when only ~10 fields are read,
`json_obj_get` rescans the parent object ~7 times per poll (a single-pass
multi-key scanner would roughly halve parse time), and the LVGL progress/wifi
timers tick on both screens even when not visible (5 wakeups/sec wasted on the
browser screen). New finding: `input_task` takes the LVGL lock every 2 ms
unconditionally (`cyd/esp-idf/main/main.c:329-342`), even when no button was
pressed and no encoder edge fired -- competes with the render task's lock
acquisition and causes bimodal frame times during Cover Flow scrolls. Stale
comment + dead code: `spotify_download_bytes` + `album_art_decode` RAM-decode
paths exist in both builds but are unused; waveshare with 32 MB PSRAM is going
through the LittleFS write/read round-trip on every track change for no reason
(the deferred-work list flags this). Status updates: 05-30 findings are almost
entirely fixed on main (empty list message, MAX_CARDS on-screen warning,
OFFLINE cue, vol-HUD pre-poll gate, "No active device" toast, auto-snap
browser, JPEG SOI check, waveshare auto-dim); shuffle toggle is still missing
on the IDF builds, and the SW4 + RE1 seek-preview gesture is still claimed in
CLAUDE.md but never ported to the IDF input dispatcher. 05-28 findings mostly
addressed (WiFi background reconnect timer, 404 wake-idle, volume-50 sentinel,
401-clear in `_do_cmd`, MCP re-probe every 5 s, on-screen truncation);
`esp_littlefs_info` unchecked-return still open. 05-29 architecture: the
shared `cyd_shared` component and `player.h` rename both landed; shared
`main.c` glue across builds and a proper connectivity supervisor remain open.
Suggestions made: 9.

2026-06-01 - Area covered: Implementation of open review findings. Implemented
three fixes from the 05-31 performance review backlog. (1) Waveshare `_do_cmd`
persistent TLS client: `waveshare/esp-idf/main/spotify.c` was still opening a
fresh TLS connection (full handshake ~0.5-2 s, ~30-40 KB heap spike) for every
playback command, while the CYD build already had `s_cmd_client` with
`keep_alive_enable`. Ported the same `s_cmd_client` + `cmd_client_close()`
pattern to waveshare -- handle reused across calls, dropped on transport error
or 401. (2) `input_task` unconditional LVGL lock: both CYD IDF builds called
`lvgl_port_lock(10)` every 2 ms regardless of pending input, competing with
the render task 500 times/second. Added `mcp_input_has_pending()` to
`cyd/components/cyd_shared/mcp_input.c` (non-consuming check of all event
latches and encoder delta) and `input_needs_tick()` to `input.c` (true when
vol debounce or SW4 long-press is active); both `main.c` files now gate the
lock attempt behind these checks and use timeout=0 so a busy render task is
never blocked. (3) Promoted `s_vol_pending` to file scope in `input.c` so
`input_needs_tick()` can read it. Verified: `esp_littlefs_info` return is
already checked (logged as warning -- not open). Still open from prior reviews:
shared `main.c` WiFi/bring-up glue across builds; connectivity supervisor;
Sonos three-connection-per-poll; `apply_card_transforms()` all-64-cards rewrite
per scroll frame; CYD `find_centered_card()` O(n); `?fields=` poll filter;
`json_obj_get` multi-key scan; LVGL timers running on wrong screen; waveshare
RAM art decode (PSRAM available). Suggestions actioned: 3.

2026-06-01 (follow-up) - Verification + Sonos poll fix. Audited 15 prior-review
"fixed/open" claims against current source (file:line evidence): 14 confirmed
genuinely fixed (all Security; both keep-alive clients; WiFi background
reconnect; 404 device-wake; `esp_littlefs_info` checked; MCP re-probe; shuffle
on IDF; JPEG SOI check), and the one "FALSE" was good news -- `ui_fancy_backup.cpp`
no longer exists and `download_album_art()` is a clean stub, so the 05-26
dead-code finding is resolved. Then fixed the clearest remaining open item:
Sonos `sonos_fetch_now_playing()` opened three separate HTTP connections per
poll cycle (`GetPositionInfo` + `GetTransportInfo` + `GetVolume`, each a full
init/perform/cleanup to host:1400). Added a persistent keep-alive `s_query_client`
to `soap_query()` (`waveshare/esp-idf/main/sonos.c`), reused across all three
queries and across poll cycles; `set_url`/`set_method`/`set_user_data` per call
(same host:port, only path/SOAPAction differ); dropped on transport error via
`query_client_close()`. Same pattern as the verified Spotify `s_poll_client`/
`s_cmd_client`. Still open (deferred-by-design perf + architecture): shared
`main.c` glue, connectivity supervisor, `apply_card_transforms()` per-frame
cost, `find_centered_card()` O(n), `?fields=` poll filter, `json_obj_get`
multi-key scan, off-screen LVGL timers, waveshare RAM art decode.
