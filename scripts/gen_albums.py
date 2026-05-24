#!/usr/bin/env python3
"""Single source of truth for the album list.

Reads spotify-albums-list.txt (repo root) and regenerates every album
source file across all four builds, sorted by artist then album title
(leading "The"/"A"/"An" ignored, like a music library).

    python scripts/gen_albums.py

Master file format -- one album per line, three quoted fields in any
spacing, trailing comma optional:

    "Album Title", "Artist", "spotify:album:<id>",

Anything that isn't a line with three quoted fields whose last field
starts with "spotify:album:" is ignored (blank lines, notes, etc.).

The master file is gitignored (personal album choices); the generated
.c/.cpp files are committed. embed_albums_idf.py imports parse_master()
and sort_albums() from here so the browser-thumbnail blob stays in the
exact same order as albums.c -- never edit the album arrays by hand.
"""
from __future__ import annotations

import re
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
MASTER = REPO_ROOT / "spotify-albums-list.txt"

# Each target: (path, language). language picks NULL vs nullptr.
TARGETS = [
    (REPO_ROOT / "cyd" / "esp-idf" / "main" / "albums.c", "c"),
    (REPO_ROOT / "cyd" / "esp-idf-ha" / "main" / "albums.c", "c"),
    (REPO_ROOT / "waveshare" / "esp-idf" / "main" / "albums.c", "c"),
    (REPO_ROOT / "cyd" / "platformio" / "src" / "albums.cpp", "cpp"),
]

_QUOTED = re.compile(r'"([^"]*)"')
_ARTICLES = ("the ", "a ", "an ")


def parse_master(path: Path = MASTER) -> list[tuple[str, str, str]]:
    """Return [(title, artist, uri), ...] in file order."""
    albums: list[tuple[str, str, str]] = []
    for raw in path.read_text(encoding="utf-8").splitlines():
        fields = _QUOTED.findall(raw)
        if len(fields) < 3:
            continue
        uri = next((f for f in fields if f.startswith("spotify:album:")), None)
        if uri is None:
            continue
        title, artist = [f for f in fields if f != uri][:2]
        albums.append((title.strip(), artist.strip(), uri.strip()))
    return albums


def _sort_key(s: str) -> str:
    s = s.strip().lower()
    for art in _ARTICLES:
        if s.startswith(art):
            return s[len(art):]
    return s


def sort_albums(albums: list[tuple[str, str, str]]) -> list[tuple[str, str, str]]:
    """Sort by artist then title, ignoring a leading article in both."""
    return sorted(albums, key=lambda a: (_sort_key(a[1]), _sort_key(a[0])))


def _cstr(s: str) -> str:
    return s.replace("\\", "\\\\").replace('"', '\\"')


def _render(albums: list[tuple[str, str, str]], lang: str) -> str:
    nullkw = "nullptr" if lang == "cpp" else "NULL"
    title_cells = [f'"{_cstr(t)}",' for t, _, _ in albums]
    artist_cells = [f'"{_cstr(a)}",' for _, a, _ in albums]
    tw = max((len(c) for c in title_cells), default=0)
    aw = max((len(c) for c in artist_cells), default=0)

    rows = "\n".join(
        f'    {{ {tc:<{tw}} {ac:<{aw}} "{_cstr(uri)}" }},'
        for (t, a, uri), tc, ac in zip(albums, title_cells, artist_cells)
    )

    return f"""/*
 * GENERATED FILE -- do not edit by hand.
 *
 * Source of truth: spotify-albums-list.txt (repo root, gitignored).
 * Regenerate with:  python scripts/gen_albums.py
 *
 * Sorted by artist then title (leading "The"/"A"/"An" ignored).
 */

#include "albums.h"

static const album_entry_t s_albums[] = {{
{rows}
}};

const album_entry_t *albums_get(size_t index)
{{
    if (index >= sizeof(s_albums) / sizeof(s_albums[0])) return {nullkw};
    return &s_albums[index];
}}

size_t albums_count(void)
{{
    return sizeof(s_albums) / sizeof(s_albums[0]);
}}
"""


def main() -> None:
    if not MASTER.exists():
        raise SystemExit(f"Missing master album list: {MASTER}")

    albums = sort_albums(parse_master())
    if not albums:
        raise SystemExit(f"No album lines parsed from {MASTER}")

    for path, lang in TARGETS:
        path.write_text(_render(albums, lang), encoding="utf-8", newline="\n")
        print(f"  wrote {path.relative_to(REPO_ROOT)}  ({lang})")

    print(f"\n{len(albums)} albums, sorted by artist then title:")
    for i, (title, artist, _) in enumerate(albums):
        print(f"  [{i:2d}] {artist:20s} -- {title}")


if __name__ == "__main__":
    main()
