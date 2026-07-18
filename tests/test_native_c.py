from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
NATIVE = REPO_ROOT / "tests" / "native"
STUBS = NATIVE / "stubs"


def _find_compiler() -> str | None:
    for candidate in (os.environ.get("CC"), "cc", "gcc", "clang"):
        if candidate and shutil.which(candidate):
            return candidate
    return None


class NativeCTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.compiler = _find_compiler()
        if not cls.compiler:
            raise unittest.SkipTest("no native C compiler is available")
        cls.tempdir = tempfile.TemporaryDirectory()
        cls.build_dir = Path(cls.tempdir.name)

    @classmethod
    def tearDownClass(cls) -> None:
        if hasattr(cls, "tempdir"):
            cls.tempdir.cleanup()

    def _compile(self, name: str, *sources: Path) -> Path:
        executable = self.build_dir / name
        command = [
            self.compiler,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Wno-unused-parameter",
            "-Wno-unused-function",
            "-I",
            str(STUBS),
            *map(str, sources),
            "-o",
            str(executable),
        ]
        completed = subprocess.run(command, text=True, capture_output=True, check=False)
        self.assertEqual(
            completed.returncode,
            0,
            f"native compile failed:\n{completed.stdout}\n{completed.stderr}",
        )
        return executable

    def _run(self, executable: Path, *args: str) -> None:
        completed = subprocess.run(
            [str(executable), *args], text=True, capture_output=True, check=False
        )
        self.assertEqual(
            completed.returncode,
            0,
            f"native test failed ({' '.join(args)}):\n{completed.stdout}\n{completed.stderr}",
        )

    def test_shared_json_text_codec(self) -> None:
        executable = self._compile(
            "text_codec",
            NATIVE / "test_text_codec.c",
            REPO_ROOT / "waveshare" / "components" / "p4_shared" / "text_codec.c",
        )
        self._run(executable)

    def test_cyd_mcp_encoder_and_button_state_machines(self) -> None:
        executable = self._compile("mcp_input", NATIVE / "test_mcp_input.c")
        self._run(executable)

    def test_cyd_input_dispatcher(self) -> None:
        executable = self._compile("input_dispatch", NATIVE / "test_input_dispatch.c")
        for scenario in ("browser", "transport", "volume", "mute", "short", "long"):
            with self.subTest(scenario=scenario):
                self._run(executable, scenario)

    def test_p4_art_double_buffer(self) -> None:
        executable = self._compile("art_buffer", NATIVE / "test_art_buffer.c")
        self._run(executable)

    def test_p4_reliability_runtime_gates(self) -> None:
        executable = self._compile("reliability", NATIVE / "test_reliability.c")
        self._run(executable)

    def test_runtime_album_catalogue(self) -> None:
        executable = self._compile("album_catalog", NATIVE / "test_album_catalog.c")
        for scenario in ("helpers", "load-sort", "add", "art"):
            with self.subTest(scenario=scenario):
                self._run(executable, scenario)

    def test_runtime_credentials(self) -> None:
        executable = self._compile("creds", NATIVE / "test_creds.c")
        self._run(executable)


if __name__ == "__main__":
    unittest.main()
