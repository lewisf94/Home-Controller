from __future__ import annotations

import builtins
import contextlib
import importlib.util
import io
import sys
import types
import unittest
from pathlib import Path
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "get_spotify_token_under_test", REPO_ROOT / "get_spotify_token.py"
)
get_spotify_token = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(get_spotify_token)


class _FakeOAuth:
    token_info: dict | None = {"refresh_token": "secret-refresh-token"}
    created: list[dict] = []

    def __init__(self, **kwargs) -> None:
        self.created.append(kwargs)

    def get_authorize_url(self) -> str:
        return "https://accounts.spotify.test/authorize"

    def get_access_token(self, *, as_dict: bool) -> dict | None:
        assert as_dict
        return self.token_info


class SpotifyTokenTests(unittest.TestCase):
    def setUp(self) -> None:
        _FakeOAuth.created.clear()
        _FakeOAuth.token_info = {"refresh_token": "secret-refresh-token"}
        package = types.ModuleType("spotipy")
        oauth2 = types.ModuleType("spotipy.oauth2")
        oauth2.SpotifyOAuth = _FakeOAuth
        package.oauth2 = oauth2
        self.modules = {"spotipy": package, "spotipy.oauth2": oauth2}

    def test_success_path_builds_expected_oauth_request(self) -> None:
        answers = iter([" client-id ", " client-secret "])
        with (
            mock.patch.dict(sys.modules, self.modules),
            mock.patch.object(builtins, "input", side_effect=lambda _: next(answers)),
            contextlib.redirect_stdout(io.StringIO()) as output,
        ):
            result = get_spotify_token.main()

        self.assertEqual(result, 0)
        self.assertEqual(
            _FakeOAuth.created,
            [{
                "client_id": "client-id",
                "client_secret": "client-secret",
                "redirect_uri": get_spotify_token.REDIRECT_URI,
                "scope": get_spotify_token.SCOPE,
            }],
        )
        self.assertIn("secret-refresh-token", output.getvalue())
        self.assertIn("never put credentials in a tracked", output.getvalue())

    def test_missing_refresh_token_returns_failure(self) -> None:
        _FakeOAuth.token_info = {"access_token": "short-lived"}
        answers = iter(["id", "secret"])
        with (
            mock.patch.dict(sys.modules, self.modules),
            mock.patch.object(builtins, "input", side_effect=lambda _: next(answers)),
            contextlib.redirect_stdout(io.StringIO()) as output,
        ):
            result = get_spotify_token.main()
        self.assertEqual(result, 1)
        self.assertIn("Failed to get token", output.getvalue())


if __name__ == "__main__":
    unittest.main()
