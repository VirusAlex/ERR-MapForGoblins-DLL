#!/usr/bin/env python3
"""
Generate C++ source files from MASSEDIT and FMG JSON data.

Parses all .MASSEDIT files to create WorldMapPointParam entries,
and TitleLocations.fmg.json files for localized text.

Output:
  - src/generated/goblin_map_data.cpp  (param entries)
  - src/generated/goblin_text_data.cpp (localized text)
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
    "Key - Imbued Sword Keys": "KeyImbuedSwordKeys",
    "Key - Larval Tears": "KeyLarvalTears",
    "Key - Lost Ashes": "KeyLostAshes",
    "Key - Pots n Perfumes": "KeyPotsNPerfumes",
    "Key - Seeds,Tears,Scadu,Ashes": "KeySeedsTears",
    "Key - Whetblades": "KeyWhetblades",
    "Loot - Ammo": "LootAmmo",
    "Loot - Bell-Bearings": "LootBellBearings",
    "Loot - Consumables": "LootConsumables",
    "Loot - Crafting Materials": "LootCraftingMaterials",
    "Loot - MP-Fingers, Gestures, Pates": "LootMPFingers",
    "Loot - Material Nodes (DOES NOT DISAPPEAR)": "LootMaterialNodes",
    "Loot - Reusables (Veil,Shackle)": "LootReusables",
    "Loot - Somber_Scarab": "LootSomberScarab",
    "Loot - Stonesword_Keys": "LootStoneswordKeys",
    "Loot - Unique_Drops": "LootUniqueDrops",
    "Magic - Incantations": "MagicIncantations",
    "Magic - Memory Stones": "MagicMemoryStones",
    "Magic - Sorceries": "MagicSorceries",
    "Quest - Deathroot": "QuestDeathroot",
    "Quest - Progression": "QuestProgression",
    "Quest - Seedbed Curses": "QuestSeedbedCurses",
    "Reforged - camp contents": "ReforgedCampContents",
    "Reforged - Ember Pieces": "ReforgedEmberPieces",
    "Reforged - items and changes": "ReforgedItemsAndChanges",
    "Reforged - Rune Pieces": "ReforgedRunePieces",
    "World - Graces": "WorldGraces",
    "World - Hostile NPC": "WorldHostileNPC",
    "World - Imp Statues": "WorldImpStatues",
    "World - Paintings": "WorldPaintings",
    "World - Spirit_Springs": "WorldSpiritSprings",
    "World - Spiritspring_Hawks": "WorldSpiritspringHawks",
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

# Steam language name -> folder name in msg/
LANG_FOLDER_MAP = {
    "english": "engus",
    "german": "deude",
    "french": "frafr",
    "italian": "itait",
    "japanese": "jpnjp",
    "koreana": "korkr",
    "polish": "polpl",
    "brazilian": "porbr",
    "russian": "rusru",
    "latam": "spaar",
    "spanish": "spaes",
    "thai": "thath",
    "schinese": "zhocn",
    "tchinese": "zhotw",
}


def parse_massedit_files(massedit_dir):
    """Parse all .MASSEDIT files and return dict of {row_id: {field: value, ..., '_category': str}}"""
    entries = defaultdict(dict)

    for filepath in sorted(Path(massedit_dir).glob("*.MASSEDIT")):
        filename = filepath.stem
        category = CATEGORY_MAP.get(filename)
        if category is None:
            print(f"WARNING: Unknown category for file '{filename}', skipping")
            continue

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
            f.write(f"    }}, Category::{category}, {slot}, {suffix}}},\n")

        f.write("};\n\n")
        f.write("} // namespace goblin::generated\n")

    print(f"Generated {output_path} with {len(sorted_ids)} entries")


def escape_wstring(text):
    """Escape a string for C++ wstring literal."""
    result = []
    for ch in text:
        if ch == '"':
            result.append('\\"')
        elif ch == '\\':
            result.append('\\\\')
        elif ch == '\n':
            result.append('\\n')
        elif ch == '\r':
            result.append('\\r')
        elif ch == '\t':
            result.append('\\t')
        elif ord(ch) > 127:
            # Use unicode escape
            code = ord(ch)
            if code <= 0xFFFF:
                result.append(f'\\u{code:04x}')
            else:
                result.append(f'\\U{code:08x}')
        else:
            result.append(ch)
    return ''.join(result)


def parse_fmg_json(filepath):
    """Parse a TitleLocations.fmg.json and return dict of {id: text}."""
    with open(filepath, "r", encoding="utf-8") as f:
        data = json.load(f)

    entries = {}
    for entry in data.get("Fmg", {}).get("Entries", []):
        msg_id = entry.get("ID")
        text = entry.get("Text", "")
        if msg_id is not None and text:
            entries[msg_id] = text
    return entries


def generate_text_data_cpp(msg_dir, output_path):
    """Generate goblin_text_data.cpp with static string literals."""

    lang_data = {}

    for steam_name, folder_name in LANG_FOLDER_MAP.items():
        json_path = Path(msg_dir) / folder_name / "TitleLocations.fmg.json"
        if json_path.exists():
            entries = parse_fmg_json(json_path)
            if entries:
                lang_data[steam_name] = entries
                print(f"  {steam_name}: {len(entries)} text entries")

    if not lang_data:
        print("WARNING: No text data found!")

    with open(output_path, "w", encoding="utf-8") as f:
        f.write("// AUTO-GENERATED FILE - DO NOT EDIT\n")
        f.write("// Generated by tools/generate_data.py from FMG JSON files\n\n")
        f.write('#include "goblin_text_data.hpp"\n\n')
        f.write("namespace goblin::generated\n{\n\n")

        for steam_name, entries in sorted(lang_data.items()):
            sorted_ids = sorted(entries.keys())
            arr_name = f"text_{steam_name}"
            f.write(f"static const TextEntry {arr_name}[] = {{\n")
            for msg_id in sorted_ids:
                text = entries[msg_id]
                escaped = escape_wstring(text)
                f.write(f'    {{{msg_id}, L"{escaped}"}},\n')
            f.write(f"}};\n\n")

        f.write(f"const size_t LANG_COUNT = {len(lang_data)};\n\n")
        f.write("const LangData LANG_TABLE[] = {\n")
        for steam_name, entries in sorted(lang_data.items()):
            arr_name = f"text_{steam_name}"
            count = len(entries)
            f.write(f'    {{"{steam_name}", {arr_name}, {count}}},\n')
        f.write("};\n\n")

        f.write("} // namespace goblin::generated\n")

    total = sum(len(v) for v in lang_data.values())
    print(f"Generated {output_path} with {len(lang_data)} languages, {total} total entries")


def main():
    script_dir = Path(__file__).parent
    project_dir = script_dir.parent

    massedit_dir = project_dir / "data" / "massedit"
    msg_dir = project_dir / "data" / "msg"
    output_dir = project_dir / "src" / "generated"

    output_dir.mkdir(parents=True, exist_ok=True)

    print("=== Parsing MASSEDIT files ===")
    entries = parse_massedit_files(massedit_dir)
    print(f"Total unique entries: {len(entries)}")

    print("\n=== Loading piece metadata ===")
    geom_slots = load_piece_metadata(massedit_dir)
    print(f"Loaded {len(geom_slots)} piece metadata entries")

    print("\n=== Generating map data C++ ===")
    generate_map_data_cpp(entries, output_dir / "goblin_map_data.cpp", geom_slots)

    print("\n=== Generating text data C++ ===")
    generate_text_data_cpp(msg_dir, output_dir / "goblin_text_data.cpp")

    print("\nDone.")


if __name__ == "__main__":
    main()
