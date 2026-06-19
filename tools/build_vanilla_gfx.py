#!/usr/bin/env python3
"""Build the non-ERR worldmap GFX (vanilla or convergence base) + our icons.

The shipped ERR gfx (assets/menu/02_120_worldmap_new.gfx) is built on top of
ERR's modified 02_120_worldmap.gfx, which re-icons several existing frames
(boss/camp/bounty/grace/remembrance/tower) with ERR-only textures
(MENU_MAP_ERR_*, charIds 13500-13506) that don't exist in vanilla 01_common.
Shipping it to other players would carry ERR's UI changes (and dead texture
references). This tool rebuilds the icon set on the target game's own base:

  vanilla     base: GAME_DIR/menu/02_120_worldmap.gfx (sprite 171 = 348 frames)
              out:  assets/menu/02_120_worldmap_vanilla.gfx
  convergence base: CONVERGENCE_MOD_DIR/menu/02_120_worldmap.gfx (756 frames -
              the mod adds 408 icon frames of its own, so OUR frames land at
              757+ and the bake shifts iconIds by config.ICON_FRAME_OFFSET;
              this tool verifies that constant against the actual base)
              out:  assets/menu/02_120_worldmap_convergence.gfx

  1. decompile the base gfx and our _new.gfx (FFDEC swf2xml),
  2. copy our added DefineExternalImage2 defs (charIds 1000-1024 - all
     vanilla texture names: MENU_Tab_*, MENU_ItemIcon_*, ...) into the
     base XML before sprite 171 (verifying the base doesn't already use
     those charIds),
  3. append our added frames (349..end of OUR gfx; every frame is a
     self-contained RemoveObject2+PlaceObject3+ShowFrame group) to the base
     sprite 171 and bump its frameCount,
  4. compile back (FFDEC xml2swf).

Verifies that no appended frame references an ERR-only charId.

FFDEC: set the FFDEC_CLI env var (e.g. 'java -jar C:/.../ffdec-cli.jar'),
otherwise the known local install next to the project is used.

Usage: py build_vanilla_gfx.py [--profile vanilla|convergence]
       (default: the active MFG_PROFILE if non-ERR, else vanilla)
"""
import os
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

import config

PROJECT = config.PROJECT_DIR
OURS_GFX = PROJECT / "assets" / "menu" / "02_120_worldmap_new.gfx"

VANILLA_SPRITE_FRAMES = 348          # frames in vanilla sprite 171
OURS_FIRST_FRAME = 349               # our frames start here in OUR gfx
OUR_IMAGE_IDS = set(range(1000, 1025))   # DefineExternalImage2 we added
ERR_IMAGE_IDS = set(range(13500, 13507))  # ERR-only textures - must NOT leak

# Per-profile expected base frame count. Must satisfy:
#   base_frames == VANILLA_SPRITE_FRAMES + config-side ICON_FRAME_OFFSET
# (the bake shifts our iconIds by that offset; see config.py). If a target
# mod update changes its frame count, update BOTH places.
PROFILE_BASE_FRAMES = {"vanilla": 348, "convergence": 756, "erte": 348}

# ── Cleared-marker badge (boss/invader defeated overlay) ──
# The worldmap marker (sprite 174) draws the icon (sprite 171) + a "cleared"
# badge (sprite 173 -> shape 172). Vanilla's native badge is a small gold dot
# placed far up-right of the icon (174->173 translate (566,-448)); ERR reskins
# it to a green checkmark that overlaps the icon (effective centre ≈ (-335,-336)
# twips). For our non-ERR builds we (a) replace shape 172 with our own badge
# and (b) move the badge to the ERR placement so it overlaps the icon.
#
# IMPORTANT - the badge is an EMBEDDED RASTER, not a vector. FFDEC can't import
# detailed SVGs as Scaleform shapes (clip-paths/gradients garble; and a shape
# crashes the map once it exceeds ~30-45 filled figures). So make_cleared_badge.py
# produces a small PNG (assets/badges/cleared_badge.png) from the source art
# (assets/badges/mark2.png) and `ffdec -replace ... 172 <png>` embeds it as a
# DefineBitsLossless2 bitmap-fill shape (1 figure, centred ±257 = 26px, renders
# fine in-game - verified). Vanilla-only for now (Convergence ships its own
# MENU_MAP_Boss_Dead badge via sprite 173 -> char 902, left untouched).
BADGE_PROFILES = {"vanilla", "erte"}
BADGE_IMG = PROJECT / "assets" / "badges" / "cleared_badge.png"

# ── Spoiler-free "?" icon (config::anonymousLoot) ──
# A new sprite-171 frame whose overlay is our gray question-mark raster, so
# anonymous loot markers show "?" instead of the item's icon. Built like a normal
# icon (bg char 1000 + overlay), but the overlay is a cloned shape we then
# `ffdec -replace` with the PNG (same raster-embed trick as the badge).
# ANON_FRAME_INDEX below is the 0-INDEXED position the frame lands at (= frameCount
# before append). The DLL's ANON_ICON_ID is the GAME iconId, which is the 1-BASED
# sprite frame number = this 0-indexed position + 1 (so generate_data uses 441,
# not 440). Don't conflate the two - pointing the DLL at 440 hits our last real
# icon, not the "?".
ANON_PROFILES = {"vanilla", "erte", "convergence", "err"}
ANON_IMG = PROJECT / "assets" / "badges" / "anon_qmark.png"
ANON_FRAME_INDEX = 440        # 0-indexed gfx landing position; DLL iconId = this + 1
ANON_QMARK_SHAPE = 1099       # free charId; cloned from badge shape 172
ANON_BG_CHAR = 1000           # MENU_MAP_MemoCursor (standard icon background)
BADGE_SPRITE_PARENT = 174     # marker container that places the badge sprite

# ── MapForGoblins logo over the map's decorative plaque (obj_246) ──
# Sprite 246 places MENU_FL_Map (char 10), a decorative plaque on the map UI, at
# scale 0.5. char 10 is ALSO a real icon in sprite 171, so we can't replace it
# directly; instead clone the square bitmap-fill shape 181 (256px, bounds
# 0,0-5120,5120 - identical in all 4 profiles), embed our logo, and re-point ONLY
# sprite 246 to the clone. scale/translate render the 256px square at ~1.33x
# char 10's footprint (142*0.5) while keeping the same centre. Skipped if the
# logo source is missing.
LOGO_PROFILES = {"vanilla", "erte", "convergence", "err"}
LOGO_SRC = PROJECT / "assets" / "map_icons" / "MapForGoblins_new.png"
LOGO_SHAPE = 1097             # free charId; cloned from rect shape 181
LOGO_CLONE_SRC = "181"
LOGO_PLAQUE_SPRITE = "246"
LOGO_PLAQUE_CHAR = "10"       # MENU_FL_Map
LOGO_SCALE = 0.38
LOGO_TRANS = -243             # keeps the 256px clone centred on char 10's centre
BADGE_SPRITE_ID = 173         # the badge sprite
BADGE_SHAPE_ID = 172          # the shape the badge sprite draws
BADGE_POS_TWIPS = (-335, -336)  # ERR-matched centre (overlaps the icon)


def ffdec_cmd():
    env = os.environ.get("FFDEC_CLI")
    if env:
        return env.split()
    java = Path("E:/Program Files/Java/jdk1.8.0_211/bin/java.exe")
    jar = PROJECT.parent / "FFDec - Zipped" / "ffdec-cli.jar"
    if java.exists() and jar.exists():
        return [str(java), "-jar", str(jar)]
    print("ERROR: set FFDEC_CLI (e.g. 'java -jar /path/to/ffdec-cli.jar')")
    sys.exit(1)


def run_ffdec(args):
    r = subprocess.run(ffdec_cmd() + args, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout[-2000:], r.stderr[-2000:])
        sys.exit(f"FFDEC failed: {' '.join(args)}")


def sprite171(root):
    for it in root.iter("item"):
        if it.get("type") == "DefineSpriteTag" and it.get("spriteId") == "171":
            return it
    sys.exit("sprite 171 not found")


def _place_object(depth, char_id, scale, tx, ty, alpha=256):
    """A PlaceObject3Tag placing a character (matrix + colorTransform)."""
    po = ET.Element("item")
    po.set("type", "PlaceObject3Tag")
    po.set("depth", str(depth))
    po.set("characterId", str(char_id))
    for fl in ("placeFlagHasCharacter", "placeFlagHasMatrix",
               "placeFlagHasColorTransform", "placeFlagHasImage"):
        po.set(fl, "true")
    for fl in ("placeFlagMove", "placeFlagHasClipDepth", "placeFlagHasName",
               "placeFlagHasRatio", "placeFlagHasFilterList", "placeFlagHasBlendMode",
               "placeFlagHasCacheAsBitmap", "placeFlagHasClassName",
               "placeFlagHasClipActions", "placeFlagHasVisible",
               "placeFlagOpaqueBackground", "reserved"):
        po.set(fl, "false")
    po.set("clipDepth", "0"); po.set("ratio", "0"); po.set("bitmapCache", "0")
    po.set("blendMode", "0"); po.set("forceWriteAsLong", "false"); po.set("visible", "0")
    m = ET.SubElement(po, "matrix")
    m.set("type", "MATRIX"); m.set("hasScale", "true"); m.set("hasRotate", "false")
    m.set("scaleX", str(scale)); m.set("scaleY", str(scale))
    m.set("rotateSkew0", "0.0"); m.set("rotateSkew1", "0.0")
    m.set("translateX", str(tx)); m.set("translateY", str(ty))
    m.set("nScaleBits", "17"); m.set("nRotateBits", "0"); m.set("nTranslateBits", "11")
    ct = ET.SubElement(po, "colorTransform")
    ct.set("type", "CXFORMWITHALPHA")
    ct.set("hasMultTerms", "true"); ct.set("hasAddTerms", "true")
    ct.set("redMultTerm", "256"); ct.set("greenMultTerm", "256"); ct.set("blueMultTerm", "256")
    ct.set("alphaMultTerm", str(alpha))
    ct.set("redAddTerm", "0"); ct.set("greenAddTerm", "0"); ct.set("blueAddTerm", "0")
    ct.set("alphaAddTerm", "0"); ct.set("nbits", "9")
    return po


def add_anon_icon(vroot, vsprite):
    """Clone badge shape 172 -> ANON_QMARK_SHAPE and append the "?" frame to
    sprite 171. Returns the appended frame index (must equal ANON_FRAME_INDEX)."""
    import copy
    # clone a known-good single-figure shape to get a valid charId to -replace
    src_shape = None
    for it in vroot.iter("item"):
        if it.get("type", "").startswith("DefineShape") and it.get("shapeId") == "172":
            src_shape = it
            break
    if src_shape is None:
        sys.exit("anon icon: badge shape 172 not found to clone")
    clone = copy.deepcopy(src_shape)
    clone.set("shapeId", str(ANON_QMARK_SHAPE))
    # insert the clone right before sprite 171 (top-level, same as image defs)
    parent = next(c for c in vroot.iter() if vsprite in list(c))
    parent.insert(list(parent).index(vsprite), clone)

    vsub = vsprite.find("subTags")
    frame_index = int(vsprite.get("frameCount"))  # next frame = current count
    end_tags = [c for c in list(vsub) if c.get("type") == "EndTag"]
    for e in end_tags:
        vsub.remove(e)
    for depth in (1, 2):
        rm = ET.SubElement(vsub, "item")
        rm.set("type", "RemoveObject2Tag"); rm.set("depth", str(depth))
        rm.set("forceWriteAsLong", "false")
    # The "?" seal is a COMPLETE standalone round icon (like the bg-less round
    # markers, e.g. iconId_846 = a 164px image at scale ~0.22 centred at the marker
    # origin). So: no MENU_MAP_MemoCursor bg, just the "?" at one depth, sized to
    # match those round icons and centred. ANON_QMARK_SHAPE is a CENTRE-origin
    # circle (edgeBounds ±237 = 474tw) wrapping the 160px "?" raster, so to render
    # at the reference on-screen size its scale is REF_NATIVE*REF_SCALE*20 / 474,
    # and (centre-origin, symmetric bounds) translate 0 puts it dead centre.
    # (Earlier tries: bg's top-left transform -> speck in the corner; bg footprint
    # -> filled the whole pin; foreground-glyph -> too small + offset right.)
    REF_NATIVE = 164               # iconId_846 image native px (MENU_MAP_09)
    REF_SCALE = 0.21998596         # iconId_846 overlay scale
    QMARK_EDGE = 474               # shape 172/1099 edgeBounds = 2*237
    q_scale = round(REF_NATIVE * REF_SCALE * 20 / QMARK_EDGE, 6)   # ~1.522
    vsub.append(_place_object(1, ANON_QMARK_SHAPE, q_scale, 0, 0))
    sf = ET.SubElement(vsub, "item")
    sf.set("type", "ShowFrameTag"); sf.set("forceWriteAsLong", "false")
    for e in end_tags:
        vsub.append(e)
    vsprite.set("frameCount", str(frame_index + 1))
    return frame_index


def fit_logo_square(out_png, size=256):
    """Fit the logo (LOGO_SRC) into a transparent size×size square (scaled to
    full height, centred) so it maps cleanly onto the square clone shape."""
    from PIL import Image
    logo = Image.open(LOGO_SRC).convert("RGBA")
    s = size / logo.height
    lw = max(1, round(logo.width * s))
    canv = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    canv.paste(logo.resize((lw, size), Image.LANCZOS), ((size - lw) // 2, 0))
    canv.save(out_png)


def add_logo(vroot):
    """Clone the square bitmap-fill shape 181 -> LOGO_SHAPE and re-point sprite
    246's MENU_FL_Map (char 10) placement to it (bigger, centred). Returns True
    if applied (False if the base lacks shape 181 or sprite 246)."""
    import copy
    src = parent = None
    for cont in vroot.iter():
        for ch in list(cont):
            if (ch.get("type") or "").startswith("DefineShape") and ch.get("shapeId") == LOGO_CLONE_SRC:
                src, parent = ch, cont
    if src is None:
        return False
    clone = copy.deepcopy(src)
    clone.set("shapeId", str(LOGO_SHAPE))
    parent.insert(list(parent).index(src) + 1, clone)
    sp = next((it for it in vroot.iter("item") if it.get("type") == "DefineSpriteTag"
               and it.get("spriteId") == LOGO_PLAQUE_SPRITE), None)
    if sp is None:
        return False
    n = 0
    for c in sp.find("subTags").iter("item"):
        if (c.get("type") or "").startswith("PlaceObject") and c.get("characterId") == LOGO_PLAQUE_CHAR:
            c.set("characterId", str(LOGO_SHAPE))
            m = c.find("matrix")
            m.set("hasScale", "true")
            m.set("scaleX", str(LOGO_SCALE)); m.set("scaleY", str(LOGO_SCALE))
            m.set("translateX", str(LOGO_TRANS)); m.set("translateY", str(LOGO_TRANS))
            n += 1
    return n > 0


def build_err_anon_gfx():
    """ERR ships 02_120_worldmap_new.gfx directly (ERR base + our frames). Add
    only the spoiler-free "?" frame to it -> 02_120_worldmap_err.gfx. No merge
    (our frames are already present), no badge (ERR keeps its own green check)."""
    if not ANON_IMG.exists():
        sys.exit(f"anon icon png missing: {ANON_IMG} (run tools/make_anon_icon.py)")
    out_gfx = PROJECT / "assets" / "menu" / "02_120_worldmap_err.gfx"
    print(f"profile=err  base={OURS_GFX}")
    tmp = Path(tempfile.mkdtemp(prefix="mfg_gfx_"))
    try:
        in_xml, out_xml = tmp / "err.xml", tmp / "out.xml"
        run_ffdec(["-swf2xml", str(OURS_GFX), str(in_xml)])
        tree = ET.parse(in_xml)
        vroot = tree.getroot()
        vsprite = sprite171(vroot)
        idx = add_anon_icon(vroot, vsprite)
        if idx != ANON_FRAME_INDEX:   # ERR base 348 + our 92 = 440, offset 0
            sys.exit(f"anon icon (err): frame landed at {idx}, expected {ANON_FRAME_INDEX}")
        logo_ok = LOGO_SRC.exists() and add_logo(vroot)
        tree.write(out_xml, encoding="utf-8", xml_declaration=True)
        print("compiling...")
        run_ffdec(["-xml2swf", str(out_xml), str(out_gfx)])
        run_ffdec(["-replace", str(out_gfx), str(out_gfx),
                   str(ANON_QMARK_SHAPE), str(ANON_IMG)])
        print(f"anon icon: '?' frame at index {idx}; embedded {ANON_IMG.name} "
              f"into shape {ANON_QMARK_SHAPE}")
        if logo_ok:
            logo_png = tmp / "logo_sq.png"; fit_logo_square(logo_png)
            run_ffdec(["-replace", str(out_gfx), str(out_gfx), str(LOGO_SHAPE), str(logo_png)])
            print(f"logo: embedded {LOGO_SRC.name} into obj_246 (shape {LOGO_SHAPE})")
        print(f"wrote {out_gfx} ({out_gfx.stat().st_size} bytes, "
              f"sprite 171: {vsprite.get('frameCount')} frames)")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    profile = None
    args = sys.argv[1:]
    for i, a in enumerate(args):
        if a == "--profile" and i + 1 < len(args):
            profile = args[i + 1].strip().lower()
        elif a.startswith("--profile="):
            profile = a.split("=", 1)[1].strip().lower()
    if profile is None:
        profile = config.PROFILE if config.PROFILE != "err" else "vanilla"
    if profile == "err":
        build_err_anon_gfx()
        return
    if profile not in PROFILE_BASE_FRAMES:
        sys.exit(f"unknown profile '{profile}' (expected: {sorted(PROFILE_BASE_FRAMES)})")

    if profile == "convergence":
        if not config.CONVERGENCE_MOD_DIR:
            sys.exit("set convergence_mod_dir in tools/config.ini")
        base_gfx = config.CONVERGENCE_MOD_DIR / "menu" / "02_120_worldmap.gfx"
    else:
        base_gfx = config.require_game_dir() / "menu" / "02_120_worldmap.gfx"
    if not base_gfx.exists():
        sys.exit(f"base gfx not found: {base_gfx}")
    base_frames = PROFILE_BASE_FRAMES[profile]
    out_gfx = PROJECT / "assets" / "menu" / f"02_120_worldmap_{profile}.gfx"
    print(f"profile={profile}  base={base_gfx}")

    tmp = Path(tempfile.mkdtemp(prefix="mfg_gfx_"))
    try:
        van_xml, our_xml, out_xml = tmp / "van.xml", tmp / "our.xml", tmp / "out.xml"
        print("decompiling base + ours...")
        run_ffdec(["-swf2xml", str(base_gfx), str(van_xml)])
        run_ffdec(["-swf2xml", str(OURS_GFX), str(our_xml)])

        van = ET.parse(van_xml)
        our = ET.parse(our_xml)
        vroot, oroot = van.getroot(), our.getroot()

        # ── our image defs (charIds 1000-1024) ──
        our_images = []
        for it in oroot.iter("item"):
            if it.get("type") == "DefineExternalImage2":
                cid = int(it.get("characterID") or 0)
                if cid in OUR_IMAGE_IDS:
                    our_images.append(it)
        if len(our_images) != len(OUR_IMAGE_IDS):
            sys.exit(f"expected {len(OUR_IMAGE_IDS)} image defs, found {len(our_images)}")

        # ── our sprite-171 frames after the vanilla range (in OUR gfx our
        # frames always start at 349 - the ERR base adds no frames) ──
        osub = sprite171(oroot).find("subTags")
        frame = 0
        added = []
        for child in list(osub):
            if frame >= VANILLA_SPRITE_FRAMES and child.get("type") != "EndTag":
                added.append(child)
            if child.get("type") == "ShowFrameTag":
                frame += 1
        n_added = sum(1 for c in added if c.get("type") == "ShowFrameTag")
        print(f"ours: {frame} frames, appending {n_added} "
              f"(land at {base_frames + 1}..{base_frames + n_added} on this base)")

        # safety: no ERR-only textures in what we append
        for c in added:
            for el in [c] + list(c.iter()):
                cid = el.get("characterId")
                if cid and int(cid) in ERR_IMAGE_IDS:
                    sys.exit(f"appended frame references ERR-only charId {cid}")

        # ── merge into the base ──
        vsprite = sprite171(vroot)
        vsub = vsprite.find("subTags")
        if int(vsprite.get("frameCount")) != base_frames:
            sys.exit(f"base sprite 171 has {vsprite.get('frameCount')} frames, "
                     f"expected {base_frames} - base mod/game updated? Update "
                     f"PROFILE_BASE_FRAMES here AND ICON_FRAME_OFFSET in config.py")

        # the base must not already define our charId range (we'd collide)
        for it in vroot.iter("item"):
            if it.get("type") == "DefineExternalImage2":
                cid = int(it.get("characterID") or 0)
                if cid in OUR_IMAGE_IDS:
                    sys.exit(f"base gfx already uses charId {cid} (our range "
                             f"1000-1024) - pick a new range before merging")

        # insert image defs right before the sprite 171 definition (top level)
        # find the top-level container holding the sprite
        parent = None
        for cand in vroot.iter():
            if vsprite in list(cand):
                parent = cand
                break
        idx = list(parent).index(vsprite)
        for img in reversed(our_images):
            parent.insert(idx, img)

        # drop the EndTag, append frames, restore EndTag
        end_tags = [c for c in list(vsub) if c.get("type") == "EndTag"]
        for e in end_tags:
            vsub.remove(e)
        for c in added:
            vsub.append(c)
        for e in end_tags:
            vsub.append(e)
        vsprite.set("frameCount", str(base_frames + n_added))

        # ── reposition the cleared badge to overlap the icon (badge profiles) ──
        if profile in BADGE_PROFILES:
            moved = False
            for sp in vroot.iter("item"):
                if sp.get("type") == "DefineSpriteTag" and sp.get("spriteId") == str(BADGE_SPRITE_PARENT):
                    for ch in sp.iter("item"):
                        if (ch.get("type") in ("PlaceObject2Tag", "PlaceObject3Tag")
                                and ch.get("characterId") == str(BADGE_SPRITE_ID)):
                            mtx = ch.find(".//matrix")
                            if mtx is not None:
                                mtx.set("hasTranslate", "true")
                                mtx.set("translateX", str(BADGE_POS_TWIPS[0]))
                                mtx.set("translateY", str(BADGE_POS_TWIPS[1]))
                                moved = True
            if not moved:
                sys.exit(f"badge: could not find sprite {BADGE_SPRITE_PARENT}->{BADGE_SPRITE_ID} placement")
            print(f"badge: moved {BADGE_SPRITE_PARENT}->{BADGE_SPRITE_ID} to {BADGE_POS_TWIPS} twips")

        # ── spoiler-free "?" icon frame (anon profiles) ──
        if profile in ANON_PROFILES:
            if not ANON_IMG.exists():
                sys.exit(f"anon icon png missing: {ANON_IMG} (run tools/make_anon_icon.py)")
            idx = add_anon_icon(vroot, vsprite)
            # DLL's generated ANON_ICON_ID = 440 + ICON_FRAME_OFFSET; the gfx
            # offset for this base = base_frames - 348. They must agree.
            expected = ANON_FRAME_INDEX + (base_frames - 348)
            if idx != expected:
                sys.exit(f"anon icon: frame landed at {idx}, expected {expected} "
                         f"(= 440 + offset {base_frames - 348}) - our frame count "
                         f"changed? keep generate_data ANON_ICON_ID in sync")
            print(f"anon icon: appended '?' frame at index {idx} "
                  f"(shape {ANON_QMARK_SHAPE} from clone of 172)")

        logo_ok = profile in LOGO_PROFILES and LOGO_SRC.exists() and add_logo(vroot)

        van.write(out_xml, encoding="utf-8", xml_declaration=True)
        print("compiling...")
        run_ffdec(["-xml2swf", str(out_xml), str(out_gfx)])

        # ── embed our badge raster into the badge shape (badge profiles) ──
        if profile in BADGE_PROFILES:
            if not BADGE_IMG.exists():
                sys.exit(f"badge png missing: {BADGE_IMG} (run tools/make_cleared_badge.py)")
            run_ffdec(["-replace", str(out_gfx), str(out_gfx),
                       str(BADGE_SHAPE_ID), str(BADGE_IMG)])
            print(f"badge: embedded {BADGE_IMG.name} into shape {BADGE_SHAPE_ID}")

        # ── embed the "?" raster into the cloned shape (anon profiles) ──
        if profile in ANON_PROFILES:
            run_ffdec(["-replace", str(out_gfx), str(out_gfx),
                       str(ANON_QMARK_SHAPE), str(ANON_IMG)])
            print(f"anon icon: embedded {ANON_IMG.name} into shape {ANON_QMARK_SHAPE}")

        # ── embed the MapForGoblins logo into obj_246 (logo profiles) ──
        if logo_ok:
            logo_png = tmp / "logo_sq.png"; fit_logo_square(logo_png)
            run_ffdec(["-replace", str(out_gfx), str(out_gfx), str(LOGO_SHAPE), str(logo_png)])
            print(f"logo: embedded {LOGO_SRC.name} into obj_246 (shape {LOGO_SHAPE})")

        print(f"wrote {out_gfx} ({out_gfx.stat().st_size} bytes, "
              f"sprite 171: {vsprite.get('frameCount')} frames)")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
