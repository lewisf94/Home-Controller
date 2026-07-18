from __future__ import annotations

import csv
import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from PIL import Image


REPO_ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = REPO_ROOT / "scripts"
sys.path.insert(0, str(SCRIPTS))

import convert_albums  # noqa: E402
import embed_albums  # noqa: E402
import embed_albums_idf  # noqa: E402
import preview_albums  # noqa: E402


class AlbumImageTests(unittest.TestCase):
    def test_idf_rgb565_encoding_is_little_endian(self) -> None:
        colors = {
            (0, 0): (255, 0, 0),
            (1, 0): (0, 255, 0),
            (0, 1): (0, 0, 255),
            (1, 1): (255, 255, 255),
        }
        encoded = embed_albums_idf._to_rgb565(
            2, colors, lambda x, y, pixels: pixels[(x, y)]
        )
        self.assertEqual(encoded, bytes([0x00, 0xF8, 0xE0, 0x07, 0x1F, 0x00, 0xFF, 0xFF]))

    def test_placeholder_has_expected_size_and_colour(self) -> None:
        encoded = embed_albums_idf.placeholder(2)
        value = ((32 >> 3) << 11) | ((32 >> 2) << 5) | (32 >> 3)
        pixel = bytes([value & 0xFF, value >> 8])
        self.assertEqual(encoded, pixel * 4)

    def test_find_cover_prefers_mapping_then_supported_id_extensions(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            source = Path(td)
            mapped = source / "mapped.png"
            mapped.write_bytes(b"mapped")
            direct = source / "direct.webp"
            direct.write_bytes(b"direct")
            with (
                mock.patch.object(embed_albums_idf, "SRC_DIR", source),
                mock.patch.dict(embed_albums_idf.IMAGE_FILES, {"mapped": mapped.name}, clear=True),
            ):
                self.assertEqual(embed_albums_idf.find_cover("mapped"), mapped)
                self.assertEqual(embed_albums_idf.find_cover("direct"), direct)
                self.assertIsNone(embed_albums_idf.find_cover("missing"))

    def test_guess_metadata(self) -> None:
        self.assertEqual(
            convert_albums.guess_metadata("Artist_Name-Album_Title"),
            ("Album Title", "Artist Name", ""),
        )
        self.assertEqual(
            convert_albums.guess_metadata("Album_Only"),
            ("Album Only", "Unknown", ""),
        )

    def test_converter_writes_big_endian_rgb565(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            source = Path(td) / "red.png"
            target = Path(td) / "red.bin"
            Image.new("RGB", (1, 1), (255, 0, 0)).save(source)
            convert_albums.convert_to_rgb565(source, target, (1, 1))
            self.assertEqual(target.read_bytes(), bytes([0xF8, 0x00]))

    def test_process_directory_sorts_images_and_preserves_existing_uris(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            source = Path(td) / "input"
            target = Path(td) / "output"
            source.mkdir()
            target.mkdir()
            Image.new("RGB", (2, 2), (0, 255, 0)).save(source / "Zed-Last.png")
            Image.new("RGB", (2, 2), (255, 0, 0)).save(source / "Alpha-First.jpg")
            with (target / "metadata.csv").open("w", newline="", encoding="utf-8") as fh:
                csv.writer(fh).writerow(
                    ["Alpha-First.bin", "old", "old", "spotify:album:keep"]
                )

            with (
                mock.patch.object(convert_albums, "TARGET_SIZE", (1, 1)),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                convert_albums.process_directory(source, target)

            with (target / "metadata.csv").open(newline="", encoding="utf-8") as fh:
                rows = list(csv.reader(fh))
            self.assertEqual(
                rows,
                [
                    ["Alpha-First.bin", "First", "Alpha", "spotify:album:keep"],
                    ["Zed-Last.bin", "Last", "Zed", ""],
                ],
            )
            self.assertEqual((target / "Alpha-First.bin").stat().st_size, 2)
            self.assertEqual((target / "Zed-Last.bin").stat().st_size, 2)

    def test_preview_size_matches_converter_and_decodes_rgb565(self) -> None:
        self.assertEqual(
            preview_albums.SIZE,
            convert_albums.TARGET_SIZE[0],
            "preview and conversion dimensions must remain aligned",
        )
        red = bytes([0xF8, 0x00])
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "cover.bin"
            path.write_bytes(red * (preview_albums.SIZE * preview_albums.SIZE))
            image = preview_albums.read_rgb565_bin(path)
        self.assertEqual(image.size, (preview_albums.SIZE, preview_albums.SIZE))
        self.assertEqual(image.getpixel((0, 0)), (255, 0, 0))
        self.assertEqual(image.getpixel((preview_albums.SIZE - 1, preview_albums.SIZE - 1)), (255, 0, 0))

    def test_preview_handles_a_truncated_file_without_throwing(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "short.bin"
            path.write_bytes(bytes([0x07, 0xE0]))
            image = preview_albums.read_rgb565_bin(path)
        self.assertEqual(image.getpixel((0, 0)), (0, 255, 0))
        self.assertEqual(image.getpixel((1, 0)), (0, 0, 0))

    def test_legacy_header_conversion_preserves_big_endian_words(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            source = Path(td) / "two.bin"
            header = Path(td) / "two.h"
            source.write_bytes(bytes([0x12, 0x34, 0xAB, 0xCD]))
            with contextlib.redirect_stdout(io.StringIO()):
                embed_albums.bin_to_header(source, header, "album_art_0")
            text = header.read_text()
        self.assertIn("const uint16_t album_art_0[2]", text)
        self.assertIn("0x1234,0xABCD,", text)


if __name__ == "__main__":
    unittest.main()
