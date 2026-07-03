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
    "Loot - Merchant Bell-Bearings": "LootMerchantBellBearings",
    "Loot - MP-Fingers": "LootMPFingers",
    "Loot - Prattling Pates": "LootPrattlingPates",
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
    "World - Kindling Spirits": "WorldKindlingSpirits",
    "World - Seal Puzzles": "WorldInteractables",
    "World - Hero's Tomb Statues": "WorldInteractables",
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
    # Group-2 gate flags addressed by their real paramdef names (what
    # generate_loot_massedit emits for switched chests). Group1 is the DLL's
    # category/live-loot lane; group2 is baked straight through (see memcpy in
    # inject_map_entries) so a marker's icon can be gated on a world-state flag.
    # textDisableFlag2Id1 had NO mapping before, so it was silently dropped.
    "textEnableFlag2Id1": ("textEnableFlag2Id1", "i32"),
    "textDisableFlag2Id1": ("textDisableFlag2Id1", "i32"),
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


# ERR-only MASSEDIT categories: never bake these in the vanilla profile, even
# if a stale file is present in the (gitignored) vanilla output dir.
ERR_ONLY_FILES = {
    "Reforged - Rune Pieces", "Reforged - Ember Pieces",
    "Reforged - Items", "Reforged - Fortunes", "Reforged - Sealed Curios",
    "World - Kindling Spirits",
}


def parse_massedit_files(massedit_dir):
    """Parse all .MASSEDIT files and return dict of {row_id: {field: value, ..., '_category': str}}"""
    import config
    entries = defaultdict(dict)

    for filepath in sorted(Path(massedit_dir).glob("*.MASSEDIT")):
        filename = filepath.stem
        if config.PROFILE != 'err' and filename in ERR_ONLY_FILES:
            print(f"SKIP (ERR-only): {filename}")
            continue
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
    # e.g. m11_10 (Roundtable Hold) - MSB coords land in ocean
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


def _load_lot_linkage():
    """row_id(int) -> (lotId, lotType) from generate_loot_massedit's side file."""
    import config
    p = config.DATA_DIR / 'loot_lot_linkage.json'
    if not p.exists():
        return {}
    with open(p, encoding='utf-8') as f:
        raw = json.load(f)
    return {int(k): (int(v[0]), int(v[1])) for k, v in raw.items()}


def generate_map_data_cpp(entries, output_path, geom_slots=None, orig_xz=None):
    """Generate goblin_map_data.cpp with all param entries."""
    if geom_slots is None:
        geom_slots = {}
    if orig_xz is None:
        orig_xz = {}

    lot_linkage = _load_lot_linkage()

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
            lot_id, lot_type = lot_linkage.get(row_id, (0, 0))
            name_field = f'"{obj_name}"' if obj_name else 'nullptr'
            # real_posX/real_posZ = pre-de-overlap MSB-true coords (fall back to the
            # possibly-shifted display pos if not snapshotted) for collected tracking.
            rx, rz = orig_xz.get(row_id, (fields.get("posX", "0"), fields.get("posZ", "0")))
            rx = format_value("real_posX", "f", str(rx))
            rz = format_value("real_posZ", "f", str(rz))
            f.write(f"    }}, Category::{category}, {slot}, {suffix}, {name_field}, "
                    f"{lot_id}u, {lot_type}, {rx}, {rz}}},\n")

        f.write("};\n\n")
        f.write("} // namespace goblin::generated\n")

    print(f"Generated {output_path} with {len(sorted_ids)} entries")


def generate_item_icons_cpp(output_path):
    """Generate goblin_item_icons.cpp: encoded-item-key -> (iconId, Category).

    Source = item_icon_table.json from generate_loot_massedit (the same ordered
    LOOT_CATEGORIES classifier, applied per item). Lets the DLL re-icon and
    re-gate a lot-backed marker from the LIVE randomized item. Sorted by key
    for binary search."""
    import config
    p = config.DATA_DIR / 'item_icon_table.json'
    table = {}  # key(int) -> (iconId, Category enum name)
    if p.exists():
        with open(p, encoding='utf-8') as f:
            raw = json.load(f)
        for k, v in raw.items():
            enum = CATEGORY_MAP.get(v[1])
            if enum:
                table[int(k)] = (int(v[0]), enum)

    # Spoiler-free "?" map-icon iconId. Just another registry-assigned icon id; the
    # DLL injects its frame and remaps markers to it at runtime like every other icon.
    anon_icon_id = __import__("icon_registry").iconid("anon")

    with open(output_path, "w", encoding="utf-8") as f:
        f.write("// AUTO-GENERATED FILE - DO NOT EDIT\n")
        f.write("// Generated by tools/generate_data.py from item_icon_table.json\n\n")
        f.write('#include "goblin_item_icons.hpp"\n\n')
        f.write("namespace goblin::generated\n{\n\n")
        f.write(f"const uint16_t ANON_ICON_ID = {anon_icon_id}u;\n\n")
        f.write(f"const size_t ITEM_ICON_COUNT = {len(table)};\n\n")
        f.write("const ItemIcon ITEM_ICONS[] = {\n")
        for key in sorted(table.keys()):
            icon, enum = table[key]
            f.write(f"    {{{key}, {icon}u, Category::{enum}}},\n")
        f.write("};\n\n")
        f.write("} // namespace goblin::generated\n")

    print(f"Generated {output_path} with {len(table)} item-icon entries")


def generate_item_fallback_cpp(output_path, entries=None):
    """Generate goblin_item_fallback.cpp: marker-textId -> English string. Injected by
    setup_messages as the lowest-priority PlaceName layer so a marker whose text has no
    string in the player's LANGUAGE falls back to English instead of resolving to nothing
    (the engine draws no icon for a fully text-less marker). Overhauls (e.g. The Convergence)
    localize only part of their custom content, so a non-English player otherwise gets blank
    boss names / locations / item names. Covers three id spaces, all keyed the way the marker
    references them (so remap_textid lines them up):
      - items:     encoded-item-key -> English name   (item_icon_table.json, 3rd field)
      - bosses:    npcNameId+700M    -> English name   (boss_list.json vanillaPlaceName)
      - locations: raw PlaceName id  -> English name   (PlaceName_engus.json), only ids a
                   marker actually references (collected from `entries`)."""
    import config
    p = config.DATA_DIR / 'item_icon_table.json'
    table = {}  # key(int) -> English name
    if p.exists():
        with open(p, encoding='utf-8') as f:
            raw = json.load(f)
        for k, v in raw.items():
            name = (v[2] if len(v) > 2 else '').strip()
            if name:
                table[int(k)] = name

    # English fallback for EVERY other band a marker references (NpcName boss/NPC
    # names +700M, ActionButtonText prompts +800M, raw PlaceName locations), from
    # english_fallback.json (extract_english_fallback.py dumps it from the engus
    # msgbnd, keyed by the same offset-encoded id the markers use). Only ids a marker
    # actually references are emitted. setdefault => a real localized string (copied
    # from the player's language at runtime) still wins; this only fills the gaps an
    # overhaul left untranslated in that language. Items are already covered above.
    if entries:
        ef = config.DATA_DIR / 'english_fallback.json'
        english = {}
        if ef.exists():
            with open(ef, encoding='utf-8') as f:
                english = json.load(f)
        else:
            print("  WARNING: english_fallback.json not found - run extract_english_fallback.py")
        referenced = set()
        for fields in entries.values():
            for fld, val in fields.items():
                if not fld.startswith('textId'):
                    continue
                try:
                    tid = int(float(val))
                except (TypeError, ValueError):
                    continue
                if tid > 0:
                    referenced.add(tid)
        for tid in referenced:
            nm = english.get(str(tid))
            if nm:
                table.setdefault(tid, nm.strip())

    with open(output_path, "w", encoding="utf-8") as f:
        f.write("// AUTO-GENERATED FILE - DO NOT EDIT\n")
        f.write("// Generated by tools/generate_data.py from item_icon_table.json\n\n")
        f.write('#include "goblin_item_fallback.hpp"\n\n')
        f.write("namespace goblin::generated\n{\n\n")
        f.write(f"const size_t ITEM_NAME_FALLBACK_COUNT = {len(table)};\n\n")
        f.write("const ItemNameFallback ITEM_NAME_FALLBACK[] = {\n")
        for key in sorted(table.keys()):
            f.write(f"    {{{key}, {_wlit(table[key])}}},\n")
        if not table:
            f.write("    {0, nullptr},\n")
        f.write("};\n\n")
        f.write("} // namespace goblin::generated\n")
    print(f"Generated {output_path} with {len(table)} item-name fallback entries")


# Fixed language order for the embedded enemy-name table (msgbnd codes). engus
# first so it can serve as the fallback. Must match ENEMY_NAME_LANGS in the DLL.
ENEMY_NAME_LANGS = ["engus", "jpnjp", "deude", "frafr", "itait", "korkr", "polpl",
                    "porbr", "rusru", "spaes", "spaar", "thath", "zhocn", "zhotw", "araae"]


def _wlit(s):
    """C++ wide-string literal with every non-ASCII unit as \\uXXXX (encoding-safe)."""
    units = s.encode("utf-16-le")
    out = []
    for i in range(0, len(units), 2):
        cu = units[i] | (units[i + 1] << 8)
        if 0x20 <= cu < 0x7f and chr(cu) not in '"\\':
            out.append(chr(cu))
        else:
            out.append("\\u%04x" % cu)
    return 'L"' + "".join(out) + '"'


def generate_enemy_names_cpp(output_path):
    """Generate goblin_enemy_names.cpp: localized enemy names for the non-ERR
    builds (marker textId = id + 900000000). Empty for the ERR build (it uses
    its own runtime name table). Strings are FromSoft / community-wiki enemy
    names; no reference to where the source data was read from."""
    import config
    is_err = (config.PROFILE == "err")
    table = {}
    if not is_err:
        p = config.PROJECT_DIR / "data" / "enemy_names_i18n.json"
        if p.exists():
            with open(p, encoding="utf-8") as f:
                table = json.load(f)

    with open(output_path, "w", encoding="utf-8") as f:
        f.write("// AUTO-GENERATED FILE - DO NOT EDIT\n")
        f.write("// Generated by tools/generate_data.py from enemy_names_i18n.json\n\n")
        f.write('#include "goblin_enemy_names.hpp"\n\n')
        f.write("namespace goblin::generated\n{\n\n")
        f.write("const char *const ENEMY_NAME_LANGS[ENEMY_NAME_LANG_COUNT] = {\n    ")
        f.write(", ".join(f'"{c}"' for c in ENEMY_NAME_LANGS))
        f.write("\n};\n\n")
        f.write(f"const size_t ENEMY_NAME_COUNT = {len(table)};\n\n")
        f.write("const EnemyName ENEMY_NAMES[] = {\n")
        for k in sorted(table, key=lambda x: int(x)):
            langs = table[k]
            en = langs.get("engus", "")
            cells = [_wlit(langs.get(code, en)) for code in ENEMY_NAME_LANGS]
            f.write(f"    {{{int(k)}, {{{', '.join(cells)}}}}},\n")
        if not table:
            f.write("    {0, {" + ", ".join(["nullptr"] * len(ENEMY_NAME_LANGS)) + "}},\n")
        f.write("};\n\n")
        f.write("} // namespace goblin::generated\n")
    print(f"Generated {output_path} with {len(table)} enemy-name entries"
          + (" (ERR: empty, uses runtime table)" if is_err else ""))


def main():
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--massedit-dir", type=str, default=None,
                        help="Path to MASSEDIT directory (default: data/massedit_generated; "
                             "the pipeline passes the active profile's massedit_generated)")
    args = parser.parse_args()

    script_dir = Path(__file__).parent
    project_dir = script_dir.parent

    if args.massedit_dir:
        massedit_dir = Path(args.massedit_dir)
    else:
        massedit_dir = project_dir / "data" / "massedit_generated"
    import config
    output_dir = config.GENERATED_DIR  # src/generated or src/generated_vanilla

    output_dir.mkdir(parents=True, exist_ok=True)

    # goblin_map_data.hpp (the Category enum + struct) is a static, profile-
    # independent header with no generator. The canonical copy lives in
    # src/generated/; mirror it into a non-default bake dir (e.g. vanilla) so
    # that dir is self-contained for the compiler's include path.
    import shutil
    for hdr in ("goblin_map_data.hpp", "goblin_item_icons.hpp", "goblin_enemy_names.hpp",
                "goblin_item_fallback.hpp"):
        canonical_hpp = project_dir / "src" / "generated" / hdr
        dest_hpp = output_dir / hdr
        if canonical_hpp.resolve() != dest_hpp.resolve():
            shutil.copyfile(canonical_hpp, dest_hpp)

    print("=== Parsing MASSEDIT files ===")
    entries = parse_massedit_files(massedit_dir)
    print(f"Total unique entries: {len(entries)}")

    # Snapshot REAL (MSB-true) X/Z BEFORE the de-overlap pass shifts them. Collected-
    # geometry tracking (goblin_collected) matches a marker to its live CSWorldGeomMan
    # instance by position; the spiral de-overlap below can move a tracked node up to
    # ~15u from its real coords, which broke immediate WGM hiding (it then only hid via
    # GEOF on tile reload). Baked as real_posX/real_posZ so tracking uses the true pos.
    orig_xz = {rid: (f.get("posX", "0"), f.get("posZ", "0")) for rid, f in entries.items()}

    # De-overlap: pull markers that sit within CLUSTER_RADIUS of each other (visually stacked on
    # the map - even across categories, and even a few units apart) onto a square spiral
    # SPIRAL_STEP apart around the cluster anchor. The old logic only grouped EXACT (0.1-rounded)
    # coincident coords, so two markers a couple units apart - or the same pickup emitted at
    # slightly different coords by two different generators - still rendered on top of each other.
    # CLUSTER_RADIUS/SPIRAL_STEP are world units; bump if icons still overlap after a map rebuild.
    print("\n=== De-overlapping icons ===")
    SPIRAL_STEP = 8.0      # spacing between spiraled markers (proven to read as distinct icons)
    CLUSTER_RADIUS = 8.0   # markers within this of a cluster anchor are treated as overlapping
    # Bucket by tile first (markers can only collide within the same area + overworld grid cell),
    # then greedily cluster by proximity inside each tile. Sorted iteration keeps anchors stable
    # (deterministic output across rebuilds).
    _tile_groups = {}  # (area, gx, gz) -> [(row_id, px, pz)]
    for row_id in sorted(entries.keys()):
        fields = entries[row_id]
        area = fields.get("areaNo", "0")
        gx = fields.get("gridXNo", "0")
        gz = fields.get("gridZNo", "0")
        px = float(fields.get("posX", "0"))
        pz = float(fields.get("posZ", "0"))
        _tile_groups.setdefault((area, gx, gz), []).append((row_id, px, pz))
    coord_groups = []  # list of clusters; each = (anchor_px, anchor_pz, [row_ids...])
    _r2 = CLUSTER_RADIUS * CLUSTER_RADIUS
    for _items in _tile_groups.values():
        _clusters = []  # each: [anchor_px, anchor_pz, [row_ids]]
        for row_id, px, pz in _items:
            _hit = None
            for _cl in _clusters:
                if (px - _cl[0]) ** 2 + (pz - _cl[1]) ** 2 <= _r2:
                    _hit = _cl
                    break
            if _hit is None:
                _clusters.append([px, pz, [row_id]])
            else:
                _hit[2].append(row_id)
        coord_groups.extend((c[0], c[1], c[2]) for c in _clusters)

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
    groups = 0
    for ax, az, row_ids in coord_groups:
        if len(row_ids) <= 1:
            continue
        groups += 1
        offsets = spiral_offsets(len(row_ids))
        for i, row_id in enumerate(row_ids):
            if i == 0:
                continue  # anchor stays in place; others spiral around it
            ox, oz = offsets[i]
            entries[row_id]["posX"] = f"{ax + ox * SPIRAL_STEP:.3f}"
            entries[row_id]["posZ"] = f"{az + oz * SPIRAL_STEP:.3f}"
            shifted += 1

    print(f"  Shifted {shifted} entries across {groups} overlap clusters (radius {CLUSTER_RADIUS}u)")

    print("\n=== Loading piece metadata ===")
    geom_slots = load_piece_metadata(massedit_dir)
    print(f"Loaded {len(geom_slots)} piece metadata entries")

    print("\n=== Generating map data C++ ===")
    generate_map_data_cpp(entries, output_dir / "goblin_map_data.cpp", geom_slots, orig_xz)

    print("\n=== Generating item-icon table C++ ===")
    generate_item_icons_cpp(output_dir / "goblin_item_icons.cpp")

    print("\n=== Generating enemy-name table C++ ===")
    generate_enemy_names_cpp(output_dir / "goblin_enemy_names.cpp")
    generate_item_fallback_cpp(output_dir / "goblin_item_fallback.cpp", entries)

    print("\n=== Generating legacy-conv C++ ===")
    generate_legacy_conv_cpp(config.DATA_DIR / "WorldMapLegacyConvParam.json",
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
        f.write("#pragma once\n// AUTO-GENERATED - do not edit.\n")
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
