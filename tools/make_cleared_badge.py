#!/usr/bin/env python3
"""Produce assets/badges/cleared_badge.png - the "cleared" map-marker badge.

The worldmap marker (sprite 174) draws the icon (sprite 171) + a "cleared"
badge (sprite 173 -> shape 172), shown when a boss/invader is defeated. The
vanilla game's native badge is a small gold dot placed far up-right of the
icon; ERR ships a custom green-checkmark texture that overlaps the icon. For
our non-ERR builds we use OUR OWN art (assets/badges/mark2.png) embedded into
the gfx as a raster (DefineBitsLossless2).

WHY A RASTER, not a vector: FFDEC can't import detailed SVGs as Scaleform
shapes - clip-paths/gradients garble, and a shape crashes the map once it
exceeds ~30-45 filled figures. A bitmap has no such limit, embeds self-
contained in the gfx (no atlas/extra files), and renders fine in-game
(verified). See [[reference_cleared_badge]].

This tool just DOWNSCALES the source art to the badge size and writes the
committed PNG. build_vanilla_gfx.py then embeds that PNG via
`ffdec -replace ... 172 cleared_badge.png` (which centres it at ±257 = 26px).
The downscale is a one-time step - run this only when the source art changes;
the regular build (build.bat) consumes the committed cleared_badge.png as-is.

Run:  py tools/make_cleared_badge.py
Output (committed): assets/badges/cleared_badge.png
"""
import sys

from PIL import Image

import config

SRC = config.PROJECT_DIR / "assets" / "badges" / "mark2.png"
OUT = config.PROJECT_DIR / "assets" / "badges" / "cleared_badge.png"
SIZE = 104   # px; the badge displays ~26px in-game, 4x gives crispness headroom
             # while keeping the embedded bitmap (and gfx) small.


def main():
    if not SRC.exists():
        sys.exit(f"source art not found: {SRC}")
    im = Image.open(SRC).convert("RGBA")
    src_size = im.size
    im = im.resize((SIZE, SIZE), Image.LANCZOS)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    im.save(OUT)
    print(f"wrote {OUT} ({OUT.stat().st_size} bytes, {src_size[0]}x{src_size[1]} -> {SIZE}x{SIZE})")


if __name__ == "__main__":
    main()
