---
name: sync-private
description: Sync the working repository to the private build folder (home-controller - Private), excluding include/ (a hard rule), then hash-verify every copied file. Use before a build or a flash, and after any code edit in the working repository.
---

# Sync the working repository to the private folder

The working repository (`c:\Users\User\Documents\home-controller`) carries
the GitHub remote. Make every edit and every commit there. The private
folder (`C:\Users\User\Documents\home-controller - Private`) exists only
for building and flashing; this folder carries the real `secrets.h` files.

## Hard rule

Never read, list, glob, copy, or otherwise touch any file under an
`include/` directory, in either folder. Each `include/` directory holds
credentials: the WiFi password, the Spotify client secret, and the Home
Assistant token, all inside `secrets.h`. This rule has no exception, not
even a check for whether the file exists. Remove every `include/` path from
a file list, before any further action on that list.

## Steps

1. List the files to sync, from the working repository:
   ```bash
   cd "/c/Users/User/Documents/home-controller" && git ls-files | grep -v -E '(^|/)include/'
   ```
   Sync only tracked files. Do not sync a gitignored file. In particular,
   `album_thumbs.bin` and `spotify-albums-list.txt` exist, in their correct
   current form, only in the PRIVATE folder; the working-repo copies are out
   of date by design. A sync from the working repository to the private
   folder would overwrite the real 56-album list with this out-of-date
   copy.

2. For a scoped sync, the normal case, where only a handful of files
   changed, skip the full file list. Sync only the changed files, and apply
   the `include/` filter first, in every case.

3. Copy each file from the working repository to the private folder,
   keeping the same relative path.

4. Verify each copy: compute an MD5 hash of each copied file, in both
   folders, and compare the two hash values. Report `OK` or `DIFF` for each
   file. Every file must report `OK` before a build starts.

5. Report the count of files synced, and confirm that no `include/` folder
   was touched.

## Rules that never change

- Never sync a gitignored file from the private folder back into the
  working repository, where git would then track it. Never commit or push
  from the private folder.
- Never copy an `include/` directory, in either direction. See the hard
  rule above.
