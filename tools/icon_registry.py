#!/usr/bin/env python3
"""Custom map-icon iconIds, derived from the single category table in map_categories.py.

Generators ask `iconid("<slug>")` or `iconid_for_name("<MASSEDIT name>")`; nothing hard-codes a frame
number. The iconId is just a lookup key the DLL remaps to a runtime-injected frame (value-agnostic;
we ship no gfx) - so its VALUE only has to be unique and < ICON_MAP_SIZE=1024.

Everything here is computed from map_categories.CATEGORIES (one row per marker file: name, icon slug,
png) + EXTRA_ICONS (icons with no marker file, e.g. anon "?"):
  * ICON_ORDER  = distinct slugs in table order  -> iconid(slug) = FIRST_CUSTOM + position
  * shared icon = two rows with the SAME slug (e.g. Kindling Spirits reuses 'incantations') -> one id
  * png_for(slug) = the table's png column
Add/relayer/rename a category by editing map_categories.py only.
"""
import re as _re
import map_categories as _mc

FIRST_CUSTOM = 369  # numbering start; value arbitrary (iconIds are remapped keys). Hard limit:
                    # FIRST_CUSTOM + len(ICON_ORDER) < ICON_MAP_SIZE=1024 (DLL g_icon_iid array).

# distinct icon slugs in table order (category slugs first, then file-less extras)
ICON_ORDER = []
_seen = set()
for _f, _slug, _png in _mc.CATEGORIES:
    if _slug not in _seen:
        _seen.add(_slug); ICON_ORDER.append(_slug)
for _slug, _png in _mc.EXTRA_ICONS:
    if _slug not in _seen:
        _seen.add(_slug); ICON_ORDER.append(_slug)

# iconId index is by SORTED slug name, NOT table position - so reordering rows in map_categories.py
# (which sets z-order via row_id_registry) does NOT renumber iconIds. iconId is a value-agnostic remap
# key, so a stable order just has to be deterministic and reorder-proof; sorted(slug) is both. This
# keeps the always-fresh icon tags (gen_shared) and the pipeline-baked markers agreeing on slug->iconId
# even when they rebuild on different cadences. (Adding/removing a slug still renumbers - but that's a
# full rebuild anyway.)
_index = {s: i for i, s in enumerate(sorted(ICON_ORDER))}
_NAME_TO_SLUG = {f: s for f, s, _ in _mc.CATEGORIES}          # MASSEDIT file -> icon slug
_PNG = {}                                                     # slug -> png basename (None = composed)
for _f, _slug, _png in _mc.CATEGORIES:
    if _png and _slug not in _PNG:
        _PNG[_slug] = _png
for _slug, _png in _mc.EXTRA_ICONS:
    if _png:
        _PNG[_slug] = _png


def _norm(name):
    """Normalize a human feature name ('Loot - Smithing Stones (Low)') to a slug. Fallback only -
    map_categories' explicit slug column is authoritative for known names."""
    s = name.split(" - ", 1)[-1].lower().replace("(", "").replace(")", "")
    return _re.sub(r"[^a-z0-9]+", "_", s).strip("_")


def iconid(slug):
    """slug -> iconId (FIRST_CUSTOM + sorted-slug position; reorder-proof). Raises on unknown slug."""
    if slug not in _index:
        raise KeyError(f"icon_registry: unknown icon slug '{slug}' (add it in map_categories.py)")
    return FIRST_CUSTOM + _index[slug]


def iconid_for_name(name):
    """MASSEDIT/category human name -> iconId (via the table; _norm fallback for unknown names)."""
    return iconid(_NAME_TO_SLUG.get(name) or _norm(name))


def png_for(slug):
    """Custom png basename for a slug, or None (composed-render fallback)."""
    return _PNG.get(slug)


def all_slugs():
    return list(ICON_ORDER)


if __name__ == "__main__":
    print(f"{len(ICON_ORDER)} icons, iconIds {FIRST_CUSTOM}..{FIRST_CUSTOM + len(ICON_ORDER) - 1}")
    for s in ICON_ORDER:
        print(f"  {iconid(s):4}  {s:24}  {png_for(s) or '(composed)'}")
