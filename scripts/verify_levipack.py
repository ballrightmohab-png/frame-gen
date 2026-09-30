#!/usr/bin/env python3
"""Validates that FrameGen.levipack satisfies LeviLaunchroid's mod package contract."""

import argparse
import json
import struct
import sys
import zipfile
from pathlib import Path

EXPECTED_ENTRY = "libframegen.so"
ELF_MAGIC = b"\x7fELF"
ELFCLASS64 = 2
ET_DYN = 3
EM_AARCH64 = 183
EM_X86_64 = 62


def verify_levipack(path: Path, allow_host_arch: bool = False) -> int:
    if not path.is_file():
        print(f"error: file does not exist: {path}", file=sys.stderr)
        return 1

    with zipfile.ZipFile(path, "r") as archive:
        names = archive.namelist()
        name_set = set(names)

        if "manifest.json" not in name_set:
            print("error: missing manifest.json", file=sys.stderr)
            return 1

        so_entries = [n for n in names if n.lower().endswith(".so")]
        # LeviLaunchroid FileHandler.isValidZipModPackage strictly requires
        # soEntries.size() == 1!
        if len(so_entries) != 1:
            print(
                f"error: LeviLaunchroid requires exactly 1 .so entry, found {so_entries}",
                file=sys.stderr,
            )
            return 1

        if so_entries[0] != EXPECTED_ENTRY:
            print(
                f"error: expected {EXPECTED_ENTRY}, found {so_entries[0]}",
                file=sys.stderr,
            )
            return 1

        manifest = json.loads(archive.read("manifest.json").decode("utf-8"))
        required_fields = [
            "id",
            "name",
            "author",
            "version",
            "description",
            "type",
            "entry",
            "minecraft_versions",
        ]
        for field in required_fields:
            if field not in manifest:
                print(f"error: manifest missing field '{field}'", file=sys.stderr)
                return 1

        if manifest.get("type") != "preload-native":
            print(
                f"error: manifest type must be 'preload-native', got {manifest.get('type')}",
                file=sys.stderr,
            )
            return 1

        if manifest.get("entry") != EXPECTED_ENTRY:
            print(
                f"error: manifest entry mismatch: {manifest.get('entry')}",
                file=sys.stderr,
            )
            return 1

        icon_rel = manifest.get("icon", "")
        if icon_rel and icon_rel not in name_set:
            print(
                f"error: manifest references icon '{icon_rel}' which is missing from archive",
                file=sys.stderr,
            )
            return 1

        so_bytes = archive.read(EXPECTED_ENTRY)
        if len(so_bytes) < 64 or so_bytes[:4] != ELF_MAGIC:
            print("error: libframegen.so is not a valid ELF binary", file=sys.stderr)
            return 1

        ei_class = so_bytes[4]
        e_type, e_machine = struct.unpack_from("<HH", so_bytes, 16)
        if ei_class != ELFCLASS64 or e_type != ET_DYN:
            print(
                f"error: expected 64-bit ET_DYN shared library (class={ei_class}, type={e_type})",
                file=sys.stderr,
            )
            return 1

        allowed_machines = (EM_AARCH64, EM_X86_64) if allow_host_arch else (EM_AARCH64,)
        if e_machine not in allowed_machines:
            print(
                f"error: unexpected ELF machine {e_machine} (expected EM_AARCH64={EM_AARCH64})",
                file=sys.stderr,
            )
            return 1

        if b"PLGetModRegistration" not in so_bytes:
            print(
                "error: libframegen.so does not export PLGetModRegistration",
                file=sys.stderr,
            )
            return 1

        if b"libpreloader.so" not in so_bytes:
            print(
                "error: libframegen.so is missing DT_NEEDED libpreloader.so",
                file=sys.stderr,
            )
            return 1

    arch_str = "AArch64 (arm64-v8a)" if e_machine == EM_AARCH64 else f"machine={e_machine}"
    print(f"OK: {path} verified ({len(so_bytes)} bytes {EXPECTED_ENTRY}, {arch_str})")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("levipack", type=Path)
    parser.add_argument("--allow-host-arch", action="store_true")
    args = parser.parse_args()
    return verify_levipack(args.levipack, allow_host_arch=args.allow_host_arch)


if __name__ == "__main__":
    raise SystemExit(main())
