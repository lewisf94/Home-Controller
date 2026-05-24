#!/usr/bin/env python3
"""Build the embedded album-thumbnail blob for the ESP-IDF builds.

The album list, order and metadata come from the single source of truth
(spotify-albums-list.txt -> gen_albums.py). This script only maps each
album to its cover image and bakes the 120x120 little-endian RGB565
thumbs, in the *same* sorted order as albums.c, into album_thumbs.bin.

    python scripts/embed_albums_idf.py

Each album is matched to a cover image in scripts/input_albums/ by its
Spotify album id (the part after "spotify:album:") -- so the preferred
filename is "<id>.jpg" / "<id>.png". For backwards compatibility the
descriptive filenames already on disk are also accepted via IMAGE_FILES
below. An album with no image gets a neutral placeholder tile so the
blob stays index-aligned with albums.c (the browser shows a coloured
letter card for it); missing covers are listed at the end.

album_thumbs.bin is gitignored (cover art is copyright); regenerate it
locally whenever you change the album list or add a cover image.
"""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_albums import parse_master, sort_albums  # noqa: E402

SIZE = 120  # matches CARD_SIZE in ui.c so each thumb fills its card exactly.
PLACEHOLDER_RGB = (32, 32, 32)  # neutral tile for albums with no cover yet.

REPO_ROOT = Path(__file__).resolve().parent.parent
SRC_DIR = REPO_ROOT / "scripts" / "input_albums"

# album_thumbs.bin is embedded by each IDF build's CMakeLists (EMBED_FILES);
# write every build's copy so they all stay aligned with albums.c.
OUT_DIRS = [
    REPO_ROOT / "cyd" / "esp-idf" / "main",
    REPO_ROOT / "cyd" / "esp-idf-ha" / "main",
    REPO_ROOT / "waveshare" / "esp-idf" / "main",
]

# Spotify album id -> cover filename in input_albums/. Optional: an id with
# no entry here is looked up as "<id>.jpg"/"<id>.png". Kept so the descriptive
# filenames already on disk keep working.
IMAGE_FILES = {
    "3cqdpAqjVjv5IB8HTruJkj": "Aha_Shake_Heartbreak-Kings_of_Leon.jpg",
    "7v6FNgLDS8KmaWA1amUtqe": "Arctic_Monkeys_–_Tranquility_Base_Hotel_&_Casino.png",
    "2VuZJsJBPLwg9BeQFQle8G": "Cage_the_Elephant_Social_Cues.jpg",
    "0aVN6rMKthfuAdDXn8RTXf": "Fontaines_D.C.-Skinty_Fia.png",
    "4tUxQkrduOE8sfgwJ5BI2F": "Gorillaz (Gorillaz)_2001_album.png",
    "7IO2RSWhcIt8Tliya8fCZL": "Grian_Chatten-Chaos_for_the_Fly.png",
    "7ycBtnsMtyVbbwTfJwRjSP": "Kendrick_Lamar-To_Pimp_a_Butterfly.png",
    "3McfY0EGNjsrVdYa9ZnoSH": "Loyle_Carner_-_Hugo.png",
    "2Htq1sHgmdGffojIBM6Q1s": "Magdalena_Bay-Imaginal_Disk.png",
    "2dIGnmEIy1WZIcZCFSj6i8": "Plasticbeach-Gorillaz.jpg",
    "2yNaksHgeMQM9Quse463b5": "The_Strokes-Is_This_It_cover.png",
    "2xkZV2Hl1Omi8rk2D7t5lN": "The_Strokes-The_New_Abnormal.png",
    "0RyCpIKlCV0kgEuzrmp73O": "What_Went_Down-Foals.jpg",
    "50Zz8CkIhATKUlQMbHO3k1": "Whatever_People_Say_I_Am,_That's_What_I'm_Not_(2006_Arctic_Monkeys_album).jpg",
}


def find_cover(album_id: str) -> Path | None:
    """Resolve an album id to a cover image path, or None if absent."""
    mapped = IMAGE_FILES.get(album_id)
    if mapped:
        p = SRC_DIR / mapped
        if p.exists():
            return p
    for ext in (".jpg", ".jpeg", ".png", ".webp"):
        p = SRC_DIR / f"{album_id}{ext}"
        if p.exists():
            return p
    return None


def encode(path: Path) -> bytes:
    try:
        from PIL import Image
    except ImportError:
        sys.exit("Install Pillow to encode covers:  pip install Pillow")
    img = Image.open(path).convert("RGB").resize((SIZE, SIZE), Image.Resampling.LANCZOS)
    return _to_rgb565(img.load(), lambda x, y, px: px[x, y])


def placeholder() -> bytes:
    r, g, b = PLACEHOLDER_RGB
    return _to_rgb565(None, lambda x, y, _: (r, g, b))


def _to_rgb565(px, get) -> bytes:
    out = bytearray(SIZE * SIZE * 2)
    i = 0
    for y in range(SIZE):
        for x in range(SIZE):
            r, g, b = get(x, y, px)
            v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            out[i] = v & 0xFF
            out[i + 1] = (v >> 8) & 0xFF
            i += 2
    return bytes(out)


def main() -> None:
    albums = sort_albums(parse_master())
    blob = bytearray()
    missing = []

    for idx, (title, artist, uri) in enumerate(albums):
        album_id = uri.rsplit(":", 1)[-1]
        cover = find_cover(album_id)
        if cover:
            blob.extend(encode(cover))
            print(f"  [{idx:2d}] {artist:20s} -- {title:34s} <- {cover.name}")
        else:
            blob.extend(placeholder())
            missing.append((artist, title, album_id))
            print(f"  [{idx:2d}] {artist:20s} -- {title:34s} <- (placeholder, no cover)")

    for out_dir in OUT_DIRS:
        if out_dir.is_dir():
            (out_dir / "album_thumbs.bin").write_bytes(blob)
            print(f"wrote {(out_dir / 'album_thumbs.bin').relative_to(REPO_ROOT)}  ({len(blob)} bytes)")

    print(f"\n{len(albums)} thumbs @ {SIZE}x{SIZE}, in albums.c order.")
    if missing:
        print(f"\n{len(missing)} album(s) need a cover in {SRC_DIR.relative_to(REPO_ROOT)}/ "
              f"(named <id>.jpg or .png):")
        for artist, title, album_id in missing:
            print(f"  - {artist} -- {title}   ->  {album_id}.jpg")


if __name__ == "__main__":
    main()
