#!/usr/bin/env python3
"""Generate src/generated_shared/goblin_overlay_icons.{hpp,cpp}: a small icon
atlas (one representative map-icon per show_* category) + an ini-key -> atlas-cell
table, embedded as raw RGBA so the in-game config overlay can draw the real
category icon next to each toggle (ImGui::Image), with no runtime image decode.

Mapping is DERIVED from the actual source (no hand-maintained dict):
  ini key  -> config var   (from goblin_config_schema.cpp B(...)/BE(...))
  config var -> Category    (from goblin_inject.cpp is_category_enabled switch)
  Category -> iconId        (most common iconId among that category's baked rows
                             in src/generated/goblin_map_data.cpp, the ERR superset)
Icon images come from assets/map_icons/by_iconId/iconId_<N>.png (rendered by
render_map_icons.py); any missing ones are rendered on the fly from the vanilla
gfx. The output is profile-independent and committed (shared by all builds).
"""
import os, re, sys, subprocess, collections, tempfile, json
sys.path.insert(0, os.path.dirname(__file__))
import config
from PIL import Image

PROJ = config.PROJECT_DIR
SRC = PROJ / "src"
BY_ICON = PROJ / "assets" / "map_icons" / "by_iconId"
OUT_DIR = SRC / "generated_shared"
CELL = 40          # atlas cell px (square)
COLS = 10          # atlas columns

# Manual ini-key -> iconId overrides (applied after the derived mapping). For keys
# whose representative icon is unhelpful or that aren't a category at all.
#   438 = blue seal-puzzle icon (most recognizable interactable)
#   441 = our gray "?" anon icon (ANON_ICON_ID in the vanilla gfx we render from)
KEY_ICON_OVERRIDE = {
    "show_interactables": 438,
    "anonymous_loot": 441,
}


def parse_key_to_var():
    """ini key -> config var, from the schema B(...)/BE(...) macros."""
    txt = (SRC / "goblin_config_schema.cpp").read_text(encoding="utf-8")
    out = {}
    for m in re.finditer(r'\bBE?\(\s*"([A-Za-z0-9_]+)"\s*,\s*([A-Za-z0-9_]+)\s*,', txt):
        out[m.group(1)] = m.group(2)
    return out


def parse_var_to_category():
    """config var -> Category, from is_category_enabled()."""
    txt = (SRC / "goblin_inject.cpp").read_text(encoding="utf-8")
    out = {}
    for m in re.finditer(r'case\s+Category::(\w+):\s*return\s+goblin::config::(\w+);', txt):
        out[m.group(2)] = m.group(1)
    return out


def parse_category_to_icon():
    """Category -> most common iconId, from the baked ERR map data (superset)."""
    txt = (SRC / "generated" / "goblin_map_data.cpp").read_text(encoding="utf-8", errors="replace")
    tally = collections.defaultdict(collections.Counter)
    cur_icon = None
    for line in txt.splitlines():
        mi = re.search(r'\.iconId\s*=\s*(\d+)', line)
        if mi:
            cur_icon = int(mi.group(1))
        mc = re.search(r'\},\s*Category::(\w+),', line)
        if mc and cur_icon is not None:
            tally[mc.group(1)][cur_icon] += 1
    return {cat: c.most_common(1)[0][0] for cat, c in tally.items()}


def icon_image(icon_id, render_cache):
    """RGBA Image for an iconId. Prefer a FRESH full-composite render; fall back
    to the by_iconId cache (which can be an incomplete flat-plate render)."""
    p = render_cache / f"iconId_{icon_id}.png"
    if p.exists():
        return Image.open(p).convert("RGBA")
    p2 = BY_ICON / f"iconId_{icon_id}.png"
    if p2.exists():
        return Image.open(p2).convert("RGBA")
    return None


def render_icons(icon_ids, render_cache):
    """Render the given iconIds fresh from the vanilla gfx (full composite)."""
    if not icon_ids:
        return
    gfx = PROJ / "assets" / "menu" / "02_120_worldmap_vanilla.gfx"
    if not gfx.exists():
        print(f"[overlay-icons] vanilla gfx missing, cannot render icons")
        return
    os.makedirs(render_cache, exist_ok=True)
    frames = ",".join(str(i) for i in sorted(icon_ids))
    print(f"[overlay-icons] rendering {len(icon_ids)} icons via render_map_icons...")
    subprocess.run([sys.executable, str(PROJ / "tools" / "render_map_icons.py"),
                    "--gfx", str(gfx), "--source", "game",
                    "--frames", frames, "--out-dir", str(render_cache)],
                   cwd=str(PROJ / "tools"))


def fit_cell(img, zoom=1.0):
    """Fit an RGBA icon into a CELL x CELL transparent square, preserving aspect.
    Crops to the alpha>16 bounds so faint halos don't shrink the visible glyph.
    zoom>1 scales the content past the cell and center-crops the overflow (used for
    plate-on-circle icons so their small inner glyph reads bigger)."""
    alpha = img.split()[3]
    mask = alpha.point(lambda v: 255 if v > 16 else 0)
    bb = mask.getbbox() or img.getbbox() or (0, 0, img.width, img.height)
    img = img.crop(bb)
    # Some map icons are drawn semi-transparent (e.g. the seal-puzzle icon caps at
    # alpha ~149) and vanish on the dark overlay bg. Rescale alpha so the strongest
    # pixel becomes fully opaque, preserving the relative softness.
    a = img.split()[3]
    peak = a.getextrema()[1]
    if 0 < peak < 230:
        r, g, b, _ = img.split()
        a = a.point(lambda v: min(255, round(v * 255 / peak)))
        img = Image.merge("RGBA", (r, g, b, a))
    s = min(CELL / img.width, CELL / img.height) * zoom
    w, h = max(1, round(img.width * s)), max(1, round(img.height * s))
    img = img.resize((w, h), Image.LANCZOS)
    cell = Image.new("RGBA", (CELL, CELL), (0, 0, 0, 0))
    cell.paste(img, ((CELL - w) // 2, (CELL - h) // 2), img)  # neg. offset => center-crop
    return cell


def main():
    key_to_var = parse_key_to_var()
    var_to_cat = parse_var_to_category()
    cat_to_icon = parse_category_to_icon()

    # ini key -> iconId (only for keys that resolve through to a category icon)
    key_to_icon = {}
    for key, var in key_to_var.items():
        cat = var_to_cat.get(var)
        if cat and cat in cat_to_icon:
            key_to_icon[key] = cat_to_icon[cat]
    key_to_icon.update(KEY_ICON_OVERRIDE)  # manual overrides (seal icon, anon "?")
    print(f"[overlay-icons] {len(key_to_icon)} keys mapped to icons "
          f"({len(key_to_var)} keys, {len(var_to_cat)} categories)")

    needed = sorted(set(key_to_icon.values()))
    render_cache = PROJ / "scratch" / "overlay_icon_render"
    render_icons(needed, render_cache)  # always render fresh (full composite)

    # Two-layer icons (a category plate behind a small glyph) read visually smaller
    # than single-glyph icons at the same cell size, because the plate fills the cell
    # and the glyph inside is small. We detect "plated" icons two ways and ZOOM their
    # cell content (cropping the plate edges) so the inner glyph reads bigger while the
    # on-screen icon size stays uniform: (a) a background layer (>=2 leaves, from
    # render_map_icons) catches ring frames; (b) high bbox coverage (the plate fills
    # most of its footprint) catches solid plates baked as one bitmap. Empirically
    # plated >=0.77, single-glyph <=0.71.
    PLATE_ZOOM = 1.45
    OVERRIDE_ZOOM = 1.0   # our override icons (seal/anon) are glyph-filling; no enlarge
    COVERAGE_THR = 0.74
    override_icons = set(KEY_ICON_OVERRIDE.values())
    layer_counts = {}
    lc_path = render_cache / "layer_counts.json"
    if lc_path.exists():
        layer_counts = {int(k): v for k, v in json.load(open(lc_path, encoding="utf-8")).items()}

    def bbox_coverage(img):
        a = img.split()[3]
        m = a.point(lambda v: 255 if v > 16 else 0)
        bb = m.getbbox()
        if not bb:
            return 0.0
        area = (bb[2] - bb[0]) * (bb[3] - bb[1]) or 1
        return sum(1 for c in m.crop(bb).getdata() if c) / area

    # assign each unique iconId a cell; build the atlas. render_map_icons now
    # applies the Scaleform color-transform (our per-iconId tints, baked into the
    # gfx via icon_tints_config.json), so the rendered PNG is already correctly
    # tinted - no post-tint here.
    icon_to_idx = {}
    cells = []
    for icon in needed:
        img = icon_image(icon, render_cache)
        if img is None:
            print(f"[overlay-icons] WARN no image for iconId {icon}; keys using it skip")
            continue
        if icon in override_icons:
            zoom = OVERRIDE_ZOOM
        else:
            plated = layer_counts.get(icon, 1) >= 2 or bbox_coverage(img) >= COVERAGE_THR
            zoom = PLATE_ZOOM if plated else 1.0
        icon_to_idx[icon] = len(cells)
        cells.append(fit_cell(img, zoom))

    n = len(cells)
    rows = max(1, (n + COLS - 1) // COLS)
    atlas_w, atlas_h = COLS * CELL, rows * CELL
    atlas = Image.new("RGBA", (atlas_w, atlas_h), (0, 0, 0, 0))
    for idx, cell in enumerate(cells):
        atlas.paste(cell, ((idx % COLS) * CELL, (idx // COLS) * CELL), cell)

    rgba = atlas.tobytes()  # RGBA, row-major

    # Mod logo, embedded as its own texture (shown left of the master switch).
    logo_src = PROJ / "assets" / "map_icons" / "MapForGoblins_new.png"
    if not logo_src.exists():
        logo_src = PROJ / "assets" / "map_icons" / "MapForGoblins.png"
    logo = Image.open(logo_src).convert("RGBA")
    LH = 256  # embed high-res so the ~120px About logo is a crisp downscale
    LW = max(1, round(logo.width * LH / logo.height))
    logo = logo.resize((LW, LH), Image.LANCZOS)
    logo_rgba = logo.tobytes()

    # key -> (col, row)
    key_cells = []
    for key, icon in sorted(key_to_icon.items()):
        if icon in icon_to_idx:
            idx = icon_to_idx[icon]
            key_cells.append((key, idx % COLS, idx // COLS))

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    (OUT_DIR / "goblin_overlay_icons.hpp").write_text(
        "#pragma once\n"
        "// Generated by tools/generate_overlay_icons.py. Category icon atlas for\n"
        "// the in-game config overlay (raw RGBA, no runtime decode).\n\n"
        "namespace goblin::overlay_icons\n{\n"
        "    extern const int ATLAS_W;\n"
        "    extern const int ATLAS_H;\n"
        "    extern const int CELL;\n"
        "    extern const unsigned char ATLAS_RGBA[]; // ATLAS_W*ATLAS_H*4, row-major\n"
        "    struct IconCell { const char *key; int col; int row; };\n"
        "    extern const IconCell ICON_CELLS[];\n"
        "    extern const int ICON_CELL_COUNT;\n"
        "    extern const int LOGO_W;\n"
        "    extern const int LOGO_H;\n"
        "    extern const unsigned char LOGO_RGBA[]; // LOGO_W*LOGO_H*4, row-major\n"
        "}\n", encoding="utf-8")

    with open(OUT_DIR / "goblin_overlay_icons.cpp", "w", encoding="utf-8", newline="\n") as f:
        f.write('#include "goblin_overlay_icons.hpp"\n\n')
        f.write("namespace goblin::overlay_icons\n{\n")
        f.write(f"const int ATLAS_W = {atlas_w};\n")
        f.write(f"const int ATLAS_H = {atlas_h};\n")
        f.write(f"const int CELL = {CELL};\n")
        f.write("const unsigned char ATLAS_RGBA[] = {\n")
        for i in range(0, len(rgba), 32):
            f.write(",".join(str(b) for b in rgba[i:i + 32]) + ",\n")
        f.write("};\n\n")
        f.write("const IconCell ICON_CELLS[] = {\n")
        for key, col, row in key_cells:
            f.write(f'    {{"{key}", {col}, {row}}},\n')
        f.write("};\n")
        f.write(f"const int ICON_CELL_COUNT = {len(key_cells)};\n")
        f.write(f"const int LOGO_W = {LW};\n")
        f.write(f"const int LOGO_H = {LH};\n")
        f.write("const unsigned char LOGO_RGBA[] = {\n")
        for i in range(0, len(logo_rgba), 32):
            f.write(",".join(str(b) for b in logo_rgba[i:i + 32]) + ",\n")
        f.write("};\n")
        f.write("}\n")

    print(f"[overlay-icons] atlas {atlas_w}x{atlas_h} ({n} icons, {len(rgba)} bytes), "
          f"{len(key_cells)} key->cell -> {OUT_DIR}")


if __name__ == "__main__":
    main()
