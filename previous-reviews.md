# Daily deep-dive review log

Each entry records one day's focused review so future runs cover fresh ground
instead of repeating. Newest entries at the bottom.

> **Maintained on `main`.** Earlier daily runs each branched off `main`, wrote
> this log to their own branch (`claude/dazzling-bell-odxxe`,
> `claude/pensive-davinci-x3aUp`, ...), and so never saw prior entries — which
> is why 2026-05-22 and 2026-05-23 *both* independently re-reviewed Security.
> Those entries are consolidated here. Future runs: read this file on `main`,
> and **rotate to a new area** rather than repeating one already covered.

**Coverage so far:** Security ×2 (05-22, 05-23), Performance ×1 (05-24),
Code quality ×1 (05-26), Testing/reliability ×1 (05-28). The Security actionable
findings have since been fixed on `main` (TLS verification on both builds, log
redaction, `.cache` gitignored, setup docs + `get_spotify_token.py` point at the
gitignored `secrets.h`, IDF token-refresh `snprintf` guard). Most Performance
findings from 05-24 are still open (only the poll keep-alive landed; the rest
were lower-priority/deferred). All seven 05-26 Code-quality findings are still
open as of 05-28. **Next runs should cover Architecture or Missing features**
— not Security, Performance, Code quality, or Testing/reliability.

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
