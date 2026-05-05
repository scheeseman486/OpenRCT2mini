#!/usr/bin/env python3
# OPENRCT2MINI cut 51: convert paletted PNG cursors into the X-style 2-colour
# bitmap format CursorData.cpp's cursorFromBitMap() takes.
#
# Mapping rules (based on RCT2 master palette):
#   * Index 0                    -> transparent (' ')
#   * Pixel index value <= 16    -> black ('X')
#       (Indices 10..16 are the dark half of the standard greyscale ramp:
#        pi10 'Black 0-dark' through pi16 mid-grey.)
#   * Pixel index value >= 17    -> white ('.')
#       (pi17 'White dark' through pi21 'White lighter'.)
#
# Pixels at any non-zero index outside 10..21 are still classified by the
# threshold above (luminance bucket) — since OpenRCT2's cursorFromBitMap is
# strictly 2-colour + mask, any custom shading collapses to black/white.
#
# Hot-spot: read from the PNG's `info['hotspot']` if present (PIL convention
# we set when generating cursors) — otherwise default (0, 0). Pass an
# explicit --hotspot X,Y if the PNG doesn't carry one.
#
# Usage:
#   python3 png_to_cursordata.py orct2mini_cursor_pointer.png \
#       --name kArrowCursorData --hotspot 0,0
#
# Output goes to stdout — paste it into src/openrct2-ui/CursorData.cpp.

import argparse
import os
import sys
from PIL import Image


THRESHOLD = 16  # indices <= this are black, > this are white (when not transparent)


def png_to_cursor_chars(path: str) -> str:
    img = Image.open(path)
    if img.mode != "P":
        sys.exit(f"{path}: PNG must be palette mode, got {img.mode}")
    if img.size != (32, 32):
        sys.exit(f"{path}: cursor must be 32x32, got {img.size}")
    px = img.load()
    rows = []
    for y in range(32):
        chars = []
        for x in range(32):
            idx = px[x, y]
            if idx == 0:
                chars.append(" ")
            elif idx <= THRESHOLD:
                chars.append("X")
            else:
                chars.append(".")
        rows.append("".join(chars))
    return rows


def parse_hotspot(s: str):
    a, b = s.split(",", 1)
    return int(a.strip()), int(b.strip())


def png_to_paletted_bytes(path: str) -> list:
    """Return a 1024-byte list of palette indices, 0 mapped to 0xFE
    (kPalettedCursorTransparent). Pixels keep their RCT2 palette index
    so the cursor renders with the full master palette in place."""
    img = Image.open(path)
    if img.mode != "P":
        sys.exit(f"{path}: PNG must be palette mode, got {img.mode}")
    if img.size != (32, 32):
        sys.exit(f"{path}: cursor must be 32x32, got {img.size}")
    px = img.load()
    out = []
    for y in range(32):
        for x in range(32):
            idx = px[x, y]
            out.append(0xFE if idx == 0 else idx)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("png", help="input 32x32 paletted PNG")
    ap.add_argument(
        "--name",
        default="kCustomCursorData",
        help="C++ identifier name (e.g. kArrowCursorData)",
    )
    ap.add_argument(
        "--hotspot",
        default=None,
        help="x,y hotspot (default: read PNG info['hotspot'] or 0,0)",
    )
    ap.add_argument(
        "--paletted",
        action="store_true",
        help="emit PalettedCursorData (full palette indices) instead of "
             "the 2-colour cursorFromBitMap form",
    )
    args = ap.parse_args()

    if not os.path.isfile(args.png):
        sys.exit(f"no such file: {args.png}")

    # Resolve hotspot once.
    if args.hotspot:
        hx, hy = parse_hotspot(args.hotspot)
    else:
        info_hs = Image.open(args.png).info.get("hotspot")
        hx, hy = info_hs if info_hs else (0, 0)

    if args.paletted:
        bytes_ = png_to_paletted_bytes(args.png)
        # 32 bytes per output row for readability.
        print(f"    static constexpr PalettedCursorData {args.name} = {{")
        print(f"        {{ {hx}, {hy} }},")
        print(f"        {{")
        for y in range(32):
            row = bytes_[y * 32 : (y + 1) * 32]
            cells = ", ".join(f"0x{b:02X}" for b in row)
            print(f"            {cells},")
        print(f"        }},")
        print(f"    }};")
        return

    rows = png_to_cursor_chars(args.png)

    # Emit the cursorFromBitMap() literal.
    print(
        f"    static constexpr CursorData {args.name} = cursorFromBitMap(\n"
        f"        {hx}, {hy},"
    )
    for r in rows:
        # Escape any embedded quotes (shouldn't happen — chars are space/./X).
        print(f'        "{r}"')
    print("    );")


if __name__ == "__main__":
    main()
