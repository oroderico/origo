#!/usr/bin/env python3
"""Audit that a Seed Tool build contains only its intended runtime surface."""

import argparse
from pathlib import Path
import subprocess
import sys


FORBIDDEN_SYMBOLS = {
    "esp_wifi_init",
    "esp_wifi_start",
    "nimble_port_init",
    "nvs_flash_init",
    "esp_ota_begin",
    "serial_init",
    "jade_process_init",
    "keychain_init_cache",
    "keychain_get",
    "wallet_get",
    "adc_oneshot_new_unit",
    "ledc_timer_config",
    "deflate_init_write_compressed",
    "wally_ecdh",
}
FORBIDDEN_PREFIXES = (
    "wally_asset_",
    "wally_confidential_",
    "wally_psbt_",
    "wally_tx_",
    "secp256k1_rangeproof_",
    "secp256k1_surjectionproof_",
    "secp256k1_whitelist_",
    "power_get_battery",
)
FORBIDDEN_MAP_TEXT = (
    "managed_components/",
    "/jade/main/",
    "libesp32_deflate.a",
    "libesp_adc.a",
    "libesp_driver_ledc.a",
    "libesp_wifi.a",
    "libbt.a",
    "libnvs_flash.a",
    "BigFont.c.obj",
    "DejaVuSans",
    "jade_symbols_",
)
REQUIRED = {
    "app_main",
    "seedtool_generate",
    "seedtool_complete_checksum",
    "seedtool_mainnet_address",
    "seedtool_account_xpub",
    "seedtool_words_with_prefix",
    "seedtool_next_letters",
    "seedtool_render_keyboard",
    "seedtool_display_init",
    "wally_secp_randomize",
    "esp_fill_random",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("--map", dest="map_file", type=Path)
    parser.add_argument("--bin", dest="bin_file", type=Path)
    parser.add_argument("--max-bin-size", type=int, default=285 * 1024)
    parser.add_argument("--nm", default="xtensa-esp32-elf-nm")
    args = parser.parse_args()

    output = subprocess.run(
        [args.nm, "--defined-only", str(args.elf)], check=True, text=True, capture_output=True
    ).stdout
    symbols = {line.split()[-1] for line in output.splitlines() if line.split()}
    bad = sorted(
        symbol
        for symbol in symbols
        if symbol in FORBIDDEN_SYMBOLS or symbol.startswith(FORBIDDEN_PREFIXES)
    )
    missing = sorted(REQUIRED - symbols)

    map_file = args.map_file or args.elf.with_suffix(".map")
    bad_map = []
    if not map_file.is_file():
        bad_map.append(f"missing map file: {map_file}")
    else:
        map_text = map_file.read_text(errors="replace")
        # Only archive members pulled to satisfy live references are relevant;
        # linker command groups and discarded sections mention unused archives.
        linked_members = map_text.split("Discarded input sections", 1)[0]
        bad_map.extend(item for item in FORBIDDEN_MAP_TEXT if item in linked_members)

    bin_file = args.bin_file or args.elf.with_suffix(".bin")
    size_error = None
    if not bin_file.is_file():
        size_error = f"missing binary: {bin_file}"
    elif bin_file.stat().st_size > args.max_bin_size:
        size_error = f"binary is {bin_file.stat().st_size} bytes; limit is {args.max_bin_size}"

    if bad or missing or bad_map or size_error:
        if bad:
            print("forbidden linked symbols:", ", ".join(bad), file=sys.stderr)
        if missing:
            print("required symbols missing:", ", ".join(missing), file=sys.stderr)
        if bad_map:
            print("forbidden/missing map entries:", ", ".join(bad_map), file=sys.stderr)
        if size_error:
            print(size_error, file=sys.stderr)
        return 1

    print(
        f"audit OK: {bin_file.stat().st_size} bytes; no wallet, radio, storage, OTA update, "
        "battery, generic graphics, transaction, PSBT or Elements surface"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
