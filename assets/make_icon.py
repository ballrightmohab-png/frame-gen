#!/usr/bin/env python3
"""Generates a crisp 64x64 RGBA PNG icon for LeviFrameGen (FrameGen.levipack)."""

import math
import struct
import zlib
from pathlib import Path


def make_png(width: int, height: int, rgba_bytes: bytes) -> bytes:
    def chunk(tag: bytes, data: bytes) -> bytes:
        crc = zlib.crc32(tag + data) & 0xFFFFFFFF
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", crc)

    raw_rows = bytearray()
    stride = width * 4
    for y in range(height):
        raw_rows.append(0)  # Filter type 0 (None)
        raw_rows.extend(rgba_bytes[y * stride : (y + 1) * stride])

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    idat = zlib.compress(bytes(raw_rows), level=9)
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", ihdr)
        + chunk(b"IDAT", idat)
        + chunk(b"IEND", b"")
    )


def render_icon(size: int = 64) -> bytes:
    buf = bytearray(size * size * 4)

    for y in range(size):
        for x in range(size):
            idx = (y * size + x) * 4
            cx = x - (size - 1) * 0.5
            cy = y - (size - 1) * 0.5

            # Rounded dark slate card background
            rx = max(0.0, abs(cx) - 18.0)
            ry = max(0.0, abs(cy) - 18.0)
            dist_corner = math.hypot(rx, ry)
            if dist_corner > 12.5:
                buf[idx : idx + 4] = bytes((0, 0, 0, 0))
                continue

            # Dark gradient background
            t = y / float(size - 1)
            r = int(14 + 10 * (1.0 - t))
            g = int(22 + 18 * (1.0 - t))
            b = int(32 + 24 * (1.0 - t))
            a = 255

            # Glowing cyan-emerald border
            if 10.2 <= dist_corner <= 12.5:
                r, g, b = 56, 232, 176

            # Overlapping motion-interpolated frame layers
            # Back frame outline (left-shifted)
            if 13 <= x <= 33 and 18 <= y <= 44:
                if x in (13, 33) or y in (18, 44):
                    r, g, b = 42, 145, 130
            # Middle synthesized frame (cyan glow)
            if 21 <= x <= 41 and 15 <= y <= 41:
                if x in (21, 41) or y in (15, 41):
                    r, g, b = 56, 232, 176
            # Front target frame (bright mint fill + outline)
            if 29 <= x <= 49 and 12 <= y <= 38:
                if x in (29, 49) or y in (12, 38):
                    r, g, b = 120, 255, 214
                else:
                    r = min(255, r + 22)
                    g = min(255, g + 65)
                    b = min(255, b + 52)

            # Smooth camera wave at bottom (y ~ 48..52)
            wave_y = 48.5 - 2.5 * math.sin((x - 14) * 0.18)
            if 14 <= x <= 50 and abs(y - wave_y) <= 1.35:
                r, g, b = 88, 242, 194

            buf[idx] = r
            buf[idx + 1] = g
            buf[idx + 2] = b
            buf[idx + 3] = a

    return make_png(size, size, bytes(buf))


def main() -> None:
    out_path = Path(__file__).resolve().parent / "icon.png"
    out_path.write_bytes(render_icon(64))
    print(f"Generated {out_path} ({out_path.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
