#!/usr/bin/env python3
"""Generate src/generated_shared/goblin_overlay_icons.{hpp,cpp}: a small icon
atlas (one representative map-icon per show_* category) + an ini-key -> atlas-cell
table, embedded as raw RGBA so the in-game config overlay can draw the real
category icon next to each toggle (ImGui::Image), with no runtime image decode.

Mapping is DERIVED from the actual source (no hand-maintained dict):
  ini key  -> config var   (from goblin_config_schema.cpp B(...)/BE(...))
  config var -> Category    (from goblin_inject.cpp is_category_enabled switch)
  Category -> iconId        (STATIC, from tools/map_categories.py via icon_registry)
Icon images come from the custom PNGs in map_categories (png column). Everything is
profile-INDEPENDENT (no per-profile baked data read), so the generated_shared output
is the same complete superset for all four builds. No gfx involved.
"""
import os, re, sys, subprocess, collections, tempfile, json
sys.path.insert(0, os.path.dirname(__file__))
import config
from PIL import Image

PROJ = config.PROJECT_DIR
SRC = PROJ / "src"
OUT_DIR = SRC / "generated_shared"
CELL = 40          # atlas cell px (square)
COLS = 10          # atlas columns

# Manual ini-key -> iconId overrides (applied after the derived mapping). For keys
# whose representative icon is unhelpful or that aren't a category at all. iconIds
# come from icon_registry (no raw numbers), so this tracks seeded OR auto mode:
#   interactables = blue seal-puzzle icon (most recognizable interactable)
#   anon = our gray "?" anon icon (ANON_ICON_ID)
import icon_registry as _ir
KEY_ICON_OVERRIDE = {
    "show_interactables": _ir.iconid("interactables"),
    "anonymous_loot": _ir.iconid("anon"),
}

# Custom hand-drawn icon art comes from map_categories.py (the png column). Used VERBATIM (no gfx
# render, no tint) for BOTH outputs (map lossless tags + overlay atlas) via icon_image(). The iconId
# is the slug's registry id - no raw numbers, no fragile baked-data resolution.
ICON_ART_DIR = PROJ / "assets" / "map_icons" / "custom"  # committed production icon art (map_categories png column)

_icon_override = None
def icon_override():
    """iconId -> custom PNG path, straight from map_categories (slug -> png -> iconid). Cached."""
    global _icon_override
    if _icon_override is None:
        _icon_override = {}
        for slug in _ir.all_slugs():
            png = _ir.png_for(slug)
            if png:
                _icon_override[_ir.iconid(slug)] = ICON_ART_DIR / png
    return _icon_override


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
    """Category enum -> iconId, built STATICALLY from map_categories (profile-independent). Was: the
    most-common iconId in the baked per-profile map data, which made generated_shared ERR-specific."""
    import map_categories, generate_data
    out = {}
    for name, slug, _png in map_categories.CATEGORIES:
        cat = generate_data.CATEGORY_MAP.get(name)
        if not cat:  # generate_data's auto-category fallback for unmapped files
            cat = re.sub(r'[^A-Za-z0-9]', '', name.replace(' - ', '_').replace(' ', '_'))
        out[cat] = _ir.iconid(slug)
    return out


def icon_image(icon_id, render_cache=None):
    """RGBA Image for an iconId from its committed custom PNG (map_categories png column). Every map
    icon is custom now - no gfx render, no composed/by_iconId fallback."""
    ov = icon_override().get(icon_id)
    if ov and ov.exists():
        return Image.open(ov).convert("RGBA")
    return None


def render_icons(icon_ids, render_cache=None):
    """No gfx render. Warns about any needed iconId lacking a committed PNG (every category should
    have one in map_categories.py). Kept as a stub so callers don't change."""
    missing = [i for i in (icon_ids or []) if icon_image(i) is None]
    if missing:
        print(f"[overlay-icons] WARN no PNG art for iconIds {missing} "
              f"(add its png in map_categories.py / {ICON_ART_DIR})")


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
    override_icons = set(KEY_ICON_OVERRIDE.values()) | set(icon_override().keys())  # drawn art: no plate-zoom crop
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

    # assign each unique iconId a cell; build the atlas. Icon art is the committed custom PNGs
    # (map_categories png column), used verbatim - no tint/color-transform.
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
    # cell index -> source iconId (so the overlay can fill each atlas cell at runtime by decoding the
    # SHARED lossless tag for that iconId - no embedded ATLAS_RGBA; map + menu share one icon source).
    cell_src = [0] * n
    for icon, idx in icon_to_idx.items():
        cell_src[idx] = icon

    (OUT_DIR / "goblin_overlay_icons.hpp").write_text(
        "#pragma once\n"
        "// Generated by tools/generate_overlay_icons.py. Atlas LAYOUT for the in-game config overlay.\n"
        "// The pixels are NOT embedded here: the overlay builds the atlas at runtime by decoding the\n"
        "// shared DefineBitsLossless2 icon tags (generated_shared/goblin_map_icons) into each cell, so\n"
        "// the map and the menu draw from ONE embedded icon source. CELL_SRC_ICON[cell] = its srcIconId.\n\n"
        "namespace goblin::overlay_icons\n{\n"
        "    extern const int ATLAS_W;\n"
        "    extern const int ATLAS_H;\n"
        "    extern const int CELL;\n"
        "    extern const int CELL_SRC_ICON[]; // ATLAS_CELL_COUNT entries: cell index -> source iconId\n"
        "    extern const int ATLAS_CELL_COUNT;\n"
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
        f.write("const int CELL_SRC_ICON[] = {" + ",".join(str(c) for c in cell_src) + "};\n")
        f.write(f"const int ATLAS_CELL_COUNT = {n};\n")
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

    print(f"[overlay-icons] atlas {atlas_w}x{atlas_h} layout ({n} cells, NO embedded pixels), "
          f"{len(key_cells)} key->cell -> {OUT_DIR}")


if __name__ == "__main__":
    main()
