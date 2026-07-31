---
name: add-album
description: Add one or more albums to the browser from Spotify share links. Runs the full pipeline (resolve metadata, download the cover, regenerate albums.c, rebuild the thumbnails), then syncs the generated sources back to the working repository.
---

# Add album(s) to the browser

Input: one or more Spotify album share links or URIs from Lewis.

## Where each file lives (do not confuse these)

- Source of truth: `spotify-albums-list.txt` at the repository root. The
  real, current 56-album list is the copy in the PRIVATE folder
  (`C:\Users\User\Documents\home-controller - Private`). The working-repo
  copy is absent or out of date by design; it is gitignored, and it holds
  personal choices. Always run the pipeline in the private folder.
- `album_thumbs.bin` is gitignored, since it holds copyrighted art. This
  file lives only in the private folder, at
  `waveshare\components\p4_shared\album_thumbs.bin`.
- The generated `albums.c` files are committed. Copy these files back to
  the working repository after each run.
- Never edit `albums.c` or `albums.cpp` by hand. Always regenerate these
  files with the script.

## Steps

1. Append each pasted link to `spotify-albums-list.txt`, in the PRIVATE
   folder, one link per line. The script accepts a full share URL.

2. From the private repository root, run:
   ```
   python scripts/add_albums.py
   ```
   This script resolves the title and the artist through the Spotify API,
   using Client Credentials. This step reads the `SPOTIPY_CLIENT_ID` and
   `SPOTIPY_CLIENT_SECRET` environment variables, or reads the same values
   from an interactive prompt. The script downloads each cover to
   `scripts/input_albums/<id>.jpg`, regenerates all four `albums.c` and
   `albums.cpp` targets, and rebuilds `album_thumbs.bin`. Pass
   `--no-covers`, `--no-generate`, or `--no-embed` to skip a stage.

3. Check the alignment: the album count in
   `waveshare/components/p4_shared/albums.c` must equal the thumbnail count
   in `album_thumbs.bin` (the file size in bytes, divided by 28800 bytes
   per 120x120 RGB565 thumbnail). The `p4_shared` CMake guard fails the
   build on a mismatch, but check this alignment here first.

4. Copy the regenerated, committed sources from the private folder to the
   working repository:
   - `cyd/esp-idf/main/albums.c`
   - `cyd/esp-idf-ha/main/albums.c`
   - `waveshare/components/p4_shared/albums.c`
   - `cyd/platformio/src/albums.cpp`
   Do not copy `album_thumbs.bin` or `spotify-albums-list.txt` to the
   working repository. Do not copy any file under an `include/` directory.

5. Rebuild the target Lewis is using (see the `/build-p4` skill), so the new
   albums reach the device.
