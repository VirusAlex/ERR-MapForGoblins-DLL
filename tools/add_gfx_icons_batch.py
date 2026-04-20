#!/usr/bin/env python3
"""
Batch-add tinted icon variants to 02_120_worldmap_new.gfx.

Reads tools/icon_tints_config.json, adds each new iconId as a frame
on sprite 171, then updates frameCount accordingly.

Workflow:
  1. FFDEC: decompile current GFX → XML
  2. This script: add all frames to XML
  3. FFDEC: recompile XML → GFX

Usage:
  py add_gfx_icons_batch.py [--xml in.xml] [--output out.xml]
"""
import xml.etree.ElementTree as ET
import argparse
import json
import sys, io
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
from pathlib import Path


def _set_place_object(po, char_id, scale, tx, ty,
                      r=0, g=0, b=0, alpha=256):
    """Configure a PlaceObject3Tag element as a layer."""
    po.set("type", "PlaceObject3Tag")
    po.set("characterId", str(char_id))
    for flag in ("placeFlagHasCharacter", "placeFlagHasMatrix",
                 "placeFlagHasColorTransform", "placeFlagHasImage"):
        po.set(flag, "true")
    for flag in ("placeFlagMove", "placeFlagHasClipDepth", "placeFlagHasName",
                 "placeFlagHasRatio", "placeFlagHasFilterList", "placeFlagHasBlendMode",
                 "placeFlagHasCacheAsBitmap", "placeFlagHasClassName",
                 "placeFlagHasClipActions", "placeFlagHasVisible",
                 "placeFlagOpaqueBackground", "reserved"):
        po.set(flag, "false")
    po.set("clipDepth", "0")
    po.set("ratio", "0")
    po.set("bitmapCache", "0")
    po.set("blendMode", "0")
    po.set("forceWriteAsLong", "false")
    po.set("visible", "0")

    m = ET.SubElement(po, "matrix")
    m.set("type", "MATRIX")
    m.set("hasScale", "true")
    m.set("hasRotate", "false")
    m.set("scaleX", str(scale))
    m.set("scaleY", str(scale))
    m.set("rotateSkew0", "0.0")
    m.set("rotateSkew1", "0.0")
    m.set("translateX", str(tx))
    m.set("translateY", str(ty))
    m.set("nScaleBits", "17")
    m.set("nRotateBits", "0")
    m.set("nTranslateBits", "11")

    ct = ET.SubElement(po, "colorTransform")
    ct.set("type", "CXFORMWITHALPHA")
    ct.set("hasMultTerms", "true")
    ct.set("hasAddTerms", "true")
    ct.set("redMultTerm", "256")
    ct.set("greenMultTerm", "256")
    ct.set("blueMultTerm", "256")
    ct.set("alphaMultTerm", str(alpha))
    ct.set("redAddTerm", str(r))
    ct.set("greenAddTerm", str(g))
    ct.set("blueAddTerm", str(b))
    ct.set("alphaAddTerm", "0")
    ct.set("nbits", "9")


def add_frames(xml_path, output_path, config):
    tree = ET.parse(xml_path)
    root = tree.getroot()

    families = config["_base_families"]

    for item in root.iter("item"):
        if item.get("type") != "DefineSpriteTag" or item.get("spriteId") != "171":
            continue

        subtags = item.find("subTags")
        current_count = int(item.get("frameCount", "0"))
        added = 0

        # Skip specs whose iconId is <= current_count (already present)
        pending = [s for s in config["icons"] if s["iconId"] > current_count]
        if pending:
            pending.sort(key=lambda s: s["iconId"])
            if pending[0]["iconId"] != current_count + 1:
                raise RuntimeError(
                    f"First pending iconId {pending[0]['iconId']} not contiguous "
                    f"with current {current_count}.")

        expected_next = current_count + 1
        for spec in pending:
            iid = spec["iconId"]
            if iid != expected_next:
                raise RuntimeError(
                    f"iconId {iid} not contiguous (expected {expected_next}).")

            fam = families[spec["family"]]
            single = fam.get("single", False)

            # RemoveObject2 for depths 1 and 2 (clear previous frame state)
            for depth in (1, 2):
                rm = ET.SubElement(subtags, "item")
                rm.set("type", "RemoveObject2Tag")
                rm.set("depth", str(depth))
                rm.set("forceWriteAsLong", "false")

            if not single:
                # Background at depth=1
                po_bg = ET.SubElement(subtags, "item")
                _set_place_object(
                    po_bg, fam["bg_char"], fam["bg_scale"],
                    fam["bg_tx"], fam["bg_ty"],
                    r=0, g=0, b=0, alpha=fam["bg_alpha"])
                po_bg.set("depth", "1")

            # Overlay / sole icon at depth=2
            po_fg = ET.SubElement(subtags, "item")
            _set_place_object(
                po_fg, fam["icon_char"], fam["icon_scale"],
                fam["icon_tx"], fam["icon_ty"],
                r=spec["r"], g=spec["g"], b=spec["b"], alpha=256)
            po_fg.set("depth", "2")

            # ShowFrameTag
            sf = ET.SubElement(subtags, "item")
            sf.set("type", "ShowFrameTag")
            sf.set("forceWriteAsLong", "false")

            added += 1
            expected_next += 1
            print(f"  +{iid:<4} ({spec['family']}) → {spec['category']}")

        new_count = current_count + added
        item.set("frameCount", str(new_count))
        print(f"\nTotal frames: {current_count} → {new_count}")
        break

    tree.write(output_path, encoding="utf-8", xml_declaration=True)
    print(f"Saved to {output_path}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--xml",
                        default="assets/map_icons/worldmap_new_actual.xml")
    parser.add_argument("--output", default=None,
                        help="Output XML (default: overwrite input)")
    parser.add_argument("--config",
                        default="tools/icon_tints_config.json")
    args = parser.parse_args()

    out = args.output or args.xml
    cfg = json.load(open(args.config, encoding='utf-8'))
    print(f"Adding {len(cfg['icons'])} tinted frames:")
    add_frames(args.xml, out, cfg)


if __name__ == "__main__":
    main()
