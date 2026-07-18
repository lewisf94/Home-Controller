from __future__ import annotations

import contextlib
import io
import sys
import tempfile
import unittest
import unicodedata
from pathlib import Path
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = REPO_ROOT / "scripts"
sys.path.insert(0, str(SCRIPTS))

import gen_albums  # noqa: E402


class GenAlbumsTests(unittest.TestCase):
    def test_parse_master_filters_noise_and_normalizes_unicode(self) -> None:
        decomposed = unicodedata.normalize("NFD", "Beyonce\u0301")
        text = (
            "# a note\n"
            "not an album\n"
            f'  "Title", "{decomposed}", "spotify:album:ABC123",  \n'
            '"Missing URI", "Artist", "https://example.test"\n'
            '"Another", "Artist", "spotify:album:XYZ789", "ignored"\n'
        )
        with tempfile.TemporaryDirectory() as td:
            master = Path(td) / "albums.txt"
            master.write_text(text, encoding="utf-8")
            albums = gen_albums.parse_master(master)

        self.assertEqual(
            albums,
            [
                ("Title", unicodedata.normalize("NFC", decomposed), "spotify:album:ABC123"),
                ("Another", "Artist", "spotify:album:XYZ789"),
            ],
        )

    def test_sort_ignores_leading_articles_for_artist_and_title(self) -> None:
        albums = [
            ("The Zebra", "The Beta", "spotify:album:3"),
            ("A Moon", "Alpha", "spotify:album:2"),
            ("An Apple", "Alpha", "spotify:album:1"),
            ("Plain", "A Gamma", "spotify:album:4"),
        ]
        self.assertEqual(
            [album[2] for album in gen_albums.sort_albums(albums)],
            ["spotify:album:1", "spotify:album:2", "spotify:album:3", "spotify:album:4"],
        )

    def test_ascii_fold_preserves_meaningful_punctuation_and_letters(self) -> None:
        self.assertEqual(
            gen_albums._ascii_punct("Bjork – Where’s My Utopia?…".replace("Bjork", "Björk")),
            "Bjork - Where's My Utopia?...",
        )
        self.assertEqual(gen_albums._ascii_punct("Smørrebrød & Œuvre"), "Smorrebrod & OEuvre")

    def test_render_escapes_c_strings_and_uses_language_null(self) -> None:
        albums = [('A "quote" \\ title', "Artist", "spotify:album:one")]
        c_text = gen_albums._render(albums, "c")
        cpp_text = gen_albums._render(albums, "cpp")

        self.assertIn(r'"A \"quote\" \\ title"', c_text)
        self.assertIn("return NULL;", c_text)
        self.assertIn("return nullptr;", cpp_text)

    def test_render_handles_an_empty_catalogue(self) -> None:
        rendered = gen_albums._render([], "c")
        self.assertIn("static const album_entry_t s_albums[]", rendered)
        self.assertIn("albums_count", rendered)

    def test_main_writes_all_targets_with_per_target_unicode_policy(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            master = root / "spotify-albums-list.txt"
            master.write_text(
                '"Début…", "Björk", "spotify:album:unicode",\n',
                encoding="utf-8",
            )
            c_ascii = root / "ascii.c"
            c_unicode = root / "unicode.c"
            cpp_ascii = root / "ascii.cpp"
            targets = [
                (c_ascii, "c", False),
                (c_unicode, "c", True),
                (cpp_ascii, "cpp", False),
            ]

            with (
                mock.patch.object(gen_albums, "REPO_ROOT", root),
                mock.patch.object(gen_albums, "MASTER", master),
                mock.patch.object(gen_albums, "TARGETS", targets),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                gen_albums.main()

            self.assertIn('"Debut..."', c_ascii.read_text(encoding="utf-8"))
            self.assertIn('"Début…"', c_unicode.read_text(encoding="utf-8"))
            self.assertIn("return nullptr;", cpp_ascii.read_text(encoding="utf-8"))

    def test_main_rejects_missing_or_empty_master(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            missing = root / "missing.txt"
            with (
                mock.patch.object(gen_albums, "MASTER", missing),
                self.assertRaisesRegex(SystemExit, "Missing master album list"),
            ):
                gen_albums.main()

            empty = root / "empty.txt"
            empty.write_text("notes only\n", encoding="utf-8")
            with (
                mock.patch.object(gen_albums, "MASTER", empty),
                self.assertRaisesRegex(SystemExit, "No album lines parsed"),
            ):
                gen_albums.main()


if __name__ == "__main__":
    unittest.main()
