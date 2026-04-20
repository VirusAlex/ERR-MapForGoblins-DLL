#!/usr/bin/env python3
"""
Render composed map icon previews from GFX XML.

Parses worldmap GFX XML (via FFDEC swf2xml), reads sprite 171 frame data,
loads TGA textures, applies scale/translate/colorTransform, and composites
final icon PNGs.

Prerequisites:
  - Run FFDEC: swf2xml input.gfx output.xml
  - Extract TGA textures next to GFX (use extract_subtextures.py)

Output: assets/map_icons/composed/iconId_XXX.png
"""

import xml.etree.ElementTree as ET
from PIL import Image
import os
import sys
import argparse


def render_icons(xml_path, tga_dir, out_dir, frame_range=None):
    tree = ET.parse(xml_path)
    root = tree.getroot()

    # Build charId -> exportName mapping
    char_names = {}
    for item in root.iter("item"):
        if item.get("type") == "DefineExternalImage2":
            cid = int(item.get("characterID", 0))
            char_names[cid] = item.get("exportName", "")

    # Find sprite 171 (icon sprite with ~397 frames)
    for item in root.iter("item"):
        if item.get("type") != "DefineSpriteTag":
            continue
        if item.get("spriteId") != "171":
            continue

        subtags = item.find("subTags")
        frame = 1
        current = {}
        frames = {}

        for tag in subtags:
            tt = tag.get("type", "")
            if tt == "ShowFrameTag":
                frames[frame] = dict(current)
                frame += 1
            elif tt in ("PlaceObject2Tag", "PlaceObject3Tag"):
                depth = int(tag.get("depth", 0))
                cid = int(tag.get("characterId", 0))
                matrix = tag.find("matrix")
                sx = float(matrix.get("scaleX", 1)) if matrix is not None else 1.0
                sy = float(matrix.get("scaleY", 1)) if matrix is not None else 1.0
                tx = float(matrix.get("translateX", 0)) / 20.0 if matrix is not None else 0
                ty = float(matrix.get("translateY", 0)) / 20.0 if matrix is not None else 0

                ct = tag.find("colorTransform")
                ra = int(ct.get("redAddTerm", 0)) if ct is not None else 0
                ga = int(ct.get("greenAddTerm", 0)) if ct is not None else 0
                ba = int(ct.get("blueAddTerm", 0)) if ct is not None else 0
                rm = int(ct.get("redMultTerm", 256)) if ct is not None else 256
                gm = int(ct.get("greenMultTerm", 256)) if ct is not None else 256
                bm = int(ct.get("blueMultTerm", 256)) if ct is not None else 256
                am = int(ct.get("alphaMultTerm", 256)) if ct is not None else 256
                aa = int(ct.get("alphaAddTerm", 0)) if ct is not None else 0

                current[depth] = {
                    "cid": cid, "sx": sx, "sy": sy, "tx": tx, "ty": ty,
                    "ra": ra, "ga": ga, "ba": ba, "aa": aa,
                    "rm": rm, "gm": gm, "bm": bm, "am": am,
                }
            elif tt == "RemoveObject2Tag":
                depth = int(tag.get("depth", 0))
                current.pop(depth, None)

        os.makedirs(out_dir, exist_ok=True)
        CANVAS = 128
        CX, CY = CANVAS // 2, CANVAS // 2

        if frame_range is None:
            frame_range = range(1, frame)

        rendered = 0
        for fnum in frame_range:
            if fnum not in frames:
                continue
            layers = frames[fnum]
            canvas = Image.new("RGBA", (CANVAS, CANVAS), (0, 0, 0, 0))

            for depth in sorted(layers.keys()):
                info = layers[depth]
                name = char_names.get(info["cid"], "")
                tga_path = os.path.join(tga_dir, f"{name}.tga")
                if not os.path.exists(tga_path):
                    continue

                layer = Image.open(tga_path).convert("RGBA")
                nw = max(1, int(layer.width * info["sx"]))
                nh = max(1, int(layer.height * info["sy"]))
                layer = layer.resize((nw, nh), Image.LANCZOS)

                r, g, b, a = layer.split()
                r = r.point(lambda p: min(255, max(0, int(p * info["rm"] / 256 + info["ra"]))))
                g = g.point(lambda p: min(255, max(0, int(p * info["gm"] / 256 + info["ga"]))))
                b = b.point(lambda p: min(255, max(0, int(p * info["bm"] / 256 + info["ba"]))))
                a = a.point(lambda p: min(255, max(0, int(p * info["am"] / 256 + info["aa"]))))
                layer = Image.merge("RGBA", (r, g, b, a))

                canvas.paste(layer, (int(CX + info["tx"]), int(CY + info["ty"])), layer)

            canvas.save(os.path.join(out_dir, f"iconId_{fnum}.png"))
            rendered += 1

        print(f"Rendered {rendered} icons to {out_dir}")
        break


def main():
    parser = argparse.ArgumentParser(description="Render map icon previews from GFX XML")
    parser.add_argument("--xml", type=str, default=None, help="Path to GFX XML (from FFDEC swf2xml)")
    parser.add_argument("--tga-dir", type=str, default=None, help="Directory with TGA textures")
    parser.add_argument("--out-dir", type=str, default=None, help="Output directory for PNGs")
    parser.add_argument("--frames", type=str, default="350-397", help="Frame range (e.g. 350-397)")
    args = parser.parse_args()

    project_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    xml_path = args.xml or os.path.join(project_dir, "assets", "map_icons", "worldmap_new_verify.xml")
    tga_dir = args.tga_dir or os.path.join(project_dir, "assets", "menu")
    out_dir = args.out_dir or os.path.join(project_dir, "assets", "map_icons", "composed")

    # Parse frame range
    if "-" in args.frames:
        start, end = args.frames.split("-")
        frame_range = range(int(start), int(end) + 1)
    else:
        frame_range = [int(x) for x in args.frames.split(",")]

    render_icons(xml_path, tga_dir, out_dir, frame_range)


if __name__ == "__main__":
    main()
