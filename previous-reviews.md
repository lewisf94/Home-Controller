# Daily deep-dive review log

Each entry records one day's focused review so future runs cover fresh ground
instead of repeating. Newest entries at the bottom.

> **Maintained on `main`.** Earlier daily runs each branched off `main`, wrote
> this log to their own branch (`claude/dazzling-bell-odxxe`,
> `claude/pensive-davinci-x3aUp`, ...), and so never saw prior entries — which
> is why 2026-05-22 and 2026-05-23 *both* independently re-reviewed Security.
> Those entries are consolidated here. Future runs: read this file on `main`,
> and **rotate to a new area** rather than repeating one already covered.

**Coverage so far:** Security ×2 (05-22, 05-23). Its actionable findings have
since been fixed on `main` (TLS verification on both builds, log redaction,
`.cache` gitignored, setup docs + `get_spotify_token.py` point at the gitignored
`secrets.h`, IDF token-refresh `snprintf` guard). **Next runs should cover
Performance or Code quality**, not Security.

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
