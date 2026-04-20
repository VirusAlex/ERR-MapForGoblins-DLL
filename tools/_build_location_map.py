#!/usr/bin/env python3
"""
One-time helper: build data/map_code_locations.json from SmithBox BonfireWarpParam.

Reads BonfireWarpParam community row names, extracts dungeon/location names
for each map prefix used by Rune/Ember Pieces, and outputs:
  - data/map_code_locations.json  (prefix -> parenthetical location name)
  - data/unknown_locations.txt    (pieces with no BonfireWarpParam match)
"""

import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import config

DATA_DIR = Path(__file__).parent.parent / "data"

SMITHBOX_BWP = config.require_smithbox_dir() / "Assets/PARAM/ER/Community Row Names/BonfireWarpParam.json"

# Prefixes with no BonfireWarpParam data — skip for now
UNKNOWN_PREFIXES = {"m10_01", "m31_08", "m31_90", "m39_20", "m42_01"}


def load_bonfire_warp_param():
    """Parse BonfireWarpParam and return {map_prefix: location_name}."""
    with open(SMITHBOX_BWP, encoding="utf-8") as f:
        data = json.load(f)

    prefix_to_location = {}
    for entry in data["Entries"]:
        eid = entry["ID"]
        name = entry["Entries"][0] if entry["Entries"] else ""
        if not name:
            continue

        m = re.match(r"\[(.+?)\]\s*(.+)", name)
        if not m:
            continue

        region = m.group(1)
        specific = m.group(2)

        area_code = eid // 10000
        sub_code = (eid % 10000) // 100
        prefix = f"m{area_code:02d}_{sub_code:02d}"

        if prefix in prefix_to_location:
            continue  # keep first grace per prefix

        # For legacy dungeons (m10-m28), use the bracket name (the dungeon itself)
        # For mini-dungeons (m30-m43), use the specific grace name (= dungeon name)
        if area_code < 30:
            prefix_to_location[prefix] = region
        else:
            prefix_to_location[prefix] = specific

    return prefix_to_location


def get_piece_prefixes():
    """Get all non-overworld map prefixes used by Rune and Ember pieces."""
    prefixes = set()
    for fname in ["rune_pieces.json", "ember_pieces.json"]:
        path = DATA_DIR / fname
        if not path.exists():
            continue
        items = json.load(open(path))
        for item in items:
            parts = item["map"].replace(".msb", "").split("_")
            area = int(parts[0][1:])
            if area in (60, 61):
                continue  # overworld
            prefix = f"{parts[0]}_{parts[1]}"
            prefixes.add(prefix)
    return prefixes


def write_unknown_locations(unknown_prefixes):
    """Write coordinates of pieces with unknown locations to a text file."""
    lines = ["# Pieces with unknown location names (no BonfireWarpParam data)", ""]
    for fname in ["rune_pieces.json", "ember_pieces.json"]:
        path = DATA_DIR / fname
        if not path.exists():
            continue
        items = json.load(open(path))
        for item in items:
            parts = item["map"].replace(".msb", "").split("_")
            prefix = f"{parts[0]}_{parts[1]}"
            if prefix in unknown_prefixes:
                lines.append(
                    f"map={item['map']}  x={item['x']:.1f}  y={item['y']:.1f}  "
                    f"z={item['z']:.1f}  model={item['model']}  name={item.get('name','')}"
                )
    out = DATA_DIR / "unknown_locations.txt"
    with open(out, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"  Written {out.name} ({len(lines) - 2} pieces)")


def main():
    print("=== Building location map ===")

    bwp = load_bonfire_warp_param()
    print(f"  BonfireWarpParam: {len(bwp)} map prefixes")

    piece_prefixes = get_piece_prefixes()
    print(f"  Piece prefixes (non-overworld): {len(piece_prefixes)}")

    # Build location map: only known prefixes, parenthetical format
    location_map = {}
    unknown = set()
    for prefix in sorted(piece_prefixes):
        if prefix in UNKNOWN_PREFIXES:
            unknown.add(prefix)
            continue
        name = bwp.get(prefix)
        if name:
            location_map[prefix] = f"({name})"
        else:
            print(f"  WARNING: {prefix} not in BonfireWarpParam and not in UNKNOWN list")
            unknown.add(prefix)

    out_path = DATA_DIR / "map_code_locations.json"
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(location_map, f, indent=2, ensure_ascii=False)
    print(f"\n  Written {out_path.name}: {len(location_map)} entries")

    if unknown:
        print(f"\n  Unknown prefixes ({len(unknown)}): {', '.join(sorted(unknown))}")
        write_unknown_locations(unknown)

    # Summary
    unique_names = sorted(set(location_map.values()))
    print(f"\n  Unique location names: {len(unique_names)}")
    for name in unique_names:
        count = sum(1 for v in location_map.values() if v == name)
        print(f"    {name} ({count} prefixes)")


if __name__ == "__main__":
    main()
