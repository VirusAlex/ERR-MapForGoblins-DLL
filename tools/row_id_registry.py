#!/usr/bin/env python3
"""Single source of truth for WorldMapPointParam row-ID bases - and, with it, the
MAP Z-ORDER of our marker categories.

Why this exists
---------------
Every marker generator used to hard-code its own `row_id` base (2000000, 3000000,
8800000, ...), hand-spaced "by eye". That had two problems:
  * collisions were possible and DID happen (Graces and Kindling Spirits both sat
    at 8800000; the bake silently dropped the 5 overlapping grace rows);
  * the numbers were opaque magic with no single place to see or change them.

How z-order actually works (verified by RE, see docs/research_worldmap_build.md)
-------------------------------------------------------------------------------
The DLL injects every marker with a *fresh dynamic* row ID at runtime: it walks
our entries in MAP_ENTRIES order and assigns 1,2,3,... skipping whatever IDs the
LIVE regulation already uses (goblin_inject.cpp). So:
  * collision-with-vanilla is handled AT RUNTIME, for whatever regulation is
    actually loaded (ERR / vanilla / Convergence / ERTE / Randomizer / stacked
    mods) - the build-time number never reaches the param table; and
  * MAP_ENTRIES order == dynamic-ID order == DRAW ORDER. On overlap the engine
    draws the LOWER row ID on top. generate_data.py sorts entries by row_id, so
    the category that gets the LOWEST base renders ON TOP.

Therefore the build-time row ID is only (a) a stable join key (collected / location
names / loot linkage all key off it, regenerated together) and (b) a SORT KEY that
sets the layer. Its absolute value is irrelevant; only the ORDER below matters.

Editing the layering
--------------------
The knob is the row order of map_categories.CATEGORIES (top-of-map -> bottom). Move a row
UP to make that category draw OVER more things; DOWN to sink it. LAYER_ORDER below is just
that table's file column. Each generator asks `base("<MASSEDIT file name>")`; the registry
hands out non-overlapping blocks in this order, so collisions are impossible by construction.
"""

# Map z-order (top of map, drawn over everything -> bottom) = the ROW ORDER of
# map_categories.CATEGORIES. Reorder rows THERE to relayer; this is just its file column.
import map_categories as _mc
LAYER_ORDER = [name for name, _slug, _png in _mc.CATEGORIES]

# Per-category block size. Must exceed the largest category's marker count (the
# biggest today is Material Nodes ~1455). 100000 leaves vast headroom and keeps
# the bases human-readable (block N starts at N*STRIDE).
STRIDE = 100_000

_index = {name: i for i, name in enumerate(LAYER_ORDER)}
assert len(_index) == len(LAYER_ORDER), "row_id_registry: duplicate name in LAYER_ORDER"


def base(name):
    """Row-ID base for a MASSEDIT category (its z-order slot). Raises on unknown
    name so a typo / renamed file fails the build loudly instead of silently
    colliding. Lower base == drawn higher on the map."""
    if name not in _index:
        raise KeyError(f"row_id_registry: unknown category '{name}'. "
                       f"Add it to LAYER_ORDER (its position sets the map layer).")
    return (_index[name] + 1) * STRIDE


def all_names():
    return list(LAYER_ORDER)


if __name__ == "__main__":
    for n in LAYER_ORDER:
        print(f"{base(n):>9}  {n}")
    print(f"\n{len(LAYER_ORDER)} categories, STRIDE={STRIDE}")
