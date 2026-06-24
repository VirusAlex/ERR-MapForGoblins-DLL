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

from massedit_common import (DATA_DIR, OUT_DIR as OUTPUT_DIR, OVERWORLD_AREAS,
                             resolve_location_id, resolve_location_id_at,
                             DLC_AREAS, UNDERGROUND_AREAS, get_disp_mask)

CSV_PATH = DATA_DIR / "ItemLotParam_map.csv"

# Alias for backward compatibility - tile-only resolver (legacy callers)
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

        # Per-item auto-hide flag (from items_database boss-flag records); kept
        # backwards-compatible with AEG099_821/822 markers which don't have one.
        item_flag = item.get('event_flag', 0)
        if item_flag > 0:
            lines.append(f"param WorldMapPointParam: id {row_id}: textDisableFlagId1: = {item_flag};")
            flag_idx += 1

        next_text_slot = 2
        # Per-marker nearest-grace lookup - disambiguates stacked dungeon regions
        # (e.g. Nokron vs Siofra River in m12_02/m12_07).
        loc_id = resolve_location_id_at(item['map'], item['x'], item.get('y', 0.0), item['z'])
        if loc_id > 0:
            lines.append(f"param WorldMapPointParam: id {row_id}: textId{next_text_slot}: = {loc_id};")
            if item_flag > 0:
                lines.append(f"param WorldMapPointParam: id {row_id}: textDisableFlagId{next_text_slot}: = {item_flag};")
            next_text_slot += 1

        # Enemy name for boss-flag pieces. Two paths:
        #   1. TutorialTitle (+900M offset) - for model-based enemies.
        #   2. NpcName (+700M offset) - for named NPCs (DLL slot 18, "Characters").
        enemy_model = item.get('enemy_model', '')
        npc_name_id = item.get('npc_name_id', 0)
        enemy_text_id = 0
        if enemy_model:
            tutorial_id = resolve_enemy_tutorial_id(enemy_model, item.get('npc_param', 0))
            if tutorial_id > 0:
                enemy_text_id = tutorial_id + 900000000
        elif npc_name_id > 0:
            enemy_text_id = npc_name_id + 700000000
        if enemy_text_id > 0:
            lines.append(f"param WorldMapPointParam: id {row_id}: textId{next_text_slot}: = {enemy_text_id};")
            if item_flag > 0:
                lines.append(f"param WorldMapPointParam: id {row_id}: textDisableFlagId{next_text_slot}: = {item_flag};")
            next_text_slot += 1

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
        # Strip the SoulsFormats duplicate-name decoration: ERR copy-pastes parts keeping
        # the Name, the reader disambiguates as "AEG099_821_9000 {2}". In-game the part is
        # plain "AEG099_821_9000" - bake THAT so collected-tracking can match it (decorated
        # names never match the live WGM name → the marker would never hide).
        name = item.get('name', '').split(' {')[0]
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


# --- Enemy-name resolution (shared with generate_loot_massedit) ---
def _load_enemy_tutorial_mapping():
    p = DATA_DIR / 'enemy_tutorial_mapping.json'
    return json.load(open(p)) if p.exists() else {}

def _load_tutorial_ids():
    p = DATA_DIR / 'tutorial_title_ids.json'
    return set(json.load(open(p))) if p.exists() else set()

ENEMY_NAMES = _load_enemy_tutorial_mapping()
TUTORIAL_IDS = _load_tutorial_ids()

def resolve_enemy_tutorial_id(enemy_model, npc_param_id):
    """Resolve TutorialTitle FMG id for an enemy. Mirrors
    generate_loot_massedit.resolve_enemy_tutorial_id (variant by NpcParam digit)."""
    base_id = ENEMY_NAMES.get(enemy_model, 0)
    if base_id <= 0:
        return 0
    if npc_param_id <= 0:
        return base_id
    variant = (npc_param_id // 1000) % 10
    if variant == 0:
        return base_id
    variant_id = base_id + variant * 100
    return variant_id if variant_id in TUTORIAL_IDS else base_id


# Manual override for c0000-model bosses (or other models not in
# enemy_tutorial_mapping). Maps (map, partName) -> NpcName FMG entry id -
# DLL resolves these via +700000000 offset (slot 18/328/428 NpcName.fmg).
MANUAL_BOSS_NAMEID = {
    ('m30_20_00_00', 'c0000_9003'): 903320300,  # "Stray Mimic Tear" (NpcName "Characters")
    # NpcParam nameId paths handle the rest (Necromancer Garris: nameId=137600).
}

# NpcName FMG ids whose owner we don't want as a piece marker (friendly NPCs
# the player shouldn't kill: traders, questgivers, etc.). Resolved via
# NpcParam.nameId -> NpcName text -> blacklist check.
SKIP_NPC_NAMES = {
    'Patches',          # Murkwater Cave invader/merchant
    # add more known questgivers/traders as identified
}


def _load_npc_param_name_ids():
    """Build npcParamId -> nameId mapping from regulation.bin via extract_all_items
    machinery. Cache as a JSON sidecar to avoid re-reading regulation each run."""
    cache_path = DATA_DIR / 'npc_name_id_map.json'
    if cache_path.exists():
        return {int(k): int(v) for k, v in json.load(open(cache_path)).items()}
    # Lazy build: import and reuse extract_all_items helpers
    try:
        import config
        from pythonnet import load as _pyload
        _pyload('coreclr')
        import clr
        from System.Reflection import Assembly
        from System import Array, Type as SysType, Object
        from System.IO import File as SysFile
        import os as _os, tempfile as _tempfile
        asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
        clr.AddReference(str(config.SOULSFORMATS_DLL))
        import SoulsFormats
        _str = SysType.GetType('System.String')
        _pcls = asm.GetType('SoulsFormats.PARAM')
        _pr = _pcls.BaseType.GetMethod('Read', Array[SysType]([_str]))
        defs = {}
        for xml in config.PARAMDEF_DIR.glob('*.xml'):
            try:
                d = SoulsFormats.PARAMDEF.XmlDeserialize(str(xml), False)
                if d and d.ParamType:
                    defs[str(d.ParamType)] = d
            except Exception:
                pass
        bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(config.ERR_MOD_DIR / 'regulation.bin'))
        for f in bnd.Files:
            if 'NpcParam.param' not in str(f.Name):
                continue
            tmp = _os.path.join(_tempfile.gettempdir(), '_np.param')
            SysFile.WriteAllBytes(tmp, f.Bytes.ToArray())
            p = _pr.Invoke(None, Array[Object]([tmp]))
            pdef = defs.get(str(p.ParamType))
            if pdef:
                p.ApplyParamdef(pdef)
            out = {}
            for r in p.Rows:
                rid = int(r.ID); nid = 0
                for c in r.Cells:
                    if str(c.Def.InternalName) == 'nameId':
                        try: nid = int(c.Value)
                        except Exception: pass
                        break
                if nid > 0:
                    out[rid] = nid
            with open(cache_path, 'w', encoding='utf-8') as fp:
                json.dump({str(k): v for k, v in out.items()}, fp)
            return out
    except Exception as e:
        print(f"  WARN: NpcParam load failed: {e}")
    return {}


def _load_npc_name_fmg():
    """nameId -> text from NpcName.fmg (mod's item_dlc02.msgbnd)."""
    cache_path = DATA_DIR / 'npc_name_text_map.json'
    if cache_path.exists():
        return {int(k): v for k, v in json.load(open(cache_path, encoding='utf-8')).items()}
    try:
        import config
        from pythonnet import load as _pyload
        _pyload('coreclr')
        from System.Reflection import Assembly
        from System import Array, Type as SysType, Object
        from System.IO import File as SysFile
        import os as _os, tempfile as _tempfile
        asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
        import SoulsFormats
        _str = SysType.GetType('System.String')
        _fcls = asm.GetType('SoulsFormats.FMG')
        _fr = _fcls.BaseType.GetMethod('Read', Array[SysType]([_str]))
        _bcls = asm.GetType('SoulsFormats.BND4')
        _br = _bcls.BaseType.GetMethod('Read', Array[SysType]([_str]))
        out = {}
        for mp in [config.ERR_MOD_DIR / 'msg/engus/item_dlc02.msgbnd.dcx']:
            bnd = _br.Invoke(None, Array[Object]([str(mp)]))
            for f in bnd.Files:
                if 'NpcName' not in str(f.Name): continue
                tmp = _os.path.join(_tempfile.gettempdir(), '_n.fmg')
                SysFile.WriteAllBytes(tmp, f.Bytes.ToArray())
                fmg = _fr.Invoke(None, Array[Object]([tmp]))
                for e in fmg.Entries:
                    t = str(e.Text) if e.Text else ''
                    if t and t != '[ERROR]':
                        out.setdefault(int(e.ID), t)
        with open(cache_path, 'w', encoding='utf-8') as fp:
            json.dump({str(k): v for k, v in out.items()}, fp, ensure_ascii=False)
        return out
    except Exception as e:
        print(f"  WARN: NpcName load failed: {e}")
    return {}


NPC_PARAM_NAMEID = _load_npc_param_name_ids()
NPC_NAME_TEXT = _load_npc_name_fmg()


def resolve_npc_name_id(enemy_model, npc_param, map_name, part_name):
    """Resolve NpcName FMG entry id for a non-TutorialTitle-resolvable enemy.
    Returns (name_id, skip_reason). If skip_reason is set, drop the record."""
    if (map_name, part_name) in MANUAL_BOSS_NAMEID:
        return MANUAL_BOSS_NAMEID[(map_name, part_name)], None
    name_id = NPC_PARAM_NAMEID.get(int(npc_param or 0), 0)
    if name_id > 0:
        text = NPC_NAME_TEXT.get(name_id, '')
        if text in SKIP_NPC_NAMES:
            return 0, f'blacklist:{text}'
        return name_id, None
    return 0, 'unresolved'


def load_boss_flag_pieces(goods_id):
    """Pull boss-flag-driven pieces from items_database.json - these are
    Rune/Ember Pieces awarded via common.emevd event 1200 on boss kill.

    Filter: keep only records whose enemy can be named via TutorialTitle FMG
    (`enemy_tutorial_mapping.json`) OR via NpcName FMG (`NpcParam.nameId` /
    MANUAL_BOSS_NAMEID override), excluding any name in SKIP_NPC_NAMES.
    """
    db_path = DATA_DIR / "items_database.json"
    if not db_path.exists():
        return []
    with open(db_path, encoding='utf-8') as f:
        db = json.load(f)
    out = []
    drop_stats = {'no_model_mapping_no_npc_name': 0, 'blacklisted_npc': 0}
    kept_via_npc_name = 0
    for r in db:
        if r.get('source') != 'emevd' or not r.get('defeatFlag'):
            continue
        items = r.get('items', [])
        if not items or items[0].get('id') != goods_id:
            continue
        flag = r.get('eventFlag', 0)
        if flag <= 0:
            continue
        em = r.get('enemyModel', '')
        npc_param = r.get('npcParamId', 0)
        map_name = r['map']
        part_name = r.get('partName', '')

        tutorial_id = 0
        if em in ENEMY_NAMES:
            tutorial_id = resolve_enemy_tutorial_id(em, npc_param)

        npc_name_id = 0
        if tutorial_id <= 0:
            npc_name_id, skip_reason = resolve_npc_name_id(em, npc_param, map_name, part_name)
            if skip_reason:
                if skip_reason.startswith('blacklist'):
                    drop_stats['blacklisted_npc'] += 1
                else:
                    drop_stats['no_model_mapping_no_npc_name'] += 1
                continue
            if npc_name_id > 0:
                kept_via_npc_name += 1

        out.append({
            'map': map_name, 'x': r['x'], 'y': r['y'], 'z': r['z'],
            'instance_id': -1,
            'name': part_name,
            'event_flag': flag,
            'enemy_model': em if tutorial_id > 0 else '',
            'npc_param': npc_param if tutorial_id > 0 else 0,
            'npc_name_id': npc_name_id,
        })
    print(f"  Dropped (no name found, not in manual override): {drop_stats['no_model_mapping_no_npc_name']}")
    print(f"  Dropped (NpcName in blacklist, e.g. Patches):     {drop_stats['blacklisted_npc']}")
    print(f"  Kept via NpcName resolution:                       {kept_via_npc_name}")
    return out


def main():
    rune_items = json.load(open(DATA_DIR / "rune_pieces.json"))
    ember_items = json.load(open(DATA_DIR / "ember_pieces.json"))
    print(f"Loaded AEG099 pieces: {len(rune_items)} Rune, {len(ember_items)} Ember")

    rune_boss = load_boss_flag_pieces(800010)
    ember_boss = load_boss_flag_pieces(850010)
    print(f"Loaded boss-flag pieces (event 1200): "
          f"{len(rune_boss)} Rune, {len(ember_boss)} Ember")

    rune_items += rune_boss
    ember_items += ember_boss

    rune_flags = load_event_flags(CSV_PATH, 800010)
    ember_flags = load_event_flags(CSV_PATH, 850010)
    print(f"CSV-derived event flags: {len(rune_flags)} for Rune, {len(ember_flags)} for Ember")

    print("\nGenerating MASSEDIT...")

    generate_massedit(
        rune_items, "Rune Pieces",
        text_id=800010, icon_id=__import__("icon_registry").iconid("rune_pieces"),   # "Rune Piece" - localized via GoodsName FMG
        start_row_id=__import__("row_id_registry").base("Reforged - Rune Pieces"),
        output_file=OUTPUT_DIR / "Reforged - Rune Pieces.MASSEDIT",
        event_flags=rune_flags
    )

    generate_massedit(
        ember_items, "Ember Pieces",
        text_id=850010, icon_id=__import__("icon_registry").iconid("ember_pieces"),   # "Ember Piece" - same star as Rune Pieces
        start_row_id=__import__("row_id_registry").base("Reforged - Ember Pieces"),
        output_file=OUTPUT_DIR / "Reforged - Ember Pieces.MASSEDIT",
        event_flags=ember_flags
    )

    print("\nRun 'py tools/generate_data.py' and rebuild DLL.")


if __name__ == "__main__":
    main()
