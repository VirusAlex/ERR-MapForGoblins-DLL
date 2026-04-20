#!/usr/bin/env python3
"""
Generate C++ source files from MASSEDIT data.

Parses all .MASSEDIT files to create WorldMapPointParam entries;
also emits dungeon→overworld conversion table from WorldMapLegacyConvParam.

Output:
  - src/generated/goblin_map_data.cpp  (param entries + category enum)
  - src/generated/goblin_legacy_conv.hpp (dungeon coord conversion)

Localization is handled by the DLL at runtime via FMG offset-encoding
(textId = real_id + category_offset), so no text compilation step is needed.
"""

import os
import re
import json
import sys
from collections import defaultdict
from pathlib import Path

# Category mapping: MASSEDIT filename prefix -> Category enum
CATEGORY_MAP = {
    "Equipment - Armaments": "EquipArmaments",
    "Equipment - Armour": "EquipArmour",
    "Equipment - Ashes of War": "EquipAshesOfWar",
    "Equipment - Spirits": "EquipSpirits",
    "Equipment - Talismans": "EquipTalismans",
    "Key - Celestial Dew": "KeyCelestialDew",
    "Key - Cookbooks": "KeyCookbooks",
    "Key - Crystal Tears": "KeyCrystalTears",
    "Key - Great Runes": "KeyGreatRunes",
    "Key - Imbued Sword Keys": "KeyImbuedSwordKeys",
    "Key - Larval Tears": "KeyLarvalTears",
    "Key - Lost Ashes": "KeyLostAshes",
    "Key - Pots n Perfumes": "KeyPotsNPerfumes",
    "Key - Scadutree Fragments": "KeyScadutreeFragments",
    "Key - Seeds Tears Ashes": "KeySeedsTears",
    "Key - Whetblades": "KeyWhetblades",
    "Loot - Ammo": "LootAmmo",
    "Loot - Bell-Bearings": "LootBellBearings",
    "Loot - Consumables": "LootConsumables",
    "Loot - Crafting Materials": "LootCraftingMaterials",
    "Loot - Gestures": "LootGestures",
    "Loot - Gloveworts": "LootGloveworts",
    "Loot - Golden Runes": "LootGoldenRunes",
    "Loot - Golden Runes (Low)": "LootGoldenRunesLow",
    "Loot - Great Gloveworts": "LootGreatGloveworts",
    "Loot - Greases": "LootGreases",
    "Loot - Material Nodes": "LootMaterialNodes",
    "Loot - MP-Fingers": "LootMPFingers",
    "Loot - Prattling Pates": "LootPrattlingPates",
    "Loot - Rada Fruit": "LootRadaFruit",
    "Loot - Reusables": "LootReusables",
    "Loot - Smithing Stones": "LootSmithingStones",
    "Loot - Smithing Stones (Low)": "LootSmithingStonesLow",
    "Loot - Smithing Stones (Rare)": "LootSmithingStonesRare",
    "Loot - Stat Boosts": "LootStatBoosts",
    "Loot - Stonesword Keys": "LootStoneswordKeys",
    "Loot - Throwables": "LootThrowables",
    "Loot - Rune Arcs": "LootRuneArcs",
    "Loot - Dragon Hearts": "LootDragonHearts",
    "Loot - Utilities": "LootUtilities",
    "Magic - Incantations": "MagicIncantations",
    "Magic - Memory Stones": "MagicMemoryStones",
    "Magic - Prayerbooks": "MagicPrayerbooks",
    "Magic - Sorceries": "MagicSorceries",
    "Quest - Deathroot": "QuestDeathroot",
    "Quest - Progression": "QuestProgression",
    "Quest - Seedbed Curses": "QuestSeedbedCurses",
    "Reforged - Ember Pieces": "ReforgedEmberPieces",
    "Reforged - Fortunes": "ReforgedFortunes",
    "Reforged - Items": "ReforgedItemsAndChanges",
    "Reforged - Rune Pieces": "ReforgedRunePieces",
    "Reforged - Sealed Curios": "ReforgedItemsAndChanges",
    "World - Bosses": "WorldBosses",
    "World - Graces": "WorldGraces",
    "World - Hostile NPC": "WorldHostileNPC",
    "World - Imp Statues": "WorldImpStatues",
    "World - Maps": "WorldMaps",
    "World - Paintings": "WorldPaintings",
    "World - Spirit Springs": "WorldSpiritSprings",
    "World - Spiritspring Hawks": "WorldSpiritspringHawks",
    "World - Stakes of Marika": "WorldStakesOfMarika",
    "World - Summoning Pools": "WorldSummoningPools",
}

# MASSEDIT field name -> C++ struct field name and type
# Types: u16=unsigned short, u32=unsigned int, i32=int, f=float, u8=unsigned char, bit=bitfield
FIELD_MAP = {
    "iconId": ("iconId", "u16"),
    "dispMask00": ("dispMask00", "bit"),
    "dispMask01": ("dispMask01", "bit"),
    "dispMinZoomStep": ("dispMinZoomStep", "u8"),
    "areaNo": ("areaNo", "u8"),
    "gridXNo": ("gridXNo", "u8"),
    "gridZNo": ("gridZNo", "u8"),
    "posX": ("posX", "f"),
    "posY": ("posY", "f"),
    "posZ": ("posZ", "f"),
    "textId1": ("textId1", "i32"),
    "textId2": ("textId2", "i32"),
    "textId3": ("textId3", "i32"),
    "textId4": ("textId4", "i32"),
    "textId5": ("textId5", "i32"),
    "textId6": ("textId6", "i32"),
    "textId7": ("textId7", "i32"),
    "textId8": ("textId8", "i32"),
    "textEnableFlagId1": ("textEnableFlagId1", "u32"),
    "textEnableFlagId2": ("textEnableFlagId2", "u32"),
    "textEnableFlagId4": ("textEnableFlagId4", "u32"),
    "textEnableFlagId5": ("textEnableFlagId5", "u32"),
    "textDisableFlagId1": ("textDisableFlagId1", "u32"),
    "textDisableFlagId2": ("textDisableFlagId2", "u32"),
    "textDisableFlagId3": ("textDisableFlagId3", "u32"),
    "textDisableFlagId4": ("textDisableFlagId4", "u32"),
    "textDisableFlagId5": ("textDisableFlagId5", "u32"),
    "textDisableFlagId6": ("textDisableFlagId6", "u32"),
    "textDisableFlagId7": ("textDisableFlagId7", "u32"),
    "textDisableFlagId8": ("textDisableFlagId8", "u32"),
    "selectMinZoomStep": ("selectMinZoomStep", "u8"),
    "eventFlagId": ("eventFlagId", "u32"),
    "clearedEventFlagId": ("clearedEventFlagId", "u32"),
    "textType2": ("textType2", "u8"),
    "textType3": ("textType3", "u8"),
    # pad2_0 is a 6-bit field at bits 2-7 of byte 0x18
    # value 1 = bit 2 set = dispMask02 in our struct (DLC map layer)
    "pad2_0": ("dispMask02", "bit"),
    # unkC0-unkDC map to textEnableFlag2Id1-8
    "unkC0": ("textEnableFlag2Id1", "i32"),
    "unkC4": ("textEnableFlag2Id2", "i32"),
    "unkC8": ("textEnableFlag2Id3", "i32"),
    "unkCC": ("textEnableFlag2Id4", "i32"),
    "unkD0": ("textEnableFlag2Id5", "i32"),
    "unkD4": ("textEnableFlag2Id6", "i32"),
    "unkD8": ("textEnableFlag2Id7", "i32"),
    "unkDC": ("textEnableFlag2Id8", "i32"),
}

# C++ struct field order (must match WORLD_MAP_POINT_PARAM_ST declaration)
CPP_FIELD_ORDER = [
    "eventFlagId", "distViewEventFlagId", "iconId", "bgmPlaceType",
    "isAreaIcon", "isOverrideDistViewMarkPos", "isEnableNoText",
    "areaNo_forDistViewMark", "gridXNo_forDistViewMark", "gridZNo_forDistViewMark",
    "clearedEventFlagId",
    "dispMask00", "dispMask01", "dispMask02",
    "distViewIconId", "angle",
    "areaNo", "gridXNo", "gridZNo",
    "posX", "posY", "posZ",
    "textId1", "textEnableFlagId1", "textDisableFlagId1",
    "textId2", "textEnableFlagId2", "textDisableFlagId2",
    "textId3", "textEnableFlagId3", "textDisableFlagId3",
    "textId4", "textEnableFlagId4", "textDisableFlagId4",
    "textId5", "textEnableFlagId5", "textDisableFlagId5",
    "textId6", "textEnableFlagId6", "textDisableFlagId6",
    "textId7", "textEnableFlagId7", "textDisableFlagId7",
    "textId8", "textEnableFlagId8", "textDisableFlagId8",
    "textType1", "textType2", "textType3", "textType4",
    "textType5", "textType6", "textType7", "textType8",
    "distViewId", "posX_forDistViewMark", "posY_forDistViewMark", "posZ_forDistViewMark",
    "distViewId1", "distViewId2", "distViewId3",
    "dispMinZoomStep", "selectMinZoomStep", "entryFEType",
    "textEnableFlag2Id1", "textEnableFlag2Id2", "textEnableFlag2Id3", "textEnableFlag2Id4",
    "textEnableFlag2Id5", "textEnableFlag2Id6", "textEnableFlag2Id7", "textEnableFlag2Id8",
    "textDisableFlag2Id1", "textDisableFlag2Id2", "textDisableFlag2Id3", "textDisableFlag2Id4",
    "textDisableFlag2Id5", "textDisableFlag2Id6", "textDisableFlag2Id7", "textDisableFlag2Id8",
]

# Fields to skip
SKIP_FIELDS = {"Name", "pad4"}


def parse_massedit_files(massedit_dir):
    """Parse all .MASSEDIT files and return dict of {row_id: {field: value, ..., '_category': str}}"""
    entries = defaultdict(dict)

    for filepath in sorted(Path(massedit_dir).glob("*.MASSEDIT")):
        filename = filepath.stem
        category = CATEGORY_MAP.get(filename)
        if category is None:
            # Auto-generate category name from filename
            category = re.sub(r'[^A-Za-z0-9]', '', filename.replace(' - ', '_').replace(' ', '_'))
            print(f"INFO: Auto-category for '{filename}' -> {category}")

        # Parse: param WorldMapPointParam: id XXXXX: fieldName: = value;
        pattern = re.compile(
            r"param\s+WorldMapPointParam:\s+id\s+(\d+):\s+(\w+):\s*=\s*(.+);"
        )

        with open(filepath, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                m = pattern.match(line)
                if m:
                    row_id = int(m.group(1))
                    field = m.group(2)
                    value = m.group(3).strip()

                    if field in SKIP_FIELDS:
                        continue

                    if field not in FIELD_MAP:
                        print(f"WARNING: Unknown field '{field}' in {filename}, skipping")
                        continue

                    entries[row_id][field] = value
                    entries[row_id]["_category"] = category

    # Fix interior areas that don't display correctly on overworld
    # e.g. m11_10 (Roundtable Hold) — MSB coords land in ocean
    # Shift all coords to match grace display positions (area stays unchanged)
    # m11_10: MSB ~(-305,-298) → grace display ~(-2500,-650) = offset (-2195,-352)
    COORD_SHIFTS = {(11, 10): (-2195.0, -352.0)}
    shifted = 0
    for row_id, fields in entries.items():
        area = int(fields.get("areaNo", "0"))
        gx = int(fields.get("gridXNo", "0"))
        shift = COORD_SHIFTS.get((area, gx))
        if shift:
            dx, dz = shift
            fields["posX"] = f"{float(fields.get('posX', '0')) + dx:.3f}"
            fields["posZ"] = f"{float(fields.get('posZ', '0')) + dz:.3f}"
            shifted += 1
    if shifted > 0:
        print(f"  Shifted {shifted} entries (interior coord fix)")

    return entries


def format_value(cpp_field, cpp_type, raw_value):
    if cpp_type == "f":
        v = raw_value.rstrip(";").strip()
        if "." not in v:
            v += ".f"
        else:
            v += "f"
        return v
    elif cpp_type in ("u32", "u16", "u8"):
        return str(int(float(raw_value)))
    elif cpp_type == "i32":
        return str(int(float(raw_value)))
    elif cpp_type == "bit":
        v = int(float(raw_value))
        return "true" if v else "false"
    elif cpp_type == "arr1":
        v = int(float(raw_value))
        return "{" + str(v) + "}"
    return raw_value


def load_piece_metadata(massedit_dir):
    """Load geom_slot and name_suffix from *_slots.json files.
    Returns dict: row_id (int) -> {geom_slot: int, name_suffix: int}."""
    meta = {}
    for path in Path(massedit_dir).glob("*_slots.json"):
        with open(path) as f:
            data = json.load(f)
        for row_id_str, val in data.items():
            if isinstance(val, dict):
                meta[int(row_id_str)] = val
            else:
                # Legacy format: just geom_slot as int
                meta[int(row_id_str)] = {'geom_slot': val, 'name_suffix': -1}
    return meta


def generate_map_data_cpp(entries, output_path, geom_slots=None):
    """Generate goblin_map_data.cpp with all param entries."""
    if geom_slots is None:
        geom_slots = {}

    # Sort entries by row_id
    sorted_ids = sorted(entries.keys())

    with open(output_path, "w", encoding="utf-8") as f:
        f.write("// AUTO-GENERATED FILE - DO NOT EDIT\n")
        f.write("// Generated by tools/generate_data.py from MASSEDIT files\n\n")
        f.write('#include "goblin_map_data.hpp"\n\n')
        f.write("namespace goblin::generated\n{\n\n")

        f.write(f"const size_t MAP_ENTRY_COUNT = {len(sorted_ids)};\n\n")
        f.write("const MapEntry MAP_ENTRIES[] = {\n")

        for row_id in sorted_ids:
            fields = entries[row_id]
            category = fields.get("_category", "World")

            f.write(f"    // Row ID {row_id}\n")
            f.write(f"    {{{row_id}ull, {{\n")

            field_dict = {}
            for massedit_field, raw_value in fields.items():
                if massedit_field.startswith("_"):
                    continue
                if massedit_field in SKIP_FIELDS:
                    continue
                cpp_field, cpp_type = FIELD_MAP[massedit_field]
                formatted = format_value(cpp_field, cpp_type, raw_value)
                field_dict[cpp_field] = formatted

            field_assignments = []
            for cpp_field in CPP_FIELD_ORDER:
                if cpp_field in field_dict:
                    field_assignments.append((cpp_field, field_dict[cpp_field]))

            for cpp_field, formatted in field_assignments:
                f.write(f"        .{cpp_field} = {formatted},\n")

            meta = geom_slots.get(row_id, {})
            slot = meta.get('geom_slot', -1) if isinstance(meta, dict) else meta
            suffix = meta.get('name_suffix', -1) if isinstance(meta, dict) else -1
            obj_name = meta.get('object_name', '') if isinstance(meta, dict) else ''
            if obj_name:
                f.write(f'    }}, Category::{category}, {slot}, {suffix}, "{obj_name}"}},\n')
            else:
                f.write(f"    }}, Category::{category}, {slot}, {suffix}, nullptr}},\n")

        f.write("};\n\n")
        f.write("} // namespace goblin::generated\n")

    print(f"Generated {output_path} with {len(sorted_ids)} entries")


def main():
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--massedit-dir", type=str, default=None,
                        help="Path to MASSEDIT directory (default: data/massedit)")
    args = parser.parse_args()

    script_dir = Path(__file__).parent
    project_dir = script_dir.parent

    if args.massedit_dir:
        massedit_dir = Path(args.massedit_dir)
    else:
        massedit_dir = project_dir / "data" / "massedit"
    output_dir = project_dir / "src" / "generated"

    output_dir.mkdir(parents=True, exist_ok=True)

    print("=== Parsing MASSEDIT files ===")
    entries = parse_massedit_files(massedit_dir)
    print(f"Total unique entries: {len(entries)}")

    # De-overlap: shift icons that share exact coordinates using a square spiral
    print("\n=== De-overlapping icons ===")
    SPIRAL_STEP = 8.0  # world units per spiral step
    coord_groups = {}  # (areaNo, gridX, gridZ, posX_round, posZ_round) -> [row_ids]
    for row_id, fields in entries.items():
        area = fields.get("areaNo", "0")
        gx = fields.get("gridXNo", "0")
        gz = fields.get("gridZNo", "0")
        px = round(float(fields.get("posX", "0")), 1)
        pz = round(float(fields.get("posZ", "0")), 1)
        key = (area, gx, gz, px, pz)
        coord_groups.setdefault(key, []).append(row_id)

    def spiral_offsets(n):
        """Generate n (dx, dz) offsets in a square spiral pattern.
        (0,0), (0,-1), (1,-1), (1,0), (1,1), (0,1), (-1,1), (-1,0), (-1,-1), (0,-2), ...
        """
        offsets = []
        x = z = 0
        d = 1  # current ring distance
        offsets.append((0, 0))
        while len(offsets) < n:
            # Up: (0,-d)
            for i in range(1, d + 1):
                if len(offsets) >= n: break
                offsets.append((x, -i + z))
            z -= d
            # Right+down diagonal to (d, 0) relative
            for i in range(1, d + 1):
                if len(offsets) >= n: break
                offsets.append((x + i, z + i))
            x += d; z += d
            # Down
            for i in range(1, d + 1):
                if len(offsets) >= n: break
                offsets.append((x, z + i))
            z += d
            # Left
            for i in range(1, 2 * d + 1):
                if len(offsets) >= n: break
                offsets.append((x - i, z))
            x -= 2 * d
            # Up
            for i in range(1, 2 * d + 1):
                if len(offsets) >= n: break
                offsets.append((x, z - i))
            z -= 2 * d
            # Right (partial, to start next ring)
            for i in range(1, d + 1):
                if len(offsets) >= n: break
                offsets.append((x + i, z))
            x += d
            d += 1
        return offsets[:n]

    shifted = 0
    for key, row_ids in coord_groups.items():
        if len(row_ids) <= 1:
            continue
        offsets = spiral_offsets(len(row_ids))
        for i, row_id in enumerate(row_ids):
            if i == 0:
                continue  # first stays in place
            ox, oz = offsets[i]
            old_px = float(entries[row_id].get("posX", "0"))
            old_pz = float(entries[row_id].get("posZ", "0"))
            entries[row_id]["posX"] = f"{old_px + ox * SPIRAL_STEP:.3f}"
            entries[row_id]["posZ"] = f"{old_pz + oz * SPIRAL_STEP:.3f}"
            shifted += 1

    print(f"  Shifted {shifted} entries across {sum(1 for v in coord_groups.values() if len(v) > 1)} groups")

    print("\n=== Loading piece metadata ===")
    geom_slots = load_piece_metadata(massedit_dir)
    print(f"Loaded {len(geom_slots)} piece metadata entries")

    print("\n=== Generating map data C++ ===")
    generate_map_data_cpp(entries, output_dir / "goblin_map_data.cpp", geom_slots)

    print("\n=== Generating legacy-conv C++ ===")
    generate_legacy_conv_cpp(project_dir / "data" / "WorldMapLegacyConvParam.json",
                             output_dir / "goblin_legacy_conv.hpp")

    print("\nDone.")


def generate_legacy_conv_cpp(conv_json, output_path):
    """Emit a C++ header with the dungeon→overworld conversion table.
    Used by goblin_markers to place dungeon MAP_ENTRIES on overworld coords."""
    if not conv_json.exists():
        print(f"  WARNING: {conv_json} missing, skipping")
        return
    rows = json.load(open(conv_json, encoding='utf-8'))
    # Keep first row per (srcArea, srcGridX) that lands in 60/61
    by_key = {}
    for r in rows:
        if not isinstance(r, dict): continue
        src_area = int(r.get('srcAreaNo', 0))
        src_gx = int(r.get('srcGridXNo', 0))
        dst_area = int(r.get('dstAreaNo', 0))
        if dst_area not in (60, 61): continue
        key = (src_area, src_gx)
        if key in by_key: continue
        by_key[key] = {
            'src_area': src_area, 'src_gx': src_gx,
            'src_pos_x': float(r.get('srcPosX', 0)),
            'src_pos_z': float(r.get('srcPosZ', 0)),
            'dst_area': dst_area,
            'dst_gx': int(r.get('dstGridXNo', 0)),
            'dst_gz': int(r.get('dstGridZNo', 0)),
            'dst_pos_x': float(r.get('dstPosX', 0)),
            'dst_pos_z': float(r.get('dstPosZ', 0)),
        }
    entries = sorted(by_key.values(), key=lambda e: (e['src_area'], e['src_gx']))
    with open(output_path, 'w', encoding='utf-8') as f:
        f.write("#pragma once\n// AUTO-GENERATED — do not edit.\n")
        f.write("// Dungeon-area → overworld-tile conversion table (first base-point per src key).\n\n")
        f.write("#include <cstdint>\n#include <cstddef>\n\n")
        f.write("namespace goblin::generated {\n\n")
        f.write("struct LegacyConvEntry {\n")
        f.write("    uint8_t src_area;\n    uint8_t src_gx;\n")
        f.write("    float src_pos_x;\n    float src_pos_z;\n")
        f.write("    uint8_t dst_area;\n    uint8_t dst_gx;\n    uint8_t dst_gz;\n")
        f.write("    float dst_pos_x;\n    float dst_pos_z;\n")
        f.write("};\n\n")
        f.write(f"constexpr LegacyConvEntry LEGACY_CONV[] = {{\n")
        for e in entries:
            f.write(f"    {{ {e['src_area']}, {e['src_gx']}, {e['src_pos_x']:.3f}f, {e['src_pos_z']:.3f}f, ")
            f.write(f"{e['dst_area']}, {e['dst_gx']}, {e['dst_gz']}, ")
            f.write(f"{e['dst_pos_x']:.3f}f, {e['dst_pos_z']:.3f}f }},\n")
        f.write("};\n\n")
        f.write(f"constexpr size_t LEGACY_CONV_COUNT = {len(entries)};\n\n")
        f.write("} // namespace goblin::generated\n")
    print(f"  Generated {output_path.name} with {len(entries)} conv entries")


if __name__ == "__main__":
    main()
