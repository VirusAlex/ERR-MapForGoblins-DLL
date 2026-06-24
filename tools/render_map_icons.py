#!/usr/bin/env python3
"""
Render composed map-icon previews from a worldmap GFX.

Walks sprite-171's frames (cumulative display list), composites each placed
character with its matrix + colorTransform, and writes one PNG per frame
(iconId_<N>.png, 1-based to match the in-game iconId).

Three layer sources, resolved per placed character:
  * DefineExternalImage2 (the bulk of icons) -> a texture file <exportName>.tga
    / .png / .dds found in the --tex-dir / --source / --menu-dir search path.
  * DefineShape* (vector icons + our bitmap-fill "?" frame) and embedded
    DefineBits* -> rendered by FFDEC straight from the GFX (`-export shape,image
    -format shape:png,image:png`). This is what makes the spoiler-free "?" and
    any vector icon show up (the old version only drew external-image layers).

Textures come from the actual game / mod files, not a stale snapshot:
  --source game|err|convergence|erte   resolve menu dir(s) from tools/config.ini
  --menu-dir PATH ...                  explicit menu dir(s) (each must hold
                                       hi/01_common.sblytbnd.dcx); extracted on
                                       the fly via extract_subtextures.
  --tex-dir PATH ...                   pre-extracted <name>.(tga|png|dds) dirs
                                       (assets/menu is always added as fallback).

Examples:
  # Convergence build, textures from the vanilla game + the Convergence mod:
  py tools/render_map_icons.py --gfx assets/menu/02_120_worldmap_convergence.gfx \
      --source game --menu-dir "G:/.../ConvergenceER/mod/menu" \
      --out-dir assets/map_icons/composed/convergence

  # Quick (legacy) render straight from a decompiled XML + assets/menu TGAs:
  py tools/render_map_icons.py --xml scratch/worldmap.xml --frames 1-849
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

from PIL import Image

sys.path.insert(0, os.path.dirname(__file__))
import config

PROJECT = config.PROJECT_DIR
TWIPS = 20.0  # 1 px = 20 twips


# ── FFDEC ──────────────────────────────────────────────────────────────────
def ffdec_cmd():
    env = os.environ.get("FFDEC_CLI")
    if env:
        return env.split()
    jar = PROJECT.parent / "FFDec - Zipped" / "ffdec-cli.jar"
    for java in (Path("E:/Program Files/Java/jdk-21/bin/java.exe"),
                 Path("E:/Program Files/Java/jdk1.8.0_211/bin/java.exe")):
        if java.exists() and jar.exists():
            return [str(java), "-jar", str(jar)]
    sys.exit("ERROR: set FFDEC_CLI env var (e.g. 'java -jar /path/to/ffdec-cli.jar'); "
             "note it is split on spaces - use 8.3 short paths if java/jar paths contain spaces.")


def run_ffdec(args):
    r = subprocess.run(ffdec_cmd() + args, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"FFDEC failed: {' '.join(args)}\n{r.stdout[-1500:]}\n{r.stderr[-1500:]}")


def ffdec_export_chars(gfx_path, out_dir):
    """Export every shape + embedded image as <charId>.png. Vector shapes and
    bitmap-fill shapes render correctly; external images come out as red
    placeholders (we resolve those from the texture path instead)."""
    os.makedirs(out_dir, exist_ok=True)
    run_ffdec(["-format", "shape:png,image:png", "-export", "shape,image",
               out_dir, str(gfx_path)])
    # FFDEC drops shapes/images into subfolders (shapes/, images/); names like
    # "1099.png" or "DefineShape4Tag_1099.png". Walk everything, key by trailing id.
    import re
    out = {}
    for dp, _, files in os.walk(out_dir):
        for f in files:
            if not f.lower().endswith(".png"):
                continue
            m = re.search(r"(\d+)$", os.path.splitext(f)[0])
            if m:
                out.setdefault(int(m.group(1)), os.path.join(dp, f))
    return out


# ── texture search path ──────────────────────────────────────────────────────
def resolve_source_menu_dirs(source):
    """Map a --source keyword to menu dir(s) to extract textures from."""
    g = config.GAME_DIR
    if source == "game":
        return [g / "menu"] if g else []
    if source == "err":
        d = config.ERR_MOD_DIR
        return [p / "menu" for p in (d, g) if p]
    if source == "convergence2":
        return [p / "menu" for p in (config.CONVERGENCE2_MOD_DIR, g) if p]
    if source == "erte":
        return [p / "menu" for p in (config.ERTE_MOD_DIR, g) if p]
    sys.exit(f"unknown --source '{source}' (game|err|convergence2|erte)")


def build_texture_dirs(menu_dirs, cache_root):
    """Extract 01_common sub-textures from each menu dir into its own cache dir.
    Later dirs override earlier ones (mod over game), so return mod-first."""
    import extract_subtextures
    dirs = []
    for i, md in enumerate(menu_dirs):
        hi = Path(md) / "hi"
        cache = os.path.join(cache_root, f"src{i}")
        n = extract_subtextures.extract_menu_dir(hi, cache)
        print(f"  textures: {n:5} from {md}")
        if n:
            dirs.append(cache)
    return list(reversed(dirs))  # mod (last given) wins -> searched first


def find_texture(name, tex_dirs):
    if not name:
        return None
    for d in tex_dirs:
        for ext in (".tga", ".png", ".dds"):
            p = os.path.join(d, name + ext)
            if os.path.exists(p):
                return p
    return None


# ── compositing ──────────────────────────────────────────────────────────────
def _apply_ct(img, ct):
    """Apply a Scaleform CXFORMWITHALPHA: out = in*mult/256 + add, per channel."""
    if not ct:
        return img
    r, g, b, a = img.split()
    if (ct["rm"], ct["gm"], ct["bm"], ct["ra"], ct["ga"], ct["ba"]) != (256, 256, 256, 0, 0, 0):
        r = r.point(lambda p: min(255, max(0, p * ct["rm"] // 256 + ct["ra"])))
        g = g.point(lambda p: min(255, max(0, p * ct["gm"] // 256 + ct["ga"])))
        b = b.point(lambda p: min(255, max(0, p * ct["bm"] // 256 + ct["ba"])))
    if ct["am"] != 256:
        a = a.point(lambda p: min(255, max(0, p * ct["am"] // 256)))
    return Image.merge("RGBA", (r, g, b, a))


def _ct_compose(parent, child):
    """Compose color transforms for nested sprites (child applied, then parent)."""
    if not parent:
        return child
    if not child:
        return parent
    def comb(pm, pa, cm, ca):
        return pm * cm // 256, pm * ca // 256 + pa
    rm, ra = comb(parent["rm"], parent["ra"], child["rm"], child["ra"])
    gm, ga = comb(parent["gm"], parent["ga"], child["gm"], child["ga"])
    bm, ba = comb(parent["bm"], parent["ba"], child["bm"], child["ba"])
    return {"rm": rm, "gm": gm, "bm": bm, "am": parent["am"] * child["am"] // 256,
            "ra": ra, "ga": ga, "ba": ba}


def _parse_meta(root):
    """Char metadata: external-image names, shape bounds, shape->fill-bitmap."""
    ext_names, shape_bounds, shape_bitmap, char_type = {}, {}, {}, {}
    for it in root.iter("item"):
        t = it.get("type", "")
        cid = it.get("characterID") or it.get("characterId") or it.get("shapeId")
        if t == "DefineExternalImage2":
            ext_names[int(it.get("characterID", 0))] = it.get("exportName", "")
            char_type[int(it.get("characterID", 0))] = "image"
        elif t.startswith("DefineShape"):
            sid = int(it.get("shapeId", 0))
            char_type[sid] = "shape"
            b = it.find("edgeBounds")
            if b is None:
                b = it.find("shapeBounds")
            if b is not None:
                shape_bounds[sid] = (int(b.get("Xmin", 0)), int(b.get("Ymin", 0)),
                                     int(b.get("Xmax", 0)), int(b.get("Ymax", 0)))
            for fs in it.iter("item"):
                if fs.get("type") == "FILLSTYLE" and fs.get("bitmapId", "0") not in ("0", None):
                    shape_bitmap[sid] = int(fs.get("bitmapId"))
                    break
        elif t.startswith("DefineBits") and cid and cid.isdigit():
            char_type[int(cid)] = "bits"
    return ext_names, shape_bounds, shape_bitmap, char_type


def _parse_sprite_frames(sprite_elem):
    """Cumulative per-frame display list (Scaleform semantics) -> list of
    {depth: layer-info}, one entry per ShowFrame."""
    frames, current = [], {}
    for tag in sprite_elem.find("subTags"):
        tt = tag.get("type", "")
        if tt in ("PlaceObject2Tag", "PlaceObject3Tag"):
            depth = int(tag.get("depth", 0))
            m = tag.find("matrix")
            ct = tag.find("colorTransform")
            layer = dict(current.get(depth, {}))
            if tag.get("characterId"):
                layer["cid"] = int(tag.get("characterId"))
            if m is not None:
                has_scale = m.get("hasScale", "true") != "false"  # FFDEC writes "0.0" for no-scale
                layer.update(
                    sx=float(m.get("scaleX", 1)) if has_scale else 1.0,
                    sy=float(m.get("scaleY", 1)) if has_scale else 1.0,
                    tx=float(m.get("translateX", 0)), ty=float(m.get("translateY", 0)))
            layer.setdefault("sx", 1.0); layer.setdefault("sy", 1.0)
            layer.setdefault("tx", 0.0); layer.setdefault("ty", 0.0)
            if ct is not None:
                layer["ct"] = {
                    "rm": int(ct.get("redMultTerm", 256)), "gm": int(ct.get("greenMultTerm", 256)),
                    "bm": int(ct.get("blueMultTerm", 256)), "am": int(ct.get("alphaMultTerm", 256)),
                    "ra": int(ct.get("redAddTerm", 0)), "ga": int(ct.get("greenAddTerm", 0)),
                    "ba": int(ct.get("blueAddTerm", 0))}
            current[depth] = layer
        elif tt == "RemoveObject2Tag":
            current.pop(int(tag.get("depth", 0)), None)
        elif tt == "ShowFrameTag":
            frames.append({d: dict(v) for d, v in current.items()})
    return frames


def render_icons(xml_path, tex_dirs, out_dir, frame_range=None,
                 char_pngs=None, sprite_id="171", canvas=128, objects=False):
    char_pngs = char_pngs or {}
    root = ET.parse(xml_path).getroot()
    ext_names, shape_bounds, shape_bitmap, char_type = _parse_meta(root)

    sprite_frames = {}   # spriteId(str) -> [frame display lists]
    for it in root.iter("item"):
        if it.get("type") == "DefineSpriteTag":
            sid = it.get("spriteId")
            sprite_frames[sid] = _parse_sprite_frames(it)
            char_type[int(sid)] = "sprite"

    miss_names = set()
    leaf_count = [0]   # leaves actually drawn for the current frame (plate+glyph -> >=2)

    def draw_leaf(cvs, cx, cy, cid, sx, sy, tx, ty, ct):
        """Composite one image/shape/bitmap char at the effective transform."""
        img, origin, extent_tw = None, (0.0, 0.0), None
        kind = char_type.get(cid)
        if kind == "image":
            p = find_texture(ext_names.get(cid, ""), tex_dirs)
            if p:
                img = Image.open(p).convert("RGBA")
        elif kind == "shape":
            bx, by, ex, ey = shape_bounds.get(cid, (0, 0, 0, 0))
            origin = (bx, by)
            bmp = shape_bitmap.get(cid)
            if bmp is not None and bmp in char_pngs:   # bitmap-fill shape -> full-res raster
                img = Image.open(char_pngs[bmp]).convert("RGBA")
                extent_tw = (ex - bx, ey - by)
            elif cid in char_pngs:
                img = Image.open(char_pngs[cid]).convert("RGBA")
        if img is None and cid in char_pngs:
            img = Image.open(char_pngs[cid]).convert("RGBA")
        if img is None:
            miss_names.add(ext_names.get(cid, f"char{cid}"))
            return False
        if extent_tw is not None:
            nw = max(1, int(round(extent_tw[0] * sx / TWIPS)))
            nh = max(1, int(round(extent_tw[1] * sy / TWIPS)))
        else:
            nw = max(1, int(round(img.width * sx)))
            nh = max(1, int(round(img.height * sy)))
        img = img.resize((nw, nh), Image.LANCZOS)
        img = _apply_ct(img, ct)
        px = cx + (origin[0] * sx + tx) / TWIPS
        py = cy + (origin[1] * sy + ty) / TWIPS
        cvs.alpha_composite(img, (int(round(px)), int(round(py))))
        leaf_count[0] += 1
        return True

    def composite(cvs, cx, cy, sid, frame_idx, Asx, Asy, Atx, Aty, Act, stack=()):
        """Draw sprite `sid`'s frame, recursing into nested sprites. Transform
        composes A∘layer for scale+translate; color transforms compose too."""
        frames = sprite_frames.get(str(sid))
        if not frames or sid in stack:   # missing or cycle guard
            return False
        fr = frames[frame_idx] if frame_idx < len(frames) else frames[-1]
        drew = False
        for depth in sorted(fr):
            info = fr[depth]
            cid = info.get("cid")
            if cid is None:
                continue
            esx, esy = Asx * info["sx"], Asy * info["sy"]
            etx, ety = Asx * info["tx"] + Atx, Asy * info["ty"] + Aty
            ect = _ct_compose(Act, info.get("ct"))
            if char_type.get(cid) == "sprite":
                drew |= composite(cvs, cx, cy, cid, 0, esx, esy, etx, ety, ect, stack + (sid,))
            else:
                drew |= draw_leaf(cvs, cx, cy, cid, esx, esy, etx, ety, ect)
        return drew

    os.makedirs(out_dir, exist_ok=True)

    if objects:
        # Render every NON-icon sprite (overlays like the cleared badge, cursors,
        # state markers) to <out-dir>/objects/obj_<id>.png - composited with nested
        # sprites, auto-cropped to content so internal offsets don't hide them.
        odir = os.path.join(out_dir, "objects")
        os.makedirs(odir, exist_ok=True)
        BIG = 2048
        n = 0
        for sid in sorted(sprite_frames, key=lambda s: int(s)):
            if sid == sprite_id:
                continue
            cvs = Image.new("RGBA", (BIG, BIG), (0, 0, 0, 0))
            if not composite(cvs, BIG // 2, BIG // 2, sid, 0, 1.0, 1.0, 0.0, 0.0, None):
                continue
            bbox = cvs.getbbox()
            if not bbox:
                continue
            pad = 4
            crop = cvs.crop((max(0, bbox[0] - pad), max(0, bbox[1] - pad),
                             min(BIG, bbox[2] + pad), min(BIG, bbox[3] + pad)))
            crop.save(os.path.join(odir, f"obj_{sid}.png"))
            n += 1
        print(f"Rendered {n} objects to {odir}")
        return

    frames = sprite_frames.get(sprite_id)
    if frames is None:
        sys.exit(f"sprite {sprite_id} not found in {xml_path}")
    cx = cy = canvas // 2
    if frame_range is None:
        frame_range = range(1, len(frames) + 1)

    rendered = 0
    layer_counts = {}   # iconId -> leaves drawn (>=2 => has a background plate)
    for fnum in frame_range:
        if fnum < 1 or fnum > len(frames):
            continue
        layers = frames[fnum - 1]
        if not layers:
            continue
        cvs = Image.new("RGBA", (canvas, canvas), (0, 0, 0, 0))
        leaf_count[0] = 0
        for depth in sorted(layers):
            info = layers[depth]
            cid = info.get("cid")
            if cid is None:
                continue
            if char_type.get(cid) == "sprite":
                composite(cvs, cx, cy, cid, 0, info["sx"], info["sy"], info["tx"], info["ty"], info.get("ct"))
            else:
                draw_leaf(cvs, cx, cy, cid, info["sx"], info["sy"], info["tx"], info["ty"], info.get("ct"))
        cvs.save(os.path.join(out_dir, f"iconId_{fnum}.png"))
        layer_counts[fnum] = leaf_count[0]
        rendered += 1
    with open(os.path.join(out_dir, "layer_counts.json"), "w", encoding="utf-8") as lf:
        json.dump(layer_counts, lf)
    print(f"Rendered {rendered} icons to {out_dir}")
    if miss_names:
        print(f"  ({len(miss_names)} unresolved chars; e.g. {', '.join(sorted(miss_names)[:8])})")


def main():
    ap = argparse.ArgumentParser(description="Render worldmap map-icon previews")
    ap.add_argument("--gfx", help="GFX to render (decompiled + shape/bitmap-exported via FFDEC)")
    ap.add_argument("--xml", help="Pre-decompiled GFX XML (no shape/bitmap layers unless --gfx given)")
    ap.add_argument("--source", help="Texture source: game | err | convergence | erte")
    ap.add_argument("--menu-dir", action="append", default=[], help="Menu dir(s) with hi/01_common.* (repeatable)")
    ap.add_argument("--tex-dir", action="append", default=[], help="Pre-extracted texture dir(s) (repeatable)")
    ap.add_argument("--out-dir", default=str(PROJECT / "assets" / "map_icons" / "composed"))
    ap.add_argument("--frames", default="1-849", help="Frame range e.g. 1-849 or 757,848,849")
    ap.add_argument("--sprite", default="171")
    ap.add_argument("--canvas", type=int, default=128)
    ap.add_argument("--objects", action="store_true",
                    help="Instead of iconId frames, render every NON-icon sprite "
                         "(overlays: cleared badge, cursors, state markers) to "
                         "<out-dir>/objects/obj_<id>.png, nested-composited + auto-cropped")
    args = ap.parse_args()

    if not args.gfx and not args.xml:
        ap.error("provide --gfx (preferred) or --xml")

    tmp = Path(tempfile.mkdtemp(prefix="mfg_render_"))
    try:
        xml_path = args.xml
        char_pngs = {}
        if args.gfx:
            if not xml_path:
                xml_path = str(tmp / "in.xml")
                run_ffdec(["-swf2xml", str(args.gfx), xml_path])
            char_pngs = ffdec_export_chars(args.gfx, str(tmp / "chars"))

        # texture search path: explicit tex-dirs + extracted menu/source dirs + assets/menu fallback
        menu_dirs = list(args.menu_dir)
        if args.source:
            menu_dirs = [str(d) for d in resolve_source_menu_dirs(args.source)] + menu_dirs
        tex_dirs = list(args.tex_dir)
        if menu_dirs:
            tex_dirs += build_texture_dirs(menu_dirs, str(tmp / "tex"))
        tex_dirs.append(str(PROJECT / "assets" / "menu"))  # fallback

        if "-" in args.frames:
            a, b = args.frames.split("-"); fr = range(int(a), int(b) + 1)
        else:
            fr = [int(x) for x in args.frames.split(",")]

        render_icons(xml_path, tex_dirs, args.out_dir, fr, char_pngs,
                     sprite_id=args.sprite, canvas=args.canvas, objects=args.objects)
    finally:
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
