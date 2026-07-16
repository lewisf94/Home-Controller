#!/usr/bin/env python3
"""Fail fast when a P4 build violates the project's reliability budget."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


# App slot is 10 MB since the dual-app OTA repartition (partitions.csv:
# ota_0 / ota_1 @ 0xA00000 each). Keep in sync with the partition table.
APP_PARTITION_BYTES = 0xA00000
MIN_APP_FREE_BYTES = 512 * 1024
MIN_APP_FREE_PERCENT = 8.0
BOOTLOADER_PARTITION_BYTES = 0x6000
MIN_BOOTLOADER_FREE_BYTES = 0x100


def require(condition: bool, message: str, errors: list[str]) -> None:
    if not condition:
        errors.append(message)


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def check_source(root: Path, target: str, errors: list[str]) -> None:
    defaults = read(root / "waveshare" / target / "sdkconfig.defaults")
    require("CONFIG_SPIRAM_USE_MALLOC=y" in defaults,
            f"{target}: PSRAM malloc support must stay enabled", errors)
    require("CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y" in defaults,
            f"{target}: WiFi/lwIP must prefer PSRAM", errors)
    require("CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y" in defaults,
            f"{target}: external task stacks must be explicitly enabled", errors)
    require("CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT=1" in defaults,
            f"{target}: LVGL draw units must remain 1 with PPA rotation", errors)
    require("# CONFIG_LV_USE_TINY_TTF is not set" in defaults,
            f"{target}: runtime tiny_ttf must stay disabled on the P4; use compiled fonts",
            errors)
    require("CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y" in defaults,
            f"{target}: crash coredump-to-flash must stay enabled (report-on-boot)", errors)
    require("CONFIG_ESP_TASK_WDT_PANIC=y" in defaults,
            f"{target}: task watchdog must reset (panic) on a hang, not just warn", errors)

    parts = read(root / "waveshare" / target / "partitions.csv")
    require("coredump" in parts,
            f"{target}: coredump partition missing (crash-report-on-boot)", errors)
    require("ota_0" in parts and "ota_1" in parts,
            f"{target}: dual-app OTA partitions missing", errors)
    require(re.search(r"nvs,\s+data,\s+nvs,\s+0x9000", parts) is not None,
            f"{target}: nvs must stay at 0x9000 so SETUP credentials survive a reflash", errors)

    main = read(root / "waveshare" / target / "main" / "main.c")
    require("app_core_reliability_init();" in main,
            f"{target}: reliability guard is not initialized", errors)
    require("app_core_ota_start(" in main,
            f"{target}: OTA update path is not wired", errors)
    require("xTaskCreatePinnedToCoreWithCaps" in main,
            f"{target}: application worker stack is not explicitly in PSRAM", errors)
    require("xQueueCreateWithCaps" in main,
            f"{target}: application queue storage is not explicitly in PSRAM", errors)

    audio = read(root / "waveshare" / "components" / "p4_shared" / "audio.c")
    require("xTaskCreatePinnedToCoreWithCaps(audio_task" in audio,
            "p4_shared/audio: audio task stack must stay in PSRAM", errors)
    require("xQueueCreateWithCaps(6, sizeof(audio_sfx_t)" in audio,
            "p4_shared/audio: SFX queue must stay in PSRAM", errors)

    reliability = read(root / "waveshare" / "components" / "app_core" / "reliability.c")
    require("esp_reset_reason" in reliability and "esp_core_dump_image_check" in reliability,
            "app_core: boot crash report (reset reason + coredump summary) missing", errors)

    if target == "esp-idf-ha":
        require("CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=65536" in defaults,
                "esp-idf-ha: DMA/internal reserve must stay at 64 KB "
                "(SDIO RX packet-buffer crash, 2026-07-11)", errors)
        ha = read(root / "waveshare" / target / "main" / "ha_client.c")
        get_states_count = len(re.findall(r'\\"type\\":\\"get_states\\"', ha))
        require(get_states_count <= 2,
                "esp-idf-ha: more than two get_states call sites; use the shared inventory cache",
                errors)
        require("request_inventory_refresh" in ha and
                "APP_CORE_INTERNAL_NETWORK_MIN_FREE" in ha,
                "esp-idf-ha: inventory snapshots are not memory-gated", errors)
        require("#define RX_MAX_CAP (768 * 1024)" in ha,
                "esp-idf-ha: websocket cap changed; re-budget PSRAM and transport bursts first",
                errors)
        require("media_player_is_spotify_account" in ha and
                "supported_features\", &supported" in ha,
                "esp-idf-ha: renamed HA Spotify account entities are not detected", errors)
        for option in (
            "CONFIG_MDNS_TASK_CREATE_FROM_SPIRAM=y",
            "CONFIG_MDNS_MEMORY_ALLOC_SPIRAM=y",
        ):
            require(option in defaults,
                    f"esp-idf-ha: mDNS must not consume the ESP-Hosted internal reserve: {option}",
                    errors)
        for option in (
            "CONFIG_SENDSPIN_ENABLE_PLAYER=y",
            "CONFIG_SENDSPIN_ENABLE_CONTROLLER=n",
            "CONFIG_SENDSPIN_ENABLE_METADATA=n",
            "CONFIG_SENDSPIN_ENABLE_ARTWORK=n",
            "CONFIG_SENDSPIN_ENABLE_VISUALIZER=n",
            "CONFIG_SENDSPIN_ENABLE_COLOR=n",
        ):
            require(option in defaults, f"esp-idf-ha: required setting missing: {option}", errors)


def check_binary(root: Path, target: str, errors: list[str]) -> None:
    name = "music_controller_p4_ha.bin" if target == "esp-idf-ha" else "music_controller_p4.bin"
    binary = root / "waveshare" / target / "build" / name
    require(binary.is_file(), f"{target}: expected binary not found: {binary}", errors)
    if not binary.is_file():
        return
    used = binary.stat().st_size
    free = APP_PARTITION_BYTES - used
    free_pct = free * 100.0 / APP_PARTITION_BYTES
    require(free >= MIN_APP_FREE_BYTES and free_pct >= MIN_APP_FREE_PERCENT,
            f"{target}: app headroom {free} B ({free_pct:.1f}%) is below "
            f"{MIN_APP_FREE_BYTES} B/{MIN_APP_FREE_PERCENT:.0f}%", errors)
    print(f"OK {target}: binary={used} B, app headroom={free} B ({free_pct:.1f}%)")

    bootloader = root / "waveshare" / target / "build" / "bootloader" / "bootloader.bin"
    require(bootloader.is_file(), f"{target}: bootloader binary not found: {bootloader}", errors)
    if bootloader.is_file():
        boot_used = bootloader.stat().st_size
        boot_free = BOOTLOADER_PARTITION_BYTES - boot_used
        require(boot_free >= MIN_BOOTLOADER_FREE_BYTES,
                f"{target}: bootloader headroom {boot_free} B is below "
                f"{MIN_BOOTLOADER_FREE_BYTES} B", errors)
        print(f"OK {target}: bootloader={boot_used} B, headroom={boot_free} B")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("target", choices=("esp-idf", "esp-idf-ha", "both"))
    parser.add_argument("--post-build", action="store_true")
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()

    targets = ("esp-idf", "esp-idf-ha") if args.target == "both" else (args.target,)
    errors: list[str] = []
    for target in targets:
        check_source(args.root, target, errors)
        if args.post_build:
            check_binary(args.root, target, errors)

    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1
    if not args.post_build:
        print("OK: P4 reliability source/configuration gates passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
