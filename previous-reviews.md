# Daily deep-dive review log

Each entry records one day's focused review so future runs cover fresh ground
instead of repeating. Newest entries at the bottom.

> **Maintained on `main`.** Earlier daily runs each branched off `main`, wrote
> this log to their own branch (`claude/dazzling-bell-odxxe`,
> `claude/pensive-davinci-x3aUp`, ...), and so never saw prior entries — which
> is why 2026-05-22 and 2026-05-23 *both* independently re-reviewed Security.
> Those entries are consolidated here. Future runs: read this file on `main`,
> and **rotate to a new area** rather than repeating one already covered.

**Coverage so far:** Security ×2 (05-22, 05-23), Performance ×1 (05-24). The
Security actionable findings have since been fixed on `main` (TLS verification on
both builds, log redaction, `.cache` gitignored, setup docs +
`get_spotify_token.py` point at the gitignored `secrets.h`, IDF token-refresh
`snprintf` guard). **Next runs should cover Code quality, Architecture, Missing
features, or Testing/reliability** — not Security or Performance.

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
