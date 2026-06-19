#!/usr/bin/env python3
"""Generate World - Kindling Spirits.MASSEDIT from kindling_spirits.json.

ERR drops 5 KindlingSpirit_* SFX-regions in m60_45_37_00. Picking up all
five between two Site-of-Grace rests grants Incantation 6610 ("Kindling
Spirit") via ItemLot 1045370500 - engine auto-sets flag 1045377500 on
award.

Per-spirit collection state lives only in the in-game SFX manager (RAM);
the save knows only an aggregate 4-bit counter that resets on rest. So the
generator emits a static per-spirit `WorldMapPointParam` row with
`textDisableFlagId1 = 1045377500` (permanent acquired-incantation flag) as
a fallback hide path. The DLL's `goblin::kindling::refresh()` does the
in-run hiding by reading SFX-region state and writing `areaNo = 99` on
collected rows - same trick as material_nodes but keyed off SFX state
instead of CSWorldGeomMan.

Output: data/massedit_generated/World - Kindling Spirits.MASSEDIT
        + _slots.json with entity_id metadata for the DLL.
"""

import json
from pathlib import Path

from massedit_common import OUT_DIR


# textId routing (see goblin_messages.cpp): goods_id + 500_000_000 → GoodsName FMG
GOODS_NAME_OFFSET = 500_000_000
KINDLING_GOODS_ID = 6610

# Permanent flag set by engine when the player receives ItemLot 1045370500
# (Goods 6610). Once ON, all 5 markers must stay hidden forever.
PERMANENT_FLAG = 1045377500

# Row IDs: keep well clear of other generators (graces use 7M, summoning
# pools 8.7M, material nodes 6M). Use 8.8M block - close to summoning
# pools without overlap.
START_ROW_ID = 8800000

# Icon 385 = Incantation (matches Magic - Incantations.MASSEDIT). The
# award is Goods 6610 "Kindling Spirit" incantation, so this category
# belongs visually with incantation pickups, not gathering nodes (which
# use iconId 397).
ICON_ID = 385


def main():
    project_dir = Path(__file__).parent.parent
    data_path = project_dir / "data" / "kindling_spirits.json"
    out_dir = OUT_DIR
    out_dir.mkdir(parents=True, exist_ok=True)

    if not data_path.exists():
        print(f"ERROR: {data_path} not found")
        return 1

    with open(data_path, encoding="utf-8") as f:
        data = json.load(f)

    spirits = data["spirits"]
    print(f"Loaded {len(spirits)} kindling spirits")

    text_id = KINDLING_GOODS_ID + GOODS_NAME_OFFSET   # 500006610 → "Kindling Spirit"

    lines = []
    slots = {}
    for i, sp in enumerate(spirits):
        rid = START_ROW_ID + i
        area = int(sp["areaNo"])
        gx = int(sp["gridXNo"])
        gz = int(sp["gridZNo"])
        x = float(sp["x"])
        y = float(sp["y"])
        z = float(sp["z"])

        # m60 = base-game overworld → dispMask00
        lines.append(f"param WorldMapPointParam: id {rid}: iconId: = {ICON_ID};")
        lines.append(f"param WorldMapPointParam: id {rid}: dispMask00: = 1;")
        lines.append(f"param WorldMapPointParam: id {rid}: areaNo: = {area};")
        lines.append(f"param WorldMapPointParam: id {rid}: gridXNo: = {gx};")
        lines.append(f"param WorldMapPointParam: id {rid}: gridZNo: = {gz};")
        lines.append(f"param WorldMapPointParam: id {rid}: posX: = {x:.3f};")
        lines.append(f"param WorldMapPointParam: id {rid}: posY: = {y:.3f};")
        lines.append(f"param WorldMapPointParam: id {rid}: posZ: = {z:.3f};")
        lines.append(f"param WorldMapPointParam: id {rid}: textId1: = {text_id};")
        # Permanent fallback: hide all 5 once incantation is acquired.
        # In-run per-spirit hide is driven by goblin::kindling::refresh().
        lines.append(f"param WorldMapPointParam: id {rid}: textDisableFlagId1: = {PERMANENT_FLAG};")
        lines.append(f"param WorldMapPointParam: id {rid}: selectMinZoomStep: = 1;")

        # Pass entity_id and slot to the DLL via the side-car JSON. The
        # DLL reads MAP_ENTRY metadata to build its tracking table - same
        # mechanism material_nodes uses for `geom_slot`/`object_name`.
        # We repurpose `name_suffix` to hold the SFX entity_id (it fits
        # in int16 if we mask, but to avoid range issues we encode the
        # spirit slot 1..5 there and the full entity_id in object_name).
        slots[str(rid)] = {
            "geom_slot": int(sp["slot"]),         # 1..5 (kindling-spirit slot)
            "name_suffix": int(sp["entity_id"] & 0xFFFF),  # low 16 bits of entity_id (informational)
            "object_name": f"KindlingSpirit_{int(sp['slot']):04d}",  # parsable name
        }

    massedit_path = out_dir / "World - Kindling Spirits.MASSEDIT"
    with open(massedit_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print(f"Written {len(spirits)} entries to {massedit_path.name}")

    slots_path = out_dir / "World - Kindling Spirits_slots.json"
    with open(slots_path, "w", encoding="utf-8") as f:
        json.dump(slots, f, indent=2)
    print(f"Written {len(slots)} slot entries to {slots_path.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
