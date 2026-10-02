#!/usr/bin/env python3
"""Packages LeviFrameGen into a validated LeviLaunchroid .levipack archive.

Usage:
    python3 scripts/package_levipack.py --library build/libframegen.so \
        --icon assets/icon.png --output FrameGen.levipack
"""

import argparse
import json
import sys
import zipfile
from pathlib import Path

MOD_ID = "leviframegen"
MOD_NAME = "LeviFrameGen"
MOD_AUTHOR = "ballrightmohab-png"
MOD_VERSION = "2.6.0"
MOD_DESCRIPTION = (
    "Gen-before-Real Frame Generation (2x/3x/4x) Fill-to-Cap + "
    "3D Perspective Camera Smoothing & HUD Optimizer for Minecraft Bedrock "
    "(LeviLaunchroid). Honest FPS, anti-ghosting, CAS sharpening & zero-dimming."
)
ENTRY = "libframegen.so"


def build_manifest(has_icon: bool) -> dict:
    return {
        "schema_version": 1,
        "id": MOD_ID,
        "name": MOD_NAME,
        "author": MOD_AUTHOR,
        "version": MOD_VERSION,
        "description": MOD_DESCRIPTION,
        "type": "preload-native",
        "entry": ENTRY,
        "minecraft_versions": [],
        "icon": "icon.png" if has_icon else "",
        "overwrite_files": [
            "manifest.json",
            "levimod.json",
            "libframegen.so",
            "icon.png",
            "config/config.json",
        ],
        "overwrite_folders": [
            "config",
        ],
    }


def write_package(library: Path, icon: Path | None, output: Path) -> None:
    if not library.is_file():
        raise FileNotFoundError(f"Shared library not found: {library}")

    has_icon = bool(icon and icon.is_file())
    manifest = build_manifest(has_icon)

    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists():
        output.unlink()

    with zipfile.ZipFile(
        output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9
    ) as archive:
        archive.writestr(
            "manifest.json",
            json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
        )
        archive.write(library, ENTRY)
        if has_icon and icon is not None:
            archive.write(icon, "icon.png")

    print(f"Packaged {output.resolve()} ({output.stat().st_size} bytes)")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--library", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--icon", type=Path, default=None)
    args = parser.parse_args()
    try:
        write_package(args.library.resolve(), args.icon, args.output.resolve())
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
