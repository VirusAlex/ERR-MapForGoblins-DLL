#!/usr/bin/env python3
"""
Add a new iconId frame to 02_120_worldmap.gfx.

Process:
  1. ffdec-cli -swf2xml input.gfx output.xml
  2. This script adds a new frame to sprite 171 in the XML
  3. ffdec-cli -xml2swf output.xml new.gfx

Usage:
  py add_gfx_icon.py --xml worldmap.xml --output worldmap_new.xml \
     --frame 397 \
     --bg-char 1000 --bg-scale 0.6294 --bg-tx -770 --bg-tz -770 --bg-alpha 205 \
     --icon-char 1015 --icon-scale 0.183 --icon-tx -260 --icon-tz -280 \
     --icon-red 50 --icon-green 100 --icon-blue 50

Full pipeline:
  java -jar ffdec-cli.jar -swf2xml input.gfx temp.xml
  py add_gfx_icon.py --xml temp.xml --output temp_new.xml --frame 397 ...
  java -jar ffdec-cli.jar -xml2swf temp_new.xml output.gfx
"""

import xml.etree.ElementTree as ET
import argparse
import sys


def add_frame(xml_path, output_path, frame_num,
              bg_char, bg_sx, bg_sy, bg_tx, bg_ty, bg_alpha,
              icon_char, icon_sx, icon_sy, icon_tx, icon_ty,
              icon_red, icon_green, icon_blue):
    tree = ET.parse(xml_path)
    root = tree.getroot()

    for item in root.iter("item"):
        if item.get("type") != "DefineSpriteTag":
            continue
        if item.get("spriteId") != "171":
            continue

        subtags = item.find("subTags")

        # RemoveObject2 for depths 1 and 2
        for depth in [1, 2]:
            rm = ET.SubElement(subtags, "item")
            rm.set("type", "RemoveObject2Tag")
            rm.set("depth", str(depth))
            rm.set("forceWriteAsLong", "false")

        # PlaceObject3 depth 1 (background)
        po1 = ET.SubElement(subtags, "item")
        po1.set("type", "PlaceObject3Tag")
        po1.set("depth", "1")
        po1.set("characterId", str(bg_char))
        for flag in ["placeFlagHasCharacter", "placeFlagHasMatrix",
                     "placeFlagHasColorTransform", "placeFlagHasImage"]:
            po1.set(flag, "true")
        for flag in ["placeFlagMove", "placeFlagHasClipDepth", "placeFlagHasName",
                     "placeFlagHasRatio", "placeFlagHasFilterList", "placeFlagHasBlendMode",
                     "placeFlagHasCacheAsBitmap", "placeFlagHasClassName",
                     "placeFlagHasClipActions", "placeFlagHasVisible",
                     "placeFlagOpaqueBackground", "reserved"]:
            po1.set(flag, "false")
        po1.set("clipDepth", "0")
        po1.set("ratio", "0")
        po1.set("bitmapCache", "0")
        po1.set("blendMode", "0")
        po1.set("forceWriteAsLong", "false")
        po1.set("visible", "0")

        m1 = ET.SubElement(po1, "matrix")
        m1.set("type", "MATRIX")
        m1.set("hasScale", "true")
        m1.set("hasRotate", "false")
        m1.set("scaleX", str(bg_sx))
        m1.set("scaleY", str(bg_sy))
        m1.set("rotateSkew0", "0.0")
        m1.set("rotateSkew1", "0.0")
        m1.set("translateX", str(bg_tx))
        m1.set("translateY", str(bg_ty))
        m1.set("nScaleBits", "17")
        m1.set("nRotateBits", "0")
        m1.set("nTranslateBits", "11")

        ct1 = ET.SubElement(po1, "colorTransform")
        ct1.set("type", "CXFORMWITHALPHA")
        ct1.set("hasMultTerms", "true")
        ct1.set("hasAddTerms", "true")
        ct1.set("redMultTerm", "256")
        ct1.set("greenMultTerm", "256")
        ct1.set("blueMultTerm", "256")
        ct1.set("alphaMultTerm", str(bg_alpha))
        ct1.set("redAddTerm", "0")
        ct1.set("greenAddTerm", "0")
        ct1.set("blueAddTerm", "0")
        ct1.set("alphaAddTerm", "0")
        ct1.set("nbits", "9")

        # PlaceObject3 depth 2 (overlay icon)
        po2 = ET.SubElement(subtags, "item")
        po2.set("type", "PlaceObject3Tag")
        po2.set("depth", "2")
        po2.set("characterId", str(icon_char))
        for flag in ["placeFlagHasCharacter", "placeFlagHasMatrix",
                     "placeFlagHasColorTransform", "placeFlagHasImage"]:
            po2.set(flag, "true")
        for flag in ["placeFlagMove", "placeFlagHasClipDepth", "placeFlagHasName",
                     "placeFlagHasRatio", "placeFlagHasFilterList", "placeFlagHasBlendMode",
                     "placeFlagHasCacheAsBitmap", "placeFlagHasClassName",
                     "placeFlagHasClipActions", "placeFlagHasVisible",
                     "placeFlagOpaqueBackground", "reserved"]:
            po2.set(flag, "false")
        po2.set("clipDepth", "0")
        po2.set("ratio", "0")
        po2.set("bitmapCache", "0")
        po2.set("blendMode", "0")
        po2.set("forceWriteAsLong", "false")
        po2.set("visible", "0")

        m2 = ET.SubElement(po2, "matrix")
        m2.set("type", "MATRIX")
        m2.set("hasScale", "true")
        m2.set("hasRotate", "false")
        m2.set("scaleX", str(icon_sx))
        m2.set("scaleY", str(icon_sy))
        m2.set("rotateSkew0", "0.0")
        m2.set("rotateSkew1", "0.0")
        m2.set("translateX", str(icon_tx))
        m2.set("translateY", str(icon_ty))
        m2.set("nScaleBits", "15")
        m2.set("nRotateBits", "0")
        m2.set("nTranslateBits", "10")

        ct2 = ET.SubElement(po2, "colorTransform")
        ct2.set("type", "CXFORMWITHALPHA")
        ct2.set("hasMultTerms", "false")
        ct2.set("hasAddTerms", "true")
        ct2.set("redMultTerm", "256")
        ct2.set("greenMultTerm", "256")
        ct2.set("blueMultTerm", "256")
        ct2.set("alphaMultTerm", "256")
        ct2.set("redAddTerm", str(icon_red))
        ct2.set("greenAddTerm", str(icon_green))
        ct2.set("blueAddTerm", str(icon_blue))
        ct2.set("alphaAddTerm", "0")
        ct2.set("nbits", "8")

        # ShowFrameTag
        sf = ET.SubElement(subtags, "item")
        sf.set("type", "ShowFrameTag")
        sf.set("forceWriteAsLong", "false")

        # Update frameCount
        item.set("frameCount", str(frame_num))

        print(f"Added frame {frame_num} to sprite 171")
        break

    tree.write(output_path, encoding="utf-8", xml_declaration=True)
    print(f"Saved to {output_path}")


def main():
    parser = argparse.ArgumentParser(description="Add new icon frame to GFX XML")
    parser.add_argument("--xml", required=True, help="Input XML (from FFDEC swf2xml)")
    parser.add_argument("--output", required=True, help="Output XML")
    parser.add_argument("--frame", type=int, required=True, help="New frame number (iconId)")
    parser.add_argument("--bg-char", type=int, default=1000, help="Background charId")
    parser.add_argument("--bg-scale", type=float, default=0.6294, help="Background scale")
    parser.add_argument("--bg-tx", type=int, default=-770, help="Background translateX (twips)")
    parser.add_argument("--bg-tz", type=int, default=-770, help="Background translateY (twips)")
    parser.add_argument("--bg-alpha", type=int, default=205, help="Background alphaMultTerm")
    parser.add_argument("--icon-char", type=int, default=1015, help="Icon charId")
    parser.add_argument("--icon-scale", type=float, default=0.183, help="Icon scale")
    parser.add_argument("--icon-tx", type=int, default=-260, help="Icon translateX (twips)")
    parser.add_argument("--icon-tz", type=int, default=-280, help="Icon translateY (twips)")
    parser.add_argument("--icon-red", type=int, default=50, help="Icon redAddTerm")
    parser.add_argument("--icon-green", type=int, default=100, help="Icon greenAddTerm")
    parser.add_argument("--icon-blue", type=int, default=50, help="Icon blueAddTerm")
    args = parser.parse_args()

    add_frame(args.xml, args.output, args.frame,
              args.bg_char, args.bg_scale, args.bg_scale, args.bg_tx, args.bg_tz, args.bg_alpha,
              args.icon_char, args.icon_scale, args.icon_scale, args.icon_tx, args.icon_tz,
              args.icon_red, args.icon_green, args.icon_blue)


if __name__ == "__main__":
    main()
