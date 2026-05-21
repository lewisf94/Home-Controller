#!/usr/bin/env python3
"""Build the embedded album-thumbnail blob for the ESP-IDF build.

Reads source images from scripts/input_albums/, resizes each to 120x120,
encodes as little-endian RGB565, and concatenates all 14 thumbs into a
single binary file at cyd/esp-idf/main/album_thumbs.bin.

That .bin is pulled into firmware via `EMBED_FILES` in the component's
CMakeLists.txt -- album_thumbs.c then exposes per-album pointers into
the blob. No SD card, no runtime download for the browser.

The order here MUST match albums.c. Add new albums to both files at the
same time.
"""
from pathlib import Path
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("Install Pillow first:  pip install Pillow")

SIZE = 120  # matches CARD_SIZE in ui.c so each thumb fills its card exactly.

# Filename in scripts/input_albums/  ->  human-readable title (for log lines).
# Order must mirror s_albums[] in cyd/esp-idf/main/albums.c.
ALBUMS = [
    ("Aha_Shake_Heartbreak-Kings_of_Leon.jpg",                                            "Aha Shake Heartbreak"),
    ("Arctic_Monkeys_–_Tranquility_Base_Hotel_&_Casino.png",                         "Tranquility Base Hotel & Casino"),
    ("Cage_the_Elephant_Social_Cues.jpg",                                                 "Social Cues"),
    ("Fontaines_D.C.-Skinty_Fia.png",                                                     "Skinty Fia"),
    ("Gorillaz (Gorillaz)_2001_album.png",                                                "Gorillaz"),
    ("Grian_Chatten-Chaos_for_the_Fly.png",                                               "Chaos for the Fly"),
    ("Kendrick_Lamar-To_Pimp_a_Butterfly.png",                                            "To Pimp a Butterfly"),
    ("Loyle_Carner_-_Hugo.png",                                                           "Hugo"),
    ("Magdalena_Bay-Imaginal_Disk.png",                                                   "Imaginal Disk"),
    ("Plasticbeach-Gorillaz.jpg",                                                         "Plastic Beach"),
    ("The_Strokes-Is_This_It_cover.png",                                                  "Is This It"),
    ("The_Strokes-The_New_Abnormal.png",                                                  "The New Abnormal"),
    ("What_Went_Down-Foals.jpg",                                                          "What Went Down"),
    ("Whatever_People_Say_I_Am,_That's_What_I'm_Not_(2006_Arctic_Monkeys_album).jpg",     "Whatever People Say I Am..."),
]

def encode(path):
    img = Image.open(path).convert("RGB").resize((SIZE, SIZE), Image.Resampling.LANCZOS)
    out = bytearray(SIZE * SIZE * 2)
    i = 0
    px = img.load()
    for y in range(SIZE):
        for x in range(SIZE):
            r, g, b = px[x, y]
            v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            out[i]     = v & 0xFF
            out[i + 1] = (v >> 8) & 0xFF
            i += 2
    return bytes(out)


def main():
    here = Path(__file__).resolve().parent
    src_dir = here / "input_albums"
    out_path = here.parent / "cyd" / "esp-idf" / "main" / "album_thumbs.bin"

    if not src_dir.is_dir():
        sys.exit(f"Missing source directory: {src_dir}")

    blob = bytearray()
    for idx, (fname, title) in enumerate(ALBUMS):
        src = src_dir / fname
        if not src.exists():
            sys.exit(f"Missing {src} (expected for album {idx}: {title})")
        data = encode(src)
        blob.extend(data)
        print(f"  [{idx:2d}] {title:38s} <- {fname}  ({len(data)} bytes)")

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(blob)
    print(f"\nWrote {out_path}  ({len(blob)} bytes, {len(ALBUMS)} thumbs @ {SIZE}x{SIZE})")


if __name__ == "__main__":
    main()
