from __future__ import annotations

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

import gen_lvgl_font  # noqa: E402


class GenLvglFontTests(unittest.TestCase):
    def test_quantize_nibbles_rounds_and_pads_odd_pixel(self) -> None:
        self.assertEqual(
            gen_lvgl_font._quantize_nibbles([0, 255, 127]),
            bytes([0x0F, 0x70]),
        )
        self.assertEqual(gen_lvgl_font._quantize_nibbles([]), b"")

    def test_dotify_preserves_canvas_and_stamps_only_inked_cells(self) -> None:
        image = Image.new("L", (8, 8), 0)
        image.putpixel((1, 1), 255)
        dotted = gen_lvgl_font._dotify(image, pitch=4, radius=1.2, thresh=120)
        self.assertEqual(dotted.size, image.size)
        self.assertIsNotNone(dotted.getbbox())
        self.assertEqual(dotted.crop((4, 4, 8, 8)).getbbox(), None)

    def test_main_generates_sparse_cmap_fallback_and_sorted_unique_codepoints(self) -> None:
        font = REPO_ROOT / "waveshare" / "components" / "p4_shared" / "DejaVuSans.ttf"
        with tempfile.TemporaryDirectory() as td:
            output = Path(td) / "font.c"
            argv = [
                "gen_lvgl_font.py",
                str(font),
                "12",
                str(output),
                "test_font",
                "--codepoints",
                "0x42,0x20,0x42",
                "--fallback",
                "lv_font_montserrat_12",
            ]
            with (
                mock.patch.object(sys, "argv", argv),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                gen_lvgl_font.main()
            generated = output.read_text(encoding="utf-8")

        self.assertIn("2 sparse", generated)
        self.assertIn("LV_FONT_FMT_TXT_CMAP_SPARSE_TINY", generated)
        self.assertIn("glyph_unicode_list", generated)
        self.assertIn("0, 34", generated)
        self.assertIn("extern const lv_font_t lv_font_montserrat_12", generated)
        self.assertIn(".fallback = &lv_font_montserrat_12", generated)

    def test_main_generates_contiguous_default_cmap(self) -> None:
        font = REPO_ROOT / "waveshare" / "components" / "p4_shared" / "DejaVuSans.ttf"
        with tempfile.TemporaryDirectory() as td:
            output = Path(td) / "font.c"
            argv = ["gen_lvgl_font.py", str(font), "8", str(output), "ascii_font"]
            with (
                mock.patch.object(sys, "argv", argv),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                gen_lvgl_font.main()
            generated = output.read_text(encoding="utf-8")
        self.assertIn("LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY", generated)
        self.assertIn(".range_start = 32", generated)
        self.assertIn(".range_length = 95", generated)

    def test_main_rejects_conflicting_or_invalid_ranges(self) -> None:
        font = REPO_ROOT / "waveshare" / "components" / "p4_shared" / "DejaVuSans.ttf"
        cases = [
            ["--codepoints", "0x20", "--ranges", "0x20-0x21"],
            ["--ranges", "0x30-0x20"],
            ["--ranges", "0x110000"],
            ["--codepoints", ""],
        ]
        for extra in cases:
            with self.subTest(extra=extra), tempfile.TemporaryDirectory() as td:
                argv = [
                    "gen_lvgl_font.py",
                    str(font),
                    "8",
                    str(Path(td) / "font.c"),
                    "bad_font",
                    *extra,
                ]
                with (
                    mock.patch.object(sys, "argv", argv),
                    contextlib.redirect_stdout(io.StringIO()),
                    contextlib.redirect_stderr(io.StringIO()),
                    self.assertRaises(SystemExit),
                ):
                    gen_lvgl_font.main()


if __name__ == "__main__":
    unittest.main()
