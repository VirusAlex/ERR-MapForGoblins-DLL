#!/usr/bin/env python3
"""
Generate MASSEDIT entries for Rune Pieces and Ember Pieces
from extracted JSON coordinate data.
Matches placements with ItemLotParam_map event flags for auto-hide on pickup.
"""

import csv
import json
import math
from pathlib import Path

from massedit_common import DATA_DIR, OUT_DIR as OUTPUT_DIR, OVERWORLD_AREAS, resolve_location_id
from massedit_common import DLC_AREAS, UNDERGROUND_AREAS, get_disp_mask

CSV_PATH = DATA_DIR / "ItemLotParam_map.csv"

# Alias for backward compatibility
place_name_id = resolve_location_id


def parse_map_tile(map_name):
    parts = map_name.replace(".msb", "").split("_")
    if len(parts) < 4:
        return None, None, None
    area = int(parts[0][1:])
    p1 = int(parts[1])
    p2 = int(parts[2])
    if area in (60, 61) or area in DLC_AREAS:
        return area, p1, p2
    else:
        return area, p1, 0


def load_event_flags(csv_path, goods_id):
    flags = []
    if not csv_path.exists():
        return flags
    with open(csv_path, 'r') as f:
        for row in csv.DictReader(f):
            for slot in range(1, 9):
                item_id = int(row.get(f'lotItemId0{slot}', '0') or '0')
                if item_id == goods_id:
                    flag = int(row.get('getItemFlagId', '0') or '0')
                    if flag > 0:
                        flags.append(flag)
                    break
    return flags


def generate_massedit(items, item_name, text_id, icon_id, start_row_id, output_file, event_flags=None):
    # Deduplicate: skip _10 variants (post-event duplicates)
    seen_coords = set()
    unique_items = []
    for item in items:
        key = (round(item['x'], 1), round(item['z'], 1), item['map'].split('_')[0])
        if key not in seen_coords:
            seen_coords.add(key)
            unique_items.append(item)

    print(f"  {item_name}: {len(items)} total, {len(unique_items)} unique")

    flags = list(event_flags) if event_flags else []
    flag_idx = 0

    lines = []
    row_id = start_row_id

    for item in unique_items:
        area, gridX, gridZ = parse_map_tile(item['map'])
        if area is None:
            continue

        x = item['x']
        z = item['z']

        if area in UNDERGROUND_AREAS:
            disp = "dispMask01"
        elif area in DLC_AREAS:
            disp = "pad2_0"
        else:
            disp = "dispMask00"

        lines.append(f"param WorldMapPointParam: id {row_id}: iconId: = {icon_id};")
        lines.append(f"param WorldMapPointParam: id {row_id}: {disp}: = 1;")

        lines.append(f"param WorldMapPointParam: id {row_id}: areaNo: = {area};")
        if area in (60, 61) or area in DLC_AREAS:
            lines.append(f"param WorldMapPointParam: id {row_id}: gridXNo: = {gridX};")
            lines.append(f"param WorldMapPointParam: id {row_id}: gridZNo: = {gridZ};")
        elif gridX > 0:
            lines.append(f"param WorldMapPointParam: id {row_id}: gridXNo: = {gridX};")

        lines.append(f"param WorldMapPointParam: id {row_id}: posX: = {x:.3f};")
        lines.append(f"param WorldMapPointParam: id {row_id}: posZ: = {z:.3f};")
        # Offset-encode goods ID (500M) to avoid collision with PlaceName IDs
        lines.append(f"param WorldMapPointParam: id {row_id}: textId1: = {text_id + 500000000};")

        loc_id = place_name_id(item['map'])
        if loc_id > 0:
            lines.append(f"param WorldMapPointParam: id {row_id}: textId2: = {loc_id};")

        # No auto-hide: no EntityID->ItemLot mapping available
        lines.append(f"param WorldMapPointParam: id {row_id}: selectMinZoomStep: = 1;")

        row_id += 1

    with open(output_file, 'w') as f:
        f.write('\n'.join(lines) + '\n')

    slot_map = {}
    row_id2 = start_row_id
    for item in unique_items:
        if parse_map_tile(item['map'])[0] is None:
            continue
        iid = item.get('instance_id', -1)
        name = item.get('name', '')
        parts = name.rsplit('_', 1)
        suffix = int(parts[-1]) if len(parts) == 2 and parts[-1].isdigit() else -1
        slot_map[row_id2] = {
            'geom_slot': (iid - 9000) if iid >= 9000 else -1,
            'name_suffix': suffix,
            'object_name': name
        }
        row_id2 += 1

    slot_file = output_file.parent / (output_file.stem + "_slots.json")
    with open(slot_file, 'w') as f:
        json.dump(slot_map, f)
    print(f"  Slot map: {slot_file.name} ({len(slot_map)} entries)")

    print(f"  Written {row_id - start_row_id} entries ({flag_idx} with event flags)")
    return row_id


def main():
    rune_items = json.load(open(DATA_DIR / "rune_pieces.json"))
    ember_items = json.load(open(DATA_DIR / "ember_pieces.json"))
    print(f"Loaded: {len(rune_items)} Rune Pieces, {len(ember_items)} Ember Pieces")

    rune_flags = load_event_flags(CSV_PATH, 800010)
    ember_flags = load_event_flags(CSV_PATH, 850010)
    print(f"Event flags: {len(rune_flags)} for Rune, {len(ember_flags)} for Ember")

    print("\nGenerating MASSEDIT...")

    generate_massedit(
        rune_items, "Rune Pieces",
        text_id=800010, icon_id=371,   # goodsId for "Rune Piece" / "Осколок Руны" — localized via GoodsName FMG
        start_row_id=2000000,
        output_file=OUTPUT_DIR / "Reforged - Rune Pieces.MASSEDIT",
        event_flags=rune_flags
    )

    generate_massedit(
        ember_items, "Ember Pieces",
        text_id=850010, icon_id=371,   # goodsId for "Ember Piece" — same star as Rune Pieces (distinguished by map location)
        start_row_id=3000000,
        output_file=OUTPUT_DIR / "Reforged - Ember Pieces.MASSEDIT",
        event_flags=ember_flags
    )

    print("\nRun 'py tools/generate_data.py' and rebuild DLL.")


if __name__ == "__main__":
    main()
