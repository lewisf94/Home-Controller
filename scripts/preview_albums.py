#!/usr/bin/env python3
"""Preview RGB565 .bin album art files to verify they look correct before loading onto SD card."""
import os
import struct
import sys

try:
    from PIL import Image
except ImportError:
    print("Error: Pillow library is required. Install it using: pip install Pillow")
    sys.exit(1)

# Keep this aligned with convert_albums.py: the CYD SD-card thumbnails are
# 80x80 RGB565 images.  A host test locks the two scripts together so a future
# size change cannot silently make previews read past the end of every file.
SIZE = 80

def read_rgb565_bin(path):
    """Read an RGB565 .bin file and return a PIL Image."""
    img = Image.new('RGB', (SIZE, SIZE))
    pixels = img.load()

    with open(path, 'rb') as f:
        for y in range(SIZE):
            for x in range(SIZE):
                data = f.read(2)
                if len(data) < 2:
                    return img
                rgb565 = struct.unpack('>H', data)[0]

                r5 = (rgb565 >> 11) & 0x1F
                g6 = (rgb565 >> 5) & 0x3F
                b5 = rgb565 & 0x1F

                r8 = (r5 << 3) | (r5 >> 2)
                g8 = (g6 << 2) | (g6 >> 4)
                b8 = (b5 << 3) | (b5 >> 2)

                pixels[x, y] = (r8, g8, b8)
    return img

def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    bin_dir = os.path.join(script_dir, "sd_card_albums")

    if not os.path.exists(bin_dir):
        print("No sd_card_albums folder found. Run convert_albums.py first!")
        return

    bins = sorted([f for f in os.listdir(bin_dir) if f.endswith('.bin')])
    if not bins:
        print("No .bin files found in sd_card_albums/")
        return

    print(f"Found {len(bins)} album(s). Building preview grid...")

    # Layout: 4 columns
    cols = 4
    rows = (len(bins) + cols - 1) // cols
    padding = 10
    label_h = 20
    cell_w = SIZE + padding
    cell_h = SIZE + padding + label_h

    grid = Image.new('RGB', (cols * cell_w + padding, rows * cell_h + padding), (30, 30, 30))

    from PIL import ImageDraw, ImageFont
    draw = ImageDraw.Draw(grid)

    for i, name in enumerate(bins):
        col = i % cols
        row = i // cols
        x = padding + col * cell_w
        y = padding + row * cell_h

        img = read_rgb565_bin(os.path.join(bin_dir, name))
        grid.paste(img, (x, y))

        label = os.path.splitext(name)[0][:20]  # Truncate long names
        draw.text((x, y + SIZE + 2), label, fill=(200, 200, 200))

    output_path = os.path.join(script_dir, "album_preview.png")
    grid.save(output_path)
    print(f"Preview saved to: {output_path}")

    # Try to open it automatically
    try:
        os.startfile(output_path)
    except Exception:
        print("Open the file manually to view it.")

if __name__ == "__main__":
    main()
