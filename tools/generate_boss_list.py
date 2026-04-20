#!/usr/bin/env python3
"""Generate boss_list.json from vanilla WorldMapPointParam + MSB entity matching."""

import json
import sys
import io
import os
import tempfile
from collections import defaultdict

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')

import config
from pythonnet import load
load('coreclr')
import clr
from System.Reflection import Assembly, BindingFlags
from System import Array, Type as SysType, Object
from System.IO import File as SysFile

asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
clr.AddReference(str(config.SOULSFORMATS_DLL))
import SoulsFormats

ERR_MOD_DIR = config.require_err_mod_dir()
bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(ERR_MOD_DIR / 'regulation.bin'))
from extract_all_items import load_paramdefs, read_param, param_to_dict
paramdefs = load_paramdefs()

_str_type = SysType.GetType('System.String')
_bnd4_read = asm.GetType('SoulsFormats.BND4').GetMethod('Read',
    BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str_type]), None)
_fmg_read = asm.GetType('SoulsFormats.FMG').GetMethod('Read',
    BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str_type]), None)
_msbe_read = asm.GetType('SoulsFormats.MSBE').GetMethod('Read',
    BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str_type]), None)

def rfb(rm, data, suf='.bin'):
    tmp = os.path.join(tempfile.gettempdir(), '_mfg_tmp' + suf)
    if hasattr(data, 'ToArray'):
        SysFile.WriteAllBytes(tmp, data.ToArray())
    else:
        SysFile.WriteAllBytes(tmp, data)
    r = rm.Invoke(None, Array[Object]([tmp]))
    os.unlink(tmp)
    return r


def main():
    # Vanilla WorldMapPointParam field bosses (textId2=5100)
    print("Loading WorldMapPointParam...")
    wmp = read_param(bnd, 'WorldMapPointParam', paramdefs)
    fields = {'clearedEventFlagId', 'textId1', 'textId2', 'textId4',
              'areaNo', 'gridXNo', 'gridZNo', 'posX', 'posY', 'posZ',
              'textEnableFlagId4'}
    wmp_data = param_to_dict(wmp, fields)

    # PlaceName FMG
    print("Loading PlaceName FMG...")
    msgbnd = rfb(_bnd4_read, SoulsFormats.DCX.Decompress(
        str(ERR_MOD_DIR / 'msg' / 'engus' / 'item_dlc02.msgbnd.dcx')), '.bnd')
    place_names = {}
    for f in msgbnd.Files:
        if 'PlaceName' in str(f.Name) and '_dlc' not in str(f.Name).lower():
            fmg = rfb(_fmg_read, f.Bytes, '.fmg')
            for e in fmg.Entries:
                t = str(e.Text) if e.Text else ''
                if t:
                    place_names[int(e.ID)] = t
            break

    # All MSB entities
    print("Scanning MSB entities...")
    MSB_DIR = ERR_MOD_DIR / 'map' / 'MapStudio'
    all_entities = {}
    for msb_path in sorted(MSB_DIR.glob('*.msb.dcx')):
        map_name = msb_path.name.replace('.msb.dcx', '')
        try:
            msb = rfb(_msbe_read, SoulsFormats.DCX.Decompress(str(msb_path)), '.msb')
        except:
            continue
        parts = map_name.split('_')
        if len(parts) < 4:
            continue
        area = int(parts[0][1:])
        p1 = int(parts[1])
        p2 = int(parts[2])
        for p in msb.Parts.Enemies:
            eid = int(p.EntityID)
            if eid > 0:
                groups = set()
                if p.EntityGroupIDs:
                    for g in p.EntityGroupIDs:
                        gi = int(g)
                        if gi > 0:
                            groups.add(gi)
                all_entities[eid] = {
                    'model': str(p.ModelName), 'npc': int(p.NPCParamID),
                    'x': round(float(p.Position.X), 3), 'y': round(float(p.Position.Y), 3),
                    'z': round(float(p.Position.Z), 3),
                    'map': map_name, 'area': area, 'gridX': p1, 'gridZ': p2,
                    'groups': groups,
                }

    print(f"  {len(all_entities)} MSB entities")

    # Field bosses, deduplicated by clearedEventFlagId
    field_bosses = []
    seen_flags = set()
    for rid, row in wmp_data.items():
        if row.get('textId2') != 5100 or row.get('clearedEventFlagId', 0) <= 0:
            continue
        f = row['clearedEventFlagId']
        if f in seen_flags:
            continue
        seen_flags.add(f)
        field_bosses.append((rid, row))

    print(f"  {len(field_bosses)} unique field bosses")

    SKIP_MODELS = {'c0000', 'c0100', 'c1000', 'c4190', 'c4191', 'c4192'}
    BOSS_SUFFIXES = [800, 850, 890, 340, 810, 900, 330, 851, 341, 320]


    # Load tutorial names for text matching
    from generate_loot_massedit import ENEMY_NAMES, TUTORIAL_NAMES, resolve_enemy_tutorial_id

    # Index entities by map name for extended search
    entities_by_map = defaultdict(list)
    for eid, e in all_entities.items():
        entities_by_map[e['map']].append((eid, e))

    # Group bosses by tile
    tile_bosses = defaultdict(list)
    for rid, row in field_bosses:
        area = row.get('areaNo', 0)
        gx = row.get('gridXNo', 0)
        gz = row.get('gridZNo', 0)
        tile_bosses[(area, gx, gz)].append((rid, row))

    # For each tile: find all boss entities, assign unique per vanilla entry
    result = []
    matched = 0

    for (area, gx, gz), bosses_in_tile in tile_bosses.items():
        map_name = f'm{area:02d}_{gx:02d}_{gz:02d}_00'

        assignments = {}

        # --- Phase 0: direct lookup via textEnableFlagId4 → MSB entity ---
        # textEnableFlagId4 (when textId4=5120 "Defeated") is the entity ID
        for bi, (rid, row) in enumerate(bosses_in_tile):
            ef4 = row.get('textEnableFlagId4', 0)
            t4 = row.get('textId4', 0)
            if t4 == 5120 and ef4 > 0 and ef4 in all_entities:
                assignments[bi] = (ef4, all_entities[ef4])

        # --- Phase 1: suffix-based, non-c0000 (fallback for 14 bosses not in MSB) ---
        candidate_ids = set()
        for suffix in BOSS_SUFFIXES:
            if area in (60, 61):
                prefix = 10 if area == 60 else 20
                cand = prefix * 100000000 + gx * 1000000 + gz * 10000 + suffix
            else:
                cand = area * 1000000 + gx * 10000 + suffix
            candidate_ids.add(cand)
        for rid, row in bosses_in_tile:
            cflag = row.get('clearedEventFlagId', 0)
            if cflag > 10000000:
                flag_base = (cflag // 10000) * 10000
                for suffix in BOSS_SUFFIXES:
                    candidate_ids.add(flag_base + suffix)

        candidates = []
        seen_ents = set()
        for cid in sorted(candidate_ids):
            if cid not in all_entities:
                continue
            e = all_entities[cid]
            if e['model'] in SKIP_MODELS:
                continue
            key = (e['model'], e['npc'], e['x'], e['z'])
            if key not in seen_ents:
                seen_ents.add(key)
                candidates.append((cid, e))

        # Phase 1a: text match (skip bosses already matched in Phase 0)
        used = set()
        for bi, (rid, row) in enumerate(bosses_in_tile):
            if bi in assignments:
                continue
            boss_name = place_names.get(row.get('textId1', 0), '')
            for i, (cid, e) in enumerate(candidates):
                if i in used:
                    continue
                tut_id = resolve_enemy_tutorial_id(e['model'], e['npc'], boss_name)
                tut_name = TUTORIAL_NAMES.get(tut_id, '')
                if tut_name.lower() == boss_name.lower():
                    used.add(i)
                    assignments[bi] = (cid, e)
                    break

        # Phase 1b: first unused non-c0000
        for bi, (rid, row) in enumerate(bosses_in_tile):
            if bi in assignments:
                continue
            for i, (cid, e) in enumerate(candidates):
                if i not in used:
                    used.add(i)
                    assignments[bi] = (cid, e)
                    break

        # --- Phase 2: text-match against ALL non-c0000 entities in the MSB ---
        # (catches bosses with non-standard suffixes like 375, 400, 120)
        unmatched_bis = [bi for bi in range(len(bosses_in_tile)) if bi not in assignments]
        if unmatched_bis:
            all_non_c0000 = []
            seen2 = set()
            for eid, e in sorted(entities_by_map.get(map_name, [])):
                if e['model'] in SKIP_MODELS:
                    continue
                key = (e['model'], e['npc'], e['x'], e['z'])
                if key in seen2:
                    continue
                seen2.add(key)
                all_non_c0000.append((eid, e))

            used2 = set()
            for bi in unmatched_bis[:]:
                boss_name = place_names.get(bosses_in_tile[bi][1].get('textId1', 0), '')
                for i, (cid, e) in enumerate(all_non_c0000):
                    if i in used2:
                        continue
                    tut_id = resolve_enemy_tutorial_id(e['model'], e['npc'], boss_name)
                    tut_name = TUTORIAL_NAMES.get(tut_id, '')
                    if tut_name.lower() == boss_name.lower():
                        used2.add(i)
                        assignments[bi] = (cid, e)
                        unmatched_bis.remove(bi)
                        break

        # --- Phase 3: c0000 entities for remaining unmatched bosses ---
        if unmatched_bis:
            # Boss entity group: tile_base + 5800
            if area in (60, 61):
                prefix = 10 if area == 60 else 20
                tile_base = prefix * 100000000 + gx * 1000000 + gz * 10000
            else:
                tile_base = area * 1000000 + gx * 10000
            boss_group = tile_base + 5800

            c0000_raw = []
            c0000_seen = set()
            for eid, e in sorted(entities_by_map.get(map_name, [])):
                if e['model'] != 'c0000':
                    continue
                suf = eid % 10000
                if suf < 700 or suf > 860:
                    continue
                key = (e['npc'], e['x'], e['z'])
                if key in c0000_seen:
                    continue
                c0000_seen.add(key)
                c0000_raw.append((eid, e))
            # Sort: boss group first, then prefer suf 800+, then descending suffix
            def c0000_sort_key(item):
                eid, e = item
                suf = eid % 10000
                has_group = 0 if boss_group in e.get('groups', set()) else 1
                is_800 = 0 if 800 <= suf <= 860 else 1
                return (has_group, is_800, -suf)
            c0000_candidates = sorted(c0000_raw, key=c0000_sort_key)

            c0000_used = set()
            for bi in unmatched_bis:
                for i, (cid, e) in enumerate(c0000_candidates):
                    if i not in c0000_used:
                        c0000_used.add(i)
                        assignments[bi] = (cid, e)
                        break

        # Build results
        for bi, (rid, row) in enumerate(bosses_in_tile):
            cflag = row.get('clearedEventFlagId', 0)
            # textEnableFlagId4 = actual "defeated" flag, only when textId4=5120
            kill_flag = row.get('textEnableFlagId4', 0) if row.get('textId4') == 5120 else 0
            boss_name = place_names.get(row.get('textId1', 0), '')
            wmp_text_id = row.get('textId1', 0)  # PlaceName ID from ERR WMP
            if bi in assignments:
                cid, e = assignments[bi]
                result.append({
                    'areaNo': e['area'], 'gridX': e['gridX'],
                    'gridZ': e['gridZ'],
                    'x': e['x'], 'y': e['y'], 'z': e['z'],
                    'map': e['map'], 'enemyModel': e['model'],
                    'npcParamId': e['npc'],
                    'clearedEventFlagId': cflag,
                    'killEventFlagId': kill_flag,
                    'wmpTextId1': wmp_text_id,
                    'vanillaPlaceName': boss_name,
                })
                matched += 1
            else:
                # Vanilla coords fallback
                result.append({
                    'areaNo': area, 'gridX': gx, 'gridZ': gz,
                    'x': round(float(row.get('posX', 0)), 3),
                    'y': round(float(row.get('posY', 0)), 3),
                    'z': round(float(row.get('posZ', 0)), 3),
                    'map': f'm{area}_{gx:02d}_{gz:02d}_00', 'enemyModel': '', 'npcParamId': 0,
                    'clearedEventFlagId': cflag,
                    'killEventFlagId': kill_flag,
                    'wmpTextId1': wmp_text_id,
                    'vanillaPlaceName': boss_name,
                })

    out_path = config.DATA_DIR / 'boss_list.json'
    with open(out_path, 'w', encoding='utf-8') as f:
        json.dump(result, f, ensure_ascii=False, indent=2)

    unmatched = [b for b in result if b.get('npcParamId', 0) == 0]
    print(f"Total: {len(result)}, matched: {matched}, unmatched: {len(unmatched)}")
    for b in unmatched:
        print(f'  "{b["vanillaPlaceName"]}" area={b["areaNo"]} grid=({b["gridX"]},{b["gridZ"]})')


if __name__ == '__main__':
    main()
