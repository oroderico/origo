#!/usr/bin/env python3
"""Draw assets/screens.png, the screens the README shows, from the app itself.

Each screen is the last frame origo-replay draws for a script of key presses,
so the picture is what the firmware's own renderer puts on the display, not a
mock-up of it. Run by hand when a screen changes, after building the host
targets (docs/build.md). Standard library only, like the rest of tools/.

    python3 tools/make_screens.py [path/to/origo-replay]
"""

import argparse
from pathlib import Path
import struct
import subprocess
import tempfile
import zlib

ROOT = Path(__file__).parents[1]
OUTPUT = ROOT / "assets/screens.png"
# Pixels per display pixel, screens per row, and the border around each.
SCALE = 2
COLUMNS = 3
GAP = 12
GAP_COLOUR = b"\x2b\x2b\x2b"

# p = left button, n = right button, s = both, t = timeout (origo-replay's
# keys). The wallet is BIP39's test mnemonic, eleven "abandon" and "about"
# (fingerprint 73c5da0a), made by Complete checksum: past the splash, New
# Seed, Complete checksum, 11 words + 7 coins, Enter word numbers; word number
# 0001 eleven times, each four digits taken as offered and then confirmed;
# seven tails; the mnemonic's two pages, then its confirm arrow.
WALLET = "t" + "snss" + "ns" + "sssss" * 11 + "n" * 7 + "nns"
SCREENS = (
    ("wallet", WALLET),
    # A D6 word's six rolls, 4, 2, 1, 3, 3, 2, at its confirmation: New Seed, From
    # entropy, D6 dice, 12 words, Start, then each face raised from 1.
    ("dice", "tsssss" + "ppps" + "ps" + "s" + "pps" + "pps" + "ps"),
    ("keyboard", "tnsss"),
    ("stackbit", WALLET + "snnss"),
    ("account key QR", WALLET + "nssnnsns"),
    ("addresses", WALLET + "nnnss"),
)


def last_frame(replay, keys):
    """(width, height, RGB bytes) of the last frame the script shows."""
    with tempfile.TemporaryDirectory() as frames:
        subprocess.run([str(replay), keys, frames], check=True, stdout=subprocess.DEVNULL)
        header, size, _, pixels = sorted(Path(frames).glob("*.ppm"))[-1].read_bytes().split(b"\n", 3)
    if header != b"P6":
        raise SystemExit(f"{replay}: not a binary PPM")
    width, height = map(int, size.split())
    return width, height, pixels


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("replay", nargs="?", type=Path, default=ROOT / "build-host/origo-replay")
    args = parser.parse_args()

    frames = [last_frame(args.replay, keys) for _, keys in SCREENS]
    width, height = frames[0][0], frames[0][1]
    rows = (len(frames) + COLUMNS - 1) // COLUMNS
    cell_width, cell_height = width * SCALE + GAP, height * SCALE + GAP
    canvas_width, canvas_height = COLUMNS * cell_width + GAP, rows * cell_height + GAP
    canvas = bytearray(GAP_COLOUR * (canvas_width * canvas_height))
    for k, (_, _, pixels) in enumerate(frames):
        left = GAP + (k % COLUMNS) * cell_width
        top = GAP + (k // COLUMNS) * cell_height
        for y in range(height):
            row = b"".join(pixels[(y * width + x) * 3 : (y * width + x) * 3 + 3] * SCALE for x in range(width))
            for dy in range(SCALE):
                start = ((top + y * SCALE + dy) * canvas_width + left) * 3
                canvas[start : start + len(row)] = row

    raw = b"".join(b"\0" + canvas[y * canvas_width * 3 : (y + 1) * canvas_width * 3] for y in range(canvas_height))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    OUTPUT.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", canvas_width, canvas_height, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )
    print(f"{OUTPUT.relative_to(ROOT)}: {', '.join(name for name, _ in SCREENS)}")


if __name__ == "__main__":
    main()
