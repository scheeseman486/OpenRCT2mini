#!/usr/bin/env python3
"""
OPENRCT2MINI revision 63 — generate the OnionUI port-collection box art.

Composites the GPL-3 OpenRCT2 logo (resources/logo/icon_x1024.png) onto a
250x376 portrait canvas with a vertical gradient background and the
"OpenRCT2mini" wordmark below the icon. Output goes to
Packaging/miyoo_mini/box_art.png so package.sh can copy it without
needing PIL on the build host.

Reproducible from in-tree GPL-3 sources — re-run any time the layout
needs to change. No external assets required.

Usage:
    python3 resources/portbox/build_box_art.py
"""

import os
import sys
from PIL import Image, ImageDraw, ImageFont

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, "..", ".."))

LOGO_SRC = os.path.join(PROJECT_ROOT, "resources", "logo", "icon_x1024.png")
OUT_PATH = os.path.join(PROJECT_ROOT, "Packaging", "miyoo_mini", "box_art.png")

# OnionUI box-art convention. 250x376 hits the 2:3 portrait aspect that
# matches the bundled cover packs and the Imgs/ folder reads cleanly at
# this size on the Miyoo Mini panel.
W, H = 250, 376

# Background: vertical gradient. Deep evergreen at top fading to OpenRCT2
# sky-blue at the bottom — pulled from the logo's existing palette so the
# composite reads as one piece. Hex picked by eyedropping icon_x1024.
BG_TOP = (32, 92, 60)        # deep RCT-green
BG_BOTTOM = (84, 156, 222)   # sky blue


def make_gradient(width, height, top, bottom):
    """Vertical RGB gradient. Linear interpolation, no banding tricks needed
    at this resolution."""
    img = Image.new("RGB", (width, height), top)
    px = img.load()
    for y in range(height):
        t = y / max(1, height - 1)
        r = int(top[0] + (bottom[0] - top[0]) * t)
        g = int(top[1] + (bottom[1] - top[1]) * t)
        b = int(top[2] + (bottom[2] - top[2]) * t)
        for x in range(width):
            px[x, y] = (r, g, b)
    return img


def find_font(size):
    """Pick whatever sans-serif TTF the host has — DejaVuSans is on every
    Debian-derived dev box, Liberation on Fedora, etc. Fallback to PIL's
    bundled bitmap font if none found (still readable at this size)."""
    candidates = [
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
        "/usr/share/fonts/liberation/LiberationSans-Bold.ttf",
        "/Library/Fonts/Arial.ttf",
        "C:\\Windows\\Fonts\\arialbd.ttf",
    ]
    for path in candidates:
        if os.path.isfile(path):
            try:
                return ImageFont.truetype(path, size)
            except OSError:
                continue
    return ImageFont.load_default()


def draw_centred(draw, xy_centre, text, font, fill, shadow=(0, 0, 0, 180)):
    """Draw centred at (cx, cy) with a 1-px drop shadow for readability over
    the gradient. ImageDraw text anchors are PIL>=8 only; we compute manually
    for backwards compatibility with the toolchain image's PIL."""
    bbox = draw.textbbox((0, 0), text, font=font)
    tw = bbox[2] - bbox[0]
    th = bbox[3] - bbox[1]
    cx, cy = xy_centre
    x = cx - tw // 2 - bbox[0]
    y = cy - th // 2 - bbox[1]
    if shadow is not None:
        draw.text((x + 1, y + 1), text, font=font, fill=shadow)
    draw.text((x, y), text, font=font, fill=fill)


def main():
    if not os.path.isfile(LOGO_SRC):
        sys.exit(f"missing logo source: {LOGO_SRC}")

    # Background.
    bg = make_gradient(W, H, BG_TOP, BG_BOTTOM).convert("RGBA")

    # Logo. Resize to ~70% of canvas width and centre upper third.
    logo = Image.open(LOGO_SRC).convert("RGBA")
    logo_w = int(W * 0.72)
    logo_h = int(logo_w * logo.size[1] / logo.size[0])
    logo = logo.resize((logo_w, logo_h), Image.LANCZOS)
    logo_x = (W - logo_w) // 2
    logo_y = int(H * 0.10)
    bg.alpha_composite(logo, (logo_x, logo_y))

    # Wordmark + subtitle.
    title_y = logo_y + logo_h + int(H * 0.06)
    subtitle_y = title_y + int(H * 0.07)

    draw = ImageDraw.Draw(bg)
    title_font = find_font(28)
    subtitle_font = find_font(14)

    draw_centred(draw, (W // 2, title_y), "OpenRCT2mini",
                 title_font, fill=(255, 255, 255, 255))
    draw_centred(draw, (W // 2, subtitle_y), "Miyoo Mini Edition",
                 subtitle_font, fill=(220, 240, 255, 230))

    # Save.
    os.makedirs(os.path.dirname(OUT_PATH), exist_ok=True)
    bg.save(OUT_PATH, optimize=True)
    print(f"wrote {OUT_PATH} ({W}x{H})")


if __name__ == "__main__":
    main()
