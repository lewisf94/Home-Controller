---
name: add-album
description: Add album(s) to the browser from Spotify share links - runs the full pipeline (resolve metadata, download cover, regenerate albums.c, rebake thumbs) and syncs the generated sources back to the working repo.
---

# Add album(s) to the browser

Input: one or more Spotify album share links / URIs from Lewis.

## Where things live (do not mix up)

- **Source of truth**: `spotify-albums-list.txt` at the repo root — but the
  REAL, current 56-album list is the copy in the PRIVATE folder
  (`C:\Users\User\Documents\home-controller - Private`). The working-repo copy
  is stale/absent by design (gitignored, personal choices). Always run the
  pipeline in the private folder.
- `album_thumbs.bin` (gitignored, copyright art) also lives only in private,
  at `waveshare\components\p4_shared\album_thumbs.bin`.
- The generated `albums.c` files ARE committed — they must flow back to the
  working repo afterwards.
- Never hand-edit `albums.c`/`albums.cpp` — always regenerate.

## Steps

1. Append the pasted link(s) to `spotify-albums-list.txt` in the PRIVATE
   folder (one per line; the script tolerates full share URLs).

2. From the private repo root run:
   ```
   python scripts/add_albums.py
   ```
   This resolves title + artist via the Spotify API (Client-Credentials;
   creds from `SPOTIPY_CLIENT_ID` / `SPOTIPY_CLIENT_SECRET` env vars or an
   interactive prompt), downloads covers to `scripts/input_albums/<id>.jpg`,
   regenerates all four `albums.c`/`albums.cpp` targets, and rebakes
   `album_thumbs.bin`. Flags `--no-covers` / `--no-generate` / `--no-embed`
   skip stages if needed.

3. Sanity-check alignment: the album count in
   `waveshare/components/p4_shared/albums.c` must equal the thumb count in
   `album_thumbs.bin` (bin size / 28800 bytes per 120x120 RGB565 thumb). The
   p4_shared CMake guard fails the build on divergence — but check here first.

4. Copy the regenerated committed sources private -> working repo:
   - `cyd/esp-idf/main/albums.c`
   - `cyd/esp-idf-ha/main/albums.c`
   - `waveshare/components/p4_shared/albums.c`
   - `cyd/platformio/src/albums.cpp`
   Do NOT copy `album_thumbs.bin` or `spotify-albums-list.txt` to the working
   repo, and never anything under `include/`.

5. Rebuild the target Lewis is using (see /build-p4) so the new albums land
   on device.
