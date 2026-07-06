---
name: sync-private
description: Sync the working repo to the private build folder (home-controller - Private), excluding include/ (hard rule), then hash-verify every copied file. Use before any build/flash, and after any code edit in the working repo.
---

# Sync working repo -> private folder

The working repo (`c:\Users\User\Documents\home-controller`, has the GitHub
remote) is where all edits and commits happen. The private folder
(`C:\Users\User\Documents\home-controller - Private`) is for building and
flashing only — it carries the real `secrets.h` files.

## HARD RULE

**Never read, list, glob, copy, or otherwise touch anything under any
`include/` directory in EITHER folder.** They hold credentials (secrets.h:
WiFi password, Spotify client secret, HA token). No exceptions, ever — not
even to "check if it exists". Exclude `include/` paths from every file
enumeration before doing anything with the list.

## Steps

1. Enumerate the sync set from the working repo:
   ```bash
   cd "/c/Users/User/Documents/home-controller" && git ls-files | grep -v -E '(^|/)include/'
   ```
   Tracked files only. Do NOT sync gitignored files — in particular
   `album_thumbs.bin` and `spotify-albums-list.txt` live canonically in the
   PRIVATE folder (the working copies are stale by design; syncing them
   working->private would destroy the real 56-album list).

2. For scoped syncs (normal case — a handful of files just edited), skip the
   full enumeration and sync just those files, still applying the include/
   filter first.

3. Copy each file working -> private, preserving relative paths.

4. Verify: md5 each copied file in both folders and compare. Report
   `OK`/`DIFF` per file; every file must be `OK` before building.

5. Report the count synced and confirm the include/ folders were untouched.

## Never

- Never sync private -> working for gitignored files into git's view, and
  never commit or push from the private folder.
- Never copy `include/` in either direction (see HARD RULE).
