from __future__ import annotations

import contextlib
import io
import math
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = REPO_ROOT / "scripts"
sys.path.insert(0, str(SCRIPTS))

import check_p4_reliability as reliability  # noqa: E402


COMMON_DEFAULTS = [
    "CONFIG_SPIRAM_USE_MALLOC=y",
    "CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y",
    "CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y",
    "CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT=1",
    "# CONFIG_LV_USE_TINY_TTF is not set",
    "CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y",
    "CONFIG_ESP_TASK_WDT_PANIC=y",
]

HA_DEFAULTS = [
    "CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=65536",
    "CONFIG_MDNS_TASK_CREATE_FROM_SPIRAM=y",
    "CONFIG_MDNS_MEMORY_ALLOC_SPIRAM=y",
    "CONFIG_SENDSPIN_ENABLE_PLAYER=y",
    "CONFIG_SENDSPIN_ENABLE_CONTROLLER=n",
    "CONFIG_SENDSPIN_ENABLE_METADATA=n",
    "CONFIG_SENDSPIN_ENABLE_ARTWORK=n",
    "CONFIG_SENDSPIN_ENABLE_VISUALIZER=n",
    "CONFIG_SENDSPIN_ENABLE_COLOR=n",
]


def _write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def _make_source_tree(root: Path, target: str) -> None:
    defaults = COMMON_DEFAULTS + (HA_DEFAULTS if target == "esp-idf-ha" else [])
    _write(root / "waveshare" / target / "sdkconfig.defaults", "\n".join(defaults) + "\n")
    _write(
        root / "waveshare" / target / "partitions.csv",
        "nvs, data, nvs, 0x9000, 0x6000\n"
        "coredump, data, coredump, 0xf000, 0x10000\n"
        "ota_0, app, ota_0, 0x20000, 0xA00000\n"
        "ota_1, app, ota_1, 0xA20000, 0xA00000\n",
    )
    _write(
        root / "waveshare" / target / "main" / "main.c",
        "app_core_reliability_init();\n"
        "app_core_ota_start(url);\n"
        "xTaskCreatePinnedToCoreWithCaps(worker);\n"
        "xQueueCreateWithCaps(depth);\n",
    )
    _write(
        root / "waveshare" / "components" / "p4_shared" / "audio.c",
        "xTaskCreatePinnedToCoreWithCaps(audio_task, x);\n"
        "xQueueCreateWithCaps(6, sizeof(audio_sfx_t), x);\n",
    )
    _write(
        root / "waveshare" / "components" / "app_core" / "reliability.c",
        "esp_reset_reason(); esp_core_dump_image_check();\n",
    )
    if target == "esp-idf-ha":
        _write(
            root / "waveshare" / target / "main" / "ha_client.c",
            "request_inventory_refresh();\n"
            "APP_CORE_INTERNAL_NETWORK_MIN_FREE;\n"
            "#define RX_MAX_CAP (768 * 1024)\n"
            "media_player_is_spotify_account();\n"
            'json_obj_get(attrs, "supported_features", &supported);\n'
            r'{\"type\":\"get_states\"}' "\n",
        )


def _truncate(path: Path, size: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as fh:
        fh.truncate(size)


class P4ReliabilityTests(unittest.TestCase):
    def test_current_repository_passes_source_gates(self) -> None:
        errors: list[str] = []
        for target in ("esp-idf", "esp-idf-ha"):
            reliability.check_source(REPO_ROOT, target, errors)
        self.assertEqual(errors, [])

    def test_complete_fixture_passes_both_targets(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            for target in ("esp-idf", "esp-idf-ha"):
                _make_source_tree(root, target)
            errors: list[str] = []
            reliability.check_source(root, "esp-idf", errors)
            reliability.check_source(root, "esp-idf-ha", errors)
        self.assertEqual(errors, [])

    def test_each_common_sdkconfig_gate_is_required(self) -> None:
        for option in COMMON_DEFAULTS:
            with self.subTest(option=option), tempfile.TemporaryDirectory() as td:
                root = Path(td)
                _make_source_tree(root, "esp-idf")
                defaults = root / "waveshare" / "esp-idf" / "sdkconfig.defaults"
                defaults.write_text(
                    defaults.read_text(encoding="utf-8").replace(option + "\n", ""),
                    encoding="utf-8",
                )
                errors: list[str] = []
                reliability.check_source(root, "esp-idf", errors)
                self.assertTrue(errors, option)

    def test_ha_rejects_extra_inventory_call_sites_and_changed_rx_cap(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _make_source_tree(root, "esp-idf-ha")
            ha = root / "waveshare" / "esp-idf-ha" / "main" / "ha_client.c"
            text = ha.read_text(encoding="utf-8")
            text = text.replace("#define RX_MAX_CAP (768 * 1024)", "#define RX_MAX_CAP (1024 * 1024)")
            text += r'{\"type\":\"get_states\"}' * 3
            ha.write_text(text, encoding="utf-8")
            errors: list[str] = []
            reliability.check_source(root, "esp-idf-ha", errors)
        self.assertTrue(any("more than two get_states" in error for error in errors))
        self.assertTrue(any("websocket cap changed" in error for error in errors))

    def test_binary_accepts_exact_effective_headroom_limits(self) -> None:
        effective_free = max(
            reliability.MIN_APP_FREE_BYTES,
            math.ceil(reliability.APP_PARTITION_BYTES * reliability.MIN_APP_FREE_PERCENT / 100),
        )
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            binary = root / "waveshare" / "esp-idf" / "build" / "music_controller_p4.bin"
            boot = root / "waveshare" / "esp-idf" / "build" / "bootloader" / "bootloader.bin"
            _truncate(binary, reliability.APP_PARTITION_BYTES - effective_free)
            _truncate(
                boot,
                reliability.BOOTLOADER_PARTITION_BYTES - reliability.MIN_BOOTLOADER_FREE_BYTES,
            )
            errors: list[str] = []
            with contextlib.redirect_stdout(io.StringIO()):
                reliability.check_binary(root, "esp-idf", errors)
        self.assertEqual(errors, [])

    def test_binary_reports_missing_and_over_budget_outputs(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            errors: list[str] = []
            reliability.check_binary(root, "esp-idf-ha", errors)
            self.assertEqual(len(errors), 1)
            self.assertIn("expected binary not found", errors[0])

            binary = root / "waveshare" / "esp-idf-ha" / "build" / "music_controller_p4_ha.bin"
            boot = root / "waveshare" / "esp-idf-ha" / "build" / "bootloader" / "bootloader.bin"
            _truncate(binary, reliability.APP_PARTITION_BYTES - 1)
            _truncate(boot, reliability.BOOTLOADER_PARTITION_BYTES)
            errors = []
            with contextlib.redirect_stdout(io.StringIO()):
                reliability.check_binary(root, "esp-idf-ha", errors)
        self.assertTrue(any("app headroom" in error for error in errors))
        self.assertTrue(any("bootloader headroom" in error for error in errors))

    def test_cli_main_reports_errors_and_success(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _make_source_tree(root, "esp-idf")
            with (
                mock.patch.object(sys, "argv", ["check", "esp-idf", "--root", str(root)]),
                contextlib.redirect_stdout(io.StringIO()) as stdout,
            ):
                self.assertEqual(reliability.main(), 0)
            self.assertIn("source/configuration gates passed", stdout.getvalue())

            defaults = root / "waveshare" / "esp-idf" / "sdkconfig.defaults"
            defaults.write_text("", encoding="utf-8")
            with (
                mock.patch.object(sys, "argv", ["check", "esp-idf", "--root", str(root)]),
                contextlib.redirect_stderr(io.StringIO()) as stderr,
            ):
                self.assertEqual(reliability.main(), 1)
            self.assertIn("ERROR:", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
