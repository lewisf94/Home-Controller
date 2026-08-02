from __future__ import annotations

import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]

ALBUM_FILES = [
    REPO_ROOT / "cyd" / "esp-idf" / "main" / "albums.c",
    REPO_ROOT / "cyd" / "esp-idf-ha" / "main" / "albums.c",
    REPO_ROOT / "waveshare" / "components" / "p4_shared" / "albums.c",
    REPO_ROOT / "cyd" / "platformio" / "src" / "albums.cpp",
]

URI_PATTERN = re.compile(r'"(spotify:album:[A-Za-z0-9]+)"')


class RepositoryContractTests(unittest.TestCase):
    def test_generated_album_catalogues_have_identical_uri_order(self) -> None:
        catalogues = [URI_PATTERN.findall(path.read_text(encoding="utf-8")) for path in ALBUM_FILES]
        self.assertTrue(catalogues[0], "the committed album catalogue must not be empty")
        for path, catalogue in zip(ALBUM_FILES[1:], catalogues[1:]):
            with self.subTest(path=path):
                self.assertEqual(catalogue, catalogues[0])

    def test_p4_backends_share_unicode_json_decoder(self) -> None:
        codec = (
            REPO_ROOT / "waveshare" / "components" / "p4_shared" / "text_codec.c"
        ).read_text(encoding="utf-8")
        self.assertIn("bool p4_json_copy_string", codec)
        for relative in (
            Path("waveshare/esp-idf/main/spotify.c"),
            Path("waveshare/esp-idf-ha/main/ha_client.c"),
        ):
            with self.subTest(relative=relative):
                source = (REPO_ROOT / relative).read_text(encoding="utf-8")
                self.assertIn("return p4_json_copy_string(p, out, out_len);", source)

    def test_shared_component_builds_the_codec_and_catalogue(self) -> None:
        cmake = (
            REPO_ROOT / "waveshare" / "components" / "p4_shared" / "CMakeLists.txt"
        ).read_text(encoding="utf-8")
        self.assertIn('"text_codec.c"', cmake)
        self.assertIn('"album_catalog.c"', cmake)

    def test_neo_hifi_theme_keeps_existing_modes_and_font_contract(self) -> None:
        shared = REPO_ROOT / "waveshare" / "components" / "p4_shared"
        ui = (shared / "ui.c").read_text(encoding="utf-8")
        cmake = (shared / "CMakeLists.txt").read_text(encoding="utf-8")

        # HIFI is appended after the existing persisted modes. Its developer
        # defaults map to BASIC because the editable tuning header has five
        # source columns and must not be expanded for this visual-only theme.
        self.assertIn(
            "MODE_BASIC = 0, MODE_GLYPH, MODE_PIXEL, MODE_PAPER, MODE_BOLD, MODE_HIFI, MODE_COUNT",
            ui,
        )
        self.assertIn("mode == MODE_HIFI ? MODE_BASIC : mode", ui)
        self.assertIn('#define NVS_KEY_HIFI_FONT     "hifi_font"', ui)

        for family in ("terminal", "gtl001", "space", "bebas"):
            for size in (20, 28):
                source = f"lv_font_hifi_{family}_{size}.c"
                with self.subTest(source=source):
                    self.assertTrue((shared / source).is_file())
                    self.assertIn(f'"{source}"', cmake)

        self.assertTrue((shared / "THIRD_PARTY_FONTS.md").is_file())
        self.assertTrue((shared / "OFL-1.1.txt").is_file())

    def test_neo_hifi_theme_preserves_legacy_settings_and_ui_contracts(self) -> None:
        ui = (
            REPO_ROOT / "waveshare" / "components" / "p4_shared" / "ui.c"
        ).read_text(encoding="utf-8")

        self.assertIn("#define TUNE_MODE_COUNT MODE_HIFI", ui)
        self.assertIn("fidx * TUNE_MODE_COUNT + default_mode", ui)
        self.assertIn("m < TUNE_MODE_COUNT", ui)
        self.assertIn("m == TUNE_MODE_COUNT - 1", ui)
        self.assertIn(
            "nvs_get_blob(h, NVS_KEY_ACCENT_PM, NULL, &pm_len)",
            ui,
        )
        self.assertIn("pm_len <= sizeof pm", ui)
        self.assertIn("FULL COLOUR (FIXED)", ui)
        self.assertIn("tick_x0 + i * tick_pitch", ui)
        self.assertIn(
            "is_hifi_theme() && !s_dv_set[MODE_HIFI][0][DV_BTN_BORDER]",
            ui,
        )

    def test_p4_and_rp2040_protocol_framing_contracts_match(self) -> None:
        p4 = (
            REPO_ROOT / "waveshare" / "components" / "p4_shared" / "knob.c"
        ).read_text(encoding="utf-8")
        rp = (REPO_ROOT / "rp2040" / "src" / "interface_task.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn("#define PROTOCOL_VERSION   1", p4)
        self.assertIn("#define PROTOCOL_VERSION 1", rp)
        self.assertIn("esp_rom_crc32_le(0, data, len)", p4)
        self.assertNotIn("esp_rom_crc32_le(0, data, len) ^", p4)
        self.assertIn("0xEDB88320", rp)
        self.assertIn("return ~crc;", rp)
        for source in (p4, rp):
            self.assertIn("static size_t _cobs_encode", source)
            self.assertIn("static size_t _cobs_decode", source)

    def test_ci_workflow_runs_host_suite(self) -> None:
        workflow = REPO_ROOT / ".github" / "workflows" / "host-tests.yml"
        self.assertTrue(workflow.is_file())
        text = workflow.read_text(encoding="utf-8")
        self.assertIn("python -m unittest discover", text)
        self.assertIn("check_p4_reliability.py both", text)


if __name__ == "__main__":
    unittest.main()
