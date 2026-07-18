from __future__ import annotations

import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = REPO_ROOT / "scripts"
sys.path.insert(0, str(SCRIPTS))

import add_albums  # noqa: E402


ALBUM_ID = "A" * 22
SECOND_ID = "B" * 22


class _FakeSpotify:
    def __init__(self) -> None:
        self.calls: list[str] = []

    def album(self, album_id: str) -> dict:
        self.calls.append(album_id)
        return {
            "name": "To Pimp A Butterfly",
            "artists": [{"name": "The Example Artist"}],
            "images": [{"url": "https://example.test/cover.jpg"}],
        }


class AddAlbumsTests(unittest.TestCase):
    def test_id_pattern_accepts_uri_and_share_url(self) -> None:
        for value in (
            f"spotify:album:{ALBUM_ID}",
            f"https://open.spotify.com/album/{ALBUM_ID}?si=tracking",
        ):
            with self.subTest(value=value):
                self.assertEqual(add_albums._ID.search(value).group(1), ALBUM_ID)
        self.assertIsNone(add_albums._ID.search("spotify:track:" + ALBUM_ID))
        self.assertIsNone(add_albums._ID.search("spotify:album:short"))

    def test_complete_parse_format_and_tidy(self) -> None:
        line = f'"Title", "Artist", "spotify:album:{ALBUM_ID}",'
        self.assertTrue(add_albums._is_complete(line))
        self.assertEqual(
            add_albums._parse_one(line),
            ("Title", "Artist", f"spotify:album:{ALBUM_ID}"),
        )
        self.assertEqual(add_albums._tidy("To Pimp A Butterfly"), "To Pimp a Butterfly")
        self.assertEqual(add_albums._tidy("hugo"), "hugo")
        self.assertEqual(
            add_albums._fmt('A "Quoted" Album', 'An "Artist"', ALBUM_ID),
            f'"A \'Quoted\' Album", "An \'Artist\'", "spotify:album:{ALBUM_ID}",',
        )

    def test_download_cover_writes_bytes_and_skips_existing_file(self) -> None:
        response = mock.MagicMock()
        response.__enter__.return_value.read.return_value = b"jpeg bytes"

        with tempfile.TemporaryDirectory() as td:
            cover_dir = Path(td)
            with (
                mock.patch.object(add_albums, "INPUT_ALBUMS", cover_dir),
                mock.patch.object(add_albums.urllib.request, "urlopen", return_value=response) as urlopen,
                contextlib.redirect_stdout(io.StringIO()),
            ):
                add_albums._download_cover(ALBUM_ID, [{"url": "https://example.test/a.jpg"}])
                self.assertEqual((cover_dir / f"{ALBUM_ID}.jpg").read_bytes(), b"jpeg bytes")
                add_albums._download_cover(ALBUM_ID, [{"url": "https://example.test/b.jpg"}])

            self.assertEqual(urlopen.call_count, 1)
            request = urlopen.call_args.args[0]
            self.assertEqual(request.headers["User-agent"], "music-controller")

    def test_download_cover_handles_missing_images_without_network(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            with (
                mock.patch.object(add_albums, "INPUT_ALBUMS", Path(td)),
                mock.patch.object(add_albums.urllib.request, "urlopen") as urlopen,
                contextlib.redirect_stdout(io.StringIO()) as output,
            ):
                add_albums._download_cover(ALBUM_ID, [])
            urlopen.assert_not_called()
            self.assertIn("no cover available", output.getvalue())

    def test_main_resolves_caches_normalizes_and_runs_pipeline(self) -> None:
        fake = _FakeSpotify()
        with tempfile.TemporaryDirectory() as td:
            master = Path(td) / "albums.txt"
            master.write_text(
                "# keep this note\n"
                f"https://open.spotify.com/album/{ALBUM_ID}?si=one\n"
                f"spotify:album:{ALBUM_ID}\n"
                f'"Existing The Title", "Artist Of Things", "spotify:album:{SECOND_ID}",\n',
                encoding="utf-8",
            )
            argv = ["add_albums.py", "--no-covers"]
            with (
                mock.patch.object(add_albums, "MASTER", master),
                mock.patch.object(add_albums, "_spotify", return_value=fake) as spotify_factory,
                mock.patch.object(add_albums.gen_albums, "main") as generate,
                mock.patch.object(add_albums.embed_albums_idf, "main") as embed,
                mock.patch.object(sys, "argv", argv),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                add_albums.main()

            lines = master.read_text(encoding="utf-8").splitlines()
            self.assertEqual(lines[0], "# keep this note")
            expected = (
                f'"To Pimp a Butterfly", "The Example Artist", '
                f'"spotify:album:{ALBUM_ID}",'
            )
            self.assertEqual(lines[1], expected)
            self.assertEqual(lines[2], expected)
            self.assertEqual(
                lines[3],
                f'"Existing the Title", "Artist of Things", "spotify:album:{SECOND_ID}",',
            )
            self.assertEqual(fake.calls, [ALBUM_ID])
            spotify_factory.assert_called_once_with()
            generate.assert_called_once_with()
            embed.assert_called_once_with()

    def test_main_flags_skip_generation_and_embedding(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            master = Path(td) / "albums.txt"
            master.write_text("notes only\n", encoding="utf-8")
            argv = ["add_albums.py", "--no-generate", "--no-embed", "--no-covers"]
            with (
                mock.patch.object(add_albums, "MASTER", master),
                mock.patch.object(add_albums, "_spotify") as spotify,
                mock.patch.object(add_albums.gen_albums, "main") as generate,
                mock.patch.object(add_albums.embed_albums_idf, "main") as embed,
                mock.patch.object(sys, "argv", argv),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                add_albums.main()
            spotify.assert_not_called()
            generate.assert_not_called()
            embed.assert_not_called()


if __name__ == "__main__":
    unittest.main()
