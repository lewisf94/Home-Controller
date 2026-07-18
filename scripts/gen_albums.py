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
import sys
import unicodedata
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
MASTER = REPO_ROOT / "spotify-albums-list.txt"


def force_utf8_stdout() -> None:
    """Stop print() crashing on a non-cp1252 album title.

    Windows consoles (and the ESP-IDF build that runs this generator) default
    to cp1252, which can't encode characters like U+FFFD or many non-Latin
    glyphs -- print() then raises UnicodeEncodeError and kills the build.
    Emit UTF-8, replacing anything the stream still can't encode.
    """
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass


def console_ascii(value: str) -> str:
    """Keep build logs portable without changing generated UTF-8 metadata."""
    return value.encode("ascii", errors="backslashreplace").decode("ascii")

# Each target: (path, language, preserve_unicode). Language picks NULL vs
# nullptr. Waveshare has compiled Latin/typographic Unicode fallback fonts;
# CYD keeps the proven ASCII fold until its own fonts receive the same support.
TARGETS = [
    (REPO_ROOT / "cyd" / "esp-idf" / "main" / "albums.c", "c", False),
    (REPO_ROOT / "cyd" / "esp-idf-ha" / "main" / "albums.c", "c", False),
    (REPO_ROOT / "waveshare" / "components" / "p4_shared" / "albums.c", "c", True),
    (REPO_ROOT / "cyd" / "platformio" / "src" / "albums.cpp", "cpp", False),
]

_QUOTED = re.compile(r'"([^"]*)"')
_ARTICLES = ("the ", "a ", "an ")

# Typographic punctuation -> ASCII. Spotify metadata uses smart quotes/dashes
# (e.g. U+2019 in "Where's My Utopia?"), but the compiled device fonts
# (Montserrat / unscii mono / dot) only carry the ASCII range -- the smart
# forms render as a missing-glyph box on the panel.
_PUNCT_ASCII = str.maketrans({
    "‘": "'",  "’": "'",   # single quotes
    "“": '"',  "”": '"',   # double quotes
    "–": "-",  "—": "-",   # en/em dash
    "…": "...",                 # ellipsis
    " ": " ",                   # no-break space
})


# Letters that NFKD can't decompose to ASCII + a combining mark.
_LETTER_FOLD = str.maketrans({
    "ø": "o", "Ø": "O", "æ": "ae", "Æ": "AE", "œ": "oe", "Œ": "OE",
    "ß": "ss", "đ": "d", "Đ": "D", "ł": "l", "Ł": "L", "ð": "d", "Ð": "D",
    "þ": "th", "Þ": "Th",
})


def _ascii_fold(s: str) -> str:
    """Reduce a title/artist to ASCII the device fonts actually carry.

    The compiled Montserrat / unscii-mono / dot fonts only have the ASCII
    range, so anything outside it renders as a missing-glyph box (e.g. the
    "O-umlaut" Fcukers album showed as just a box). NFKD splits accented
    letters into base + combining mark; we drop the marks, hand-fold the few
    letters NFKD can't (o-slash, ae, ...), then strip any remaining
    non-ASCII (which would have been a box anyway).
    """
    s = unicodedata.normalize("NFKD", s)
    s = "".join(c for c in s if not unicodedata.combining(c))
    s = s.translate(_LETTER_FOLD)
    return s.encode("ascii", "ignore").decode("ascii")


def _ascii_punct(s: str) -> str:
    return _ascii_fold(s.translate(_PUNCT_ASCII))


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
        albums.append((unicodedata.normalize("NFC", title.strip()),
                       unicodedata.normalize("NFC", artist.strip()), uri.strip()))
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
    force_utf8_stdout()
    if not MASTER.exists():
        raise SystemExit(f"Missing master album list: {MASTER}")

    albums = sort_albums(parse_master(MASTER))
    if not albums:
        raise SystemExit(f"No album lines parsed from {MASTER}")

    for path, lang, preserve_unicode in TARGETS:
        display_albums = albums if preserve_unicode else [
            (_ascii_punct(title), _ascii_punct(artist), uri)
            for title, artist, uri in albums
        ]
        path.write_text(_render(display_albums, lang), encoding="utf-8", newline="\n")
        print(f"  wrote {path.relative_to(REPO_ROOT)}  ({lang})")

    print(f"\n{len(albums)} albums, sorted by artist then title:")
    for i, (title, artist, _) in enumerate(albums):
        print(f"  [{i:2d}] {console_ascii(artist):20s} -- {console_ascii(title)}")


if __name__ == "__main__":
    main()
