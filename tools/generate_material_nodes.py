#!/usr/bin/env python3
"""
Generate Loot - Material Nodes.MASSEDIT from MSB gathering nodes.

Scans all_gathering_nodes_final.json for one-time pickup nodes
(isEnableRepick=True AND isHiddenOnRepick=True) and generates
WorldMapPointParam MASSEDIT entries.

Output: data/massedit_generated/Loot - Material Nodes.MASSEDIT
"""

import json
import os
import sys
from pathlib import Path
from massedit_common import UNDERGROUND_AREAS, DLC_AREAS

def main():
    project_dir = Path(__file__).parent.parent
    data_dir = project_dir / "data"
    out_dir = data_dir / "massedit_generated"
    out_dir.mkdir(exist_ok=True)

    # Load mappings
    with open(data_dir / "aeg099_item_mapping.json") as f:
        aeg099 = json.load(f)
    with open(data_dir / "aeg463_item_mapping.json") as f:
        aeg463 = json.load(f)
    with open(data_dir / "all_gathering_nodes_final.json") as f:
        all_nodes = json.load(f)

    # One-time models: isEnableRepick=True AND isHiddenOnRepick=True
    onetime_models = {}
    for e in aeg099 + aeg463:
        if e.get("isEnableRepick") and e.get("isHiddenOnRepick"):
            onetime_models[e["model"]] = e

    print(f"One-time models: {len(onetime_models)}")

    # Filter nodes
    onetime_nodes = [n for n in all_nodes if n["model"] in onetime_models]
    print(f"One-time nodes in MSBs: {len(onetime_nodes)}")

    # Deduplicate: same model + same coordinates = same physical object
    # (appears in multiple MSB variants like _00 and _10)
    seen_coords = set()
    unique_nodes = []
    dupes = 0
    for n in onetime_nodes:
        key = (n["model"], round(n["x"], 3), round(n["y"], 3), round(n["z"], 3))
        if key in seen_coords:
            dupes += 1
            continue
        seen_coords.add(key)
        unique_nodes.append(n)
    print(f"Deduplicated: {dupes} duplicates removed, {len(unique_nodes)} unique nodes")
    onetime_nodes = unique_nodes

    START_ID = 6000000
    entries = []
    for n in onetime_nodes:
        area = n["area"]
        info = onetime_models[n["model"]]
        goods_id = info.get("primaryGoodsId", info.get("goodsId", 0))
        if not goods_id or goods_id == 17000:
            continue

        entry = {
            "id": START_ID + len(entries),
            "iconId": 397,
            "areaNo": area,
            "posX": round(n["x"], 3),
            "posY": round(n["y"], 3),
            "posZ": round(n["z"], 3),
            "textId1": goods_id + 500000000,  # offset-encoded to avoid PlaceName collision
            "selectMinZoomStep": 1,
        }
        if area in (60, 61):
            entry["gridXNo"] = n["p1"]
            entry["gridZNo"] = n["p2"]
            if area == 61:
                entry["pad2_0"] = 1
            else:
                entry["dispMask00"] = 1
        elif area in UNDERGROUND_AREAS:
            entry["gridXNo"] = n["p1"]
            entry["dispMask01"] = 1
        elif area in DLC_AREAS:
            entry["gridXNo"] = n["p1"]
            entry["pad2_0"] = 1
        else:
            entry["gridXNo"] = n["p1"]
            entry["dispMask00"] = 1

        entries.append(entry)

    print(f"Generated {len(entries)} MASSEDIT entries")

    # Write slots.json for geom tracking
    slots = {}
    for i, (entry, node) in enumerate(zip(entries, [n for n in onetime_nodes if onetime_models[n["model"]].get("primaryGoodsId", onetime_models[n["model"]].get("goodsId", 0)) not in (0, 17000)])):
        name = node["name"]  # e.g. "AEG099_651_9000"
        parts = name.rsplit("_", 1)
        if len(parts) == 2 and parts[1].isdigit():
            suffix = int(parts[1])
            slot = suffix - 9000
            # full_name e.g. "AEG099_651_9000"
            slots[str(entry["id"])] = {"geom_slot": slot, "name_suffix": suffix, "object_name": name}

    slots_path = out_dir / "Loot - Material Nodes_slots.json"
    with open(slots_path, "w") as f:
        json.dump(slots, f, indent=2)
    print(f"Written {len(slots)} slot entries to {slots_path}")

    # Write MASSEDIT file
    massedit_path = out_dir / "Loot - Material Nodes.MASSEDIT"
    with open(massedit_path, "w", encoding="utf-8") as f:
        for e in entries:
            rid = e["id"]
            f.write(f'param WorldMapPointParam: id {rid}: iconId: = {e["iconId"]};\n')
            if "dispMask00" in e:
                f.write(f'param WorldMapPointParam: id {rid}: dispMask00: = {e["dispMask00"]};\n')
            if "dispMask01" in e:
                f.write(f'param WorldMapPointParam: id {rid}: dispMask01: = {e["dispMask01"]};\n')
            if "pad2_0" in e:
                f.write(f'param WorldMapPointParam: id {rid}: pad2_0: = {e["pad2_0"]};\n')
            f.write(f'param WorldMapPointParam: id {rid}: areaNo: = {e["areaNo"]};\n')
            if "gridXNo" in e:
                f.write(f'param WorldMapPointParam: id {rid}: gridXNo: = {e["gridXNo"]};\n')
            if "gridZNo" in e:
                f.write(f'param WorldMapPointParam: id {rid}: gridZNo: = {e["gridZNo"]};\n')
            f.write(f'param WorldMapPointParam: id {rid}: posX: = {e["posX"]};\n')
            f.write(f'param WorldMapPointParam: id {rid}: posY: = {e["posY"]};\n')
            f.write(f'param WorldMapPointParam: id {rid}: posZ: = {e["posZ"]};\n')
            f.write(f'param WorldMapPointParam: id {rid}: textId1: = {e["textId1"]};\n')
            f.write(f'param WorldMapPointParam: id {rid}: selectMinZoomStep: = {e["selectMinZoomStep"]};\n')

    print(f"Written to {massedit_path}")

    # Summary
    from collections import Counter
    items = Counter()
    model_to_item = {e["model"]: e.get("primaryItem", "?") for e in aeg099 + aeg463}
    for n in onetime_nodes:
        if n["model"] in onetime_models:
            items[model_to_item.get(n["model"], n["model"])] += 1

    areas = Counter(e["areaNo"] for e in entries)
    print(f"\nBy area: {dict(sorted(areas.items()))}")
    print(f"Top items:")
    for item, cnt in items.most_common(10):
        print(f"  {item}: {cnt}")


if __name__ == "__main__":
    main()
