#!/usr/bin/env python3
"""Resolve album details from Spotify links so you only paste a URL.

Edit spotify-albums-list.txt and drop in bare Spotify album links -- either
a share URL (https://open.spotify.com/album/<id>?si=...) or a URI
(spotify:album:<id>) -- one per line. Then:

    python scripts/add_albums.py

For every line that has an album id but no title/artist, this looks the album
up on the Spotify Web API, fills in the title + primary artist, and rewrites
the line in the canonical  "Title", "Artist", "spotify:album:<id>",  form. It
also normalises casing on every album line (lowercasing stray articles like
"The"/"A" that Spotify title-cases). Blank lines and notes are left untouched.
Newly resolved albums also get their cover downloaded to
scripts/input_albums/<id>.jpg (so embed_albums_idf.py finds it). Finally it
regenerates the album source files.

Note: if you want an exact custom casing, set it in the master and use
gen_albums.py to regenerate -- add_albums.py would re-normalise it.

Credentials use the Client-Credentials flow (no user login needed for album
metadata). The script asks for your Client ID / Secret if they aren't already
set -- nothing is written to disk. To skip the prompt, set them first:

    PowerShell:  $env:SPOTIPY_CLIENT_ID="..."; $env:SPOTIPY_CLIENT_SECRET="..."
    bash:        export SPOTIPY_CLIENT_ID=...   SPOTIPY_CLIENT_SECRET=...

(Use the same Client ID/Secret as secrets.h.)

Flags:  --no-covers   skip cover-art download
         --no-generate skip regenerating albums.c (just enrich the master)
"""
from __future__ import annotations

import re
import sys
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen_albums  # noqa: E402

MASTER = gen_albums.MASTER
INPUT_ALBUMS = Path(__file__).resolve().parent / "input_albums"

_ID = re.compile(r"(?:spotify:album:|open\.spotify\.com/album/)([A-Za-z0-9]{22})")
_QUOTED = gen_albums._QUOTED


def _spotify():
    try:
        import spotipy
        from spotipy.oauth2 import SpotifyClientCredentials
    except ImportError:
        sys.exit("Install spotipy first:  pip install spotipy")

    import os
    from getpass import getpass

    client_id = os.environ.get("SPOTIPY_CLIENT_ID")
    client_secret = os.environ.get("SPOTIPY_CLIENT_SECRET")
    if not client_id or not client_secret:
        print("Spotify credentials not found in the environment.")
        print("Enter the same Client ID / Secret you put in secrets.h "
              "(input is not echoed for the secret):")
        client_id = (client_id or input("  Client ID: ")).strip()
        client_secret = (client_secret or getpass("  Client Secret: ")).strip()

    try:
        sp = spotipy.Spotify(auth_manager=SpotifyClientCredentials(
            client_id=client_id, client_secret=client_secret))
        sp.album("4RrsgnUbZIFTw42Apa8lXO")  # cheap call to surface bad creds now
        return sp
    except Exception as e:
        sys.exit(f"Could not authenticate to Spotify (check the Client ID/Secret).\n  ({e})")


def _is_complete(line: str) -> bool:
    """A line that already has Title, Artist and a spotify URI -- leave alone."""
    fields = _QUOTED.findall(line)
    return len(fields) >= 3 and any(f.startswith("spotify:album:") for f in fields)


def _fmt(title: str, artist: str, album_id: str) -> str:
    t = title.replace('"', "'")
    a = artist.replace('"', "'")
    return f'"{t}", "{a}", "spotify:album:{album_id}",'


# Spotify title-cases names ("Cage The Elephant", "To Pimp A Butterfly"); lower
# these "minor words" when they aren't the first word so styling matches the
# usual convention. Only ever lowercases -- never recapitalises -- so an
# intentionally lowercase title (e.g. "hugo") is left untouched.
_MINOR = {"a", "an", "and", "as", "at", "but", "by", "for", "from", "in",
          "into", "nor", "of", "on", "onto", "or", "over", "the", "to",
          "via", "vs", "with"}


def _tidy(name: str) -> str:
    words = name.split(" ")
    return " ".join(w.lower() if i and w.lower() in _MINOR else w
                    for i, w in enumerate(words))


def _parse_one(line: str):
    fields = _QUOTED.findall(line)
    uri = next(f for f in fields if f.startswith("spotify:album:"))
    title, artist = [f for f in fields if f != uri][:2]
    return title, artist, uri


def _download_cover(album_id: str, images: list) -> None:
    dest_exists = any((INPUT_ALBUMS / f"{album_id}{e}").exists()
                      for e in (".jpg", ".jpeg", ".png", ".webp"))
    if dest_exists:
        return
    if not images:
        print(f"    no cover available for {album_id}")
        return
    INPUT_ALBUMS.mkdir(parents=True, exist_ok=True)
    url = images[0]["url"]  # largest first
    req = urllib.request.Request(url, headers={"User-Agent": "music-controller"})
    with urllib.request.urlopen(req) as r:
        (INPUT_ALBUMS / f"{album_id}.jpg").write_bytes(r.read())
    print(f"    cover -> input_albums/{album_id}.jpg")


def main() -> None:
    do_covers = "--no-covers" not in sys.argv
    do_generate = "--no-generate" not in sys.argv

    if not MASTER.exists():
        sys.exit(f"Missing master album list: {MASTER}")

    lines = MASTER.read_text(encoding="utf-8").splitlines()
    sp = None      # created lazily -- only if a bare link actually needs the API
    cache = {}     # album_id -> album object
    resolved = normalized = 0

    for i, line in enumerate(lines):
        m = _ID.search(line)
        if not m:
            continue  # blank line or note -- leave untouched
        album_id = m.group(1)

        if _is_complete(line):
            title, artist, _ = _parse_one(line)  # already have details, just retidy
        else:
            if sp is None:
                sp = _spotify()
            # One album per request: the batch /albums?ids= endpoint is 403'd for
            # Client-Credentials apps; single lookups work.
            try:
                alb = cache.get(album_id) or sp.album(album_id)
                cache[album_id] = alb
            except Exception as e:
                print(f"  ! could not resolve {album_id} (line {i + 1}): {e}")
                continue
            title, artist = alb["name"], alb["artists"][0]["name"]
            resolved += 1
            print(f"  resolved {album_id}: {artist} -- {title}")
            if do_covers:
                _download_cover(album_id, alb.get("images") or [])

        new = _fmt(_tidy(title), _tidy(artist), album_id)
        if new != line.strip():
            normalized += 1
        lines[i] = new

    MASTER.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"\nUpdated {MASTER.name} ({resolved} resolved, {normalized} normalized).")

    if do_generate:
        print()
        gen_albums.main()
        print("\nNote: run  python scripts/embed_albums_idf.py  to rebake "
              "album_thumbs.bin from the new covers.")


if __name__ == "__main__":
    main()
