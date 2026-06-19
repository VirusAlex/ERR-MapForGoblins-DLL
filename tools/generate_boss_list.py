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
    tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_mfg_tmp' + suf)
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
            # Skip parts disabled in this build (preview/test placements like
            # cView_*, vanilla entities replaced by ERR). Engine never spawns
            # them, so any boss marker would be phantom.
            if int(getattr(p, 'GameEditionDisable', 0) or 0) == 1:
                continue
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
                    'groups': groups, 'partName': str(p.Name),
                }

    print(f"  {len(all_entities)} MSB entities")

    # Cross-tile placeholder remap: when an entity lives in a supertile MSB
    # (m60_XX_YY_01/_02/_12) but its partName carries a fine-tile prefix like
    # "m60_52_38_00-c4730_9002", the position is stored in supertile-local
    # coords. Convert it to fine-tile-local coords so the marker lands on
    # the right map tile. Mirrors extract_all_items.py's remap.
    import re as _re
    _SUFFIX_SCALE = {'01': 2, '02': 4, '12': 4}
    _FINE = 256
    _prefix_re = _re.compile(r'^m(\d{2})_(\d{2})_(\d{2})_(\d{2})-')
    remapped = 0
    for eid, ent in all_entities.items():
        m = _prefix_re.match(ent.get('partName', ''))
        if not m: continue
        own_area, own_gx, own_gz, own_p3 = (int(g) for g in m.groups())
        cur_map = ent['map']
        # Skip if partName already matches the MSB (regular asset)
        if cur_map.startswith(f'm{own_area:02d}_{own_gx:02d}_{own_gz:02d}_{own_p3:02d}'):
            continue
        if own_area != ent['area']: continue  # cross-area not handled here
        suffix = cur_map[-2:]
        scale = _SUFFIX_SCALE.get(suffix)
        if scale is None: continue
        agg = _FINE * scale
        offset_x = ent['gridX'] * agg + agg / 2 - own_gx * _FINE - _FINE / 2
        offset_z = ent['gridZ'] * agg + agg / 2 - own_gz * _FINE - _FINE / 2
        ent['x'] = round(ent['x'] + offset_x, 3)
        ent['z'] = round(ent['z'] + offset_z, 3)
        ent['gridX'] = own_gx
        ent['gridZ'] = own_gz
        ent['map'] = f'm{own_area:02d}_{own_gx:02d}_{own_gz:02d}_{own_p3:02d}'
        remapped += 1
    print(f"  Remapped {remapped} cross-tile entities to fine-grid owner tiles")

    # ── VANILLA seed: GameAreaParam ─────────────────────────────────────
    # The ERR path below seeds from ERR's pre-placed WorldMapPointParam boss
    # markers (textId2==5100), which vanilla doesn't have. Vanilla's
    # authoritative boss registry is GameAreaParam: row id == boss entity id,
    # each row carries defeatBossFlagId + bossPos + bossMap. Names: most boss
    # NpcParam rows have nameId==0 - the HP-bar name is assigned by EMEVD
    # DisplayBossHealthBar 2003[11] (entity, slot, nameId), so we scan the
    # event scripts (resolving parameterized common-event initializations).
    if config.PROFILE != 'err':
        import struct as _struct
        _emevd_read = asm.GetType('SoulsFormats.EMEVD').GetMethod(
            'Read', BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
            None, Array[SysType]([_str_type]), None)

        def scan_boss_bar_names(event_dir):
            """entity_id -> NpcName id from DisplayBossHealthBar 2003[11]
            (args: state u8@0, entity u32@4, slot s16@8, nameId s32@12)."""
            ent_name = {}
            param_events = {}  # event_id -> [(ent_src_off, ent_lit, nid_src_off, nid_lit)]
            ems = []
            for ep in sorted(event_dir.glob('*.emevd.dcx')):
                try:
                    em = rfb(_emevd_read, SoulsFormats.DCX.Decompress(str(ep)), '.emevd')
                except Exception:
                    continue
                ems.append(em)
                for ev in em.Events:
                    subs = {(int(pr.InstructionIndex), int(pr.TargetStartByte)):
                            int(pr.SourceStartByte) for pr in ev.Parameters}
                    for ii, ins in enumerate(ev.Instructions):
                        if int(ins.Bank) != 2003 or int(ins.ID) != 11:
                            continue
                        args = bytes(ins.ArgData)
                        if len(args) < 16:
                            continue
                        ent = _struct.unpack_from('<I', args, 4)[0]
                        nid = _struct.unpack_from('<i', args, 12)[0]
                        ent_off = subs.get((ii, 4))
                        nid_off = subs.get((ii, 12))
                        if ent_off is None and nid_off is None:
                            if ent > 0 and nid > 0:
                                ent_name.setdefault(ent, nid)
                        else:
                            param_events.setdefault(int(ev.ID), []).append(
                                (ent_off, ent, nid_off, nid))
            # resolve parameterized defs through InitializeEvent/CommonEvent args
            for em in ems:
                for ev in em.Events:
                    for ins in ev.Instructions:
                        if int(ins.Bank) != 2000 or int(ins.ID) not in (0, 6):
                            continue
                        args = bytes(ins.ArgData)
                        if len(args) < 8:
                            continue
                        tgt = _struct.unpack_from('<I', args, 4)[0]
                        if tgt not in param_events:
                            continue
                        blob = args[8:]
                        for ent_off, ent_lit, nid_off, nid_lit in param_events[tgt]:
                            ent, nid = ent_lit, nid_lit
                            if ent_off is not None and ent_off + 4 <= len(blob):
                                ent = _struct.unpack_from('<I', blob, ent_off)[0]
                            if nid_off is not None and nid_off + 4 <= len(blob):
                                nid = _struct.unpack_from('<i', blob, nid_off)[0]
                            if ent > 0 and nid > 0:
                                ent_name.setdefault(ent, nid)
            return ent_name

        bar_names = scan_boss_bar_names(ERR_MOD_DIR / 'event')
        print(f"  boss-bar name scan: {len(bar_names)} entity->name entries")
        npc_name_path = config.DATA_DIR / 'npc_name_ids.json'
        npc_name_ids = {}
        if npc_name_path.exists():
            npc_name_ids = {int(k): int(v) for k, v in
                            json.load(open(npc_name_path, encoding='utf-8')).items()}

        # NpcName texts (for vanillaPlaceName -> Great Runes name matching)
        npc_name_text = {}
        for f in msgbnd.Files:
            if 'NpcName' in str(f.Name):
                fmg = rfb(_fmg_read, f.Bytes, '.fmg')
                for e in fmg.Entries:
                    t = str(e.Text) if e.Text else ''
                    if t and t != '[ERROR]':
                        npc_name_text.setdefault(int(e.ID), t)

        ga = read_param(bnd, 'GameAreaParam', paramdefs)
        ga_fields = {'defeatBossFlagId', 'bossPosX', 'bossPosY', 'bossPosZ',
                     'bossMapAreaNo', 'bossMapBlockNo', 'bossMapMapNo'}
        ga_data = param_to_dict(ga, ga_fields)

        # GameAreaParam.defeatBossFlagId is, for many field bosses, the boss
        # ENTITY id - NOT the flag the game sets on kill (verified live: e.g. tile
        # m60_36_45 stored 1036450340 = entity, but the death flag is 1036450800).
        # The boss-death template events 90005860/61/80 carry the real defeat flag
        # at arg offset 8 and the boss entity at offset 16. Build entity -> flag so
        # we can map the (entity-valued) GameAreaParam field to the true flag.
        emevd_defeat = {}       # entity id -> real defeat flag (field bosses)
        dungeon_map_flags = {}  # map "mAA_BB_CC_00" -> set(defeat flags) (catacomb/dungeon)
        BOSS_DEATH_EVENTS = {90005860, 90005861, 90005880}  # field: arg0=flag, arg2=entity
        DUNGEON_BOSS_EVENT = 90005646                        # dungeon: arg0=flag
        for ep in sorted((ERR_MOD_DIR / 'event').glob('*.emevd.dcx')):
            mp = ep.name.replace('.emevd.dcx', '')
            try:
                em = rfb(_emevd_read, SoulsFormats.DCX.Decompress(str(ep)), '.emevd')
            except Exception:
                continue
            for ev in em.Events:
                for ins in ev.Instructions:
                    if int(ins.Bank) != 2000:
                        continue
                    a = bytes(ins.ArgData)
                    if len(a) < 12:
                        continue
                    eid = _struct.unpack_from('<i', a, 4)[0]
                    df = _struct.unpack_from('<i', a, 8)[0]
                    if eid in BOSS_DEATH_EVENTS and len(a) >= 20:
                        ent = _struct.unpack_from('<i', a, 16)[0]
                        if df > 0 and ent > 0:
                            emevd_defeat.setdefault(ent, df)
                    elif eid == DUNGEON_BOSS_EVENT and df > 0:
                        dungeon_map_flags.setdefault(mp, set()).add(df)
        # Only trust a dungeon's template flag when it is unambiguous (one boss).
        dungeon_flag = {m: next(iter(s)) for m, s in dungeon_map_flags.items() if len(s) == 1}
        print(f"  boss-death EMEVD scan: {len(emevd_defeat)} field entity->flag, "
              f"{len(dungeon_flag)} single-boss dungeon map->flag")

        result = []
        seen_flags = set()
        matched = 0
        for rid, row in sorted(ga_data.items()):
            flag = int(row.get('defeatBossFlagId', 0) or 0)
            if rid < 10000000 or flag <= 0:
                continue  # test rows (0, 1, 9999991/2)
            # Replace the entity-valued GameAreaParam field with the real death
            # flag from the boss-death event, matched by entity (rid) or by the
            # field value itself (== entity for field bosses).
            flag = emevd_defeat.get(rid) or emevd_defeat.get(flag) or flag
            # Dungeon/catacomb bosses: GameAreaParam can carry a stale flag (3 of
            # 55 mismatch the real death flag, e.g. m30_20 stored 30200810 but the
            # boss-death event 90005646 sets 30200800). Prefer the dungeon
            # template flag when the boss's map has a single unambiguous one.
            ba = int(row.get('bossMapAreaNo', 0) or 0)
            if ba and ba not in (60, 61):
                mp = (f"m{ba:02d}_{int(row.get('bossMapBlockNo',0) or 0):02d}"
                      f"_{int(row.get('bossMapMapNo',0) or 0):02d}_00")
                if mp in dungeon_flag:
                    flag = dungeon_flag[mp]
            if flag in seen_flags:
                continue  # boss duos share one defeat flag - one marker
            seen_flags.add(flag)

            ent = all_entities.get(rid)
            if ent is not None:
                area, gx, gz = ent['area'], ent['gridX'], ent['gridZ']
                x, y, z = ent['x'], ent['y'], ent['z']
                map_name = ent['map']
                model, npc = ent['model'], ent['npc']
                matched += 1
            else:
                area = int(row.get('bossMapAreaNo', 0) or 0)
                gx = int(row.get('bossMapBlockNo', 0) or 0)
                gz = int(row.get('bossMapMapNo', 0) or 0)
                if area <= 0:
                    continue
                x = round(float(row.get('bossPosX', 0) or 0), 3)
                y = round(float(row.get('bossPosY', 0) or 0), 3)
                z = round(float(row.get('bossPosZ', 0) or 0), 3)
                map_name = f'm{area:02d}_{gx:02d}_{gz:02d}_00'
                model, npc = '', 0

            # Name resolution: NpcParam.nameId when set; else the EMEVD
            # boss-bar assignment (DisplayBossHealthBar scan); else the
            # NpcName id convention 900000000 + npcId//10 as a last resort.
            name_id = npc_name_ids.get(npc, 0)
            if name_id <= 0:
                name_id = bar_names.get(rid, 0)
            if name_id <= 0 and npc > 0:
                cand = 900000000 + npc // 10
                if cand in npc_name_text:
                    name_id = cand
            if name_id > 0 and name_id not in npc_name_text:
                name_id = 0  # never bake an id the FMG can't resolve
            result.append({
                'areaNo': area, 'gridX': gx, 'gridZ': gz,
                'x': x, 'y': y, 'z': z,
                'map': map_name, 'enemyModel': model, 'npcParamId': npc,
                'npcNameId': name_id,
                'clearedEventFlagId': flag,
                'killEventFlagId': flag,
                'wmpTextId1': 0,
                'vanillaPlaceName': npc_name_text.get(name_id, ''),
            })

        out_path = config.DATA_DIR / 'boss_list.json'
        with open(out_path, 'w', encoding='utf-8') as f:
            json.dump(result, f, ensure_ascii=False, indent=2)
        named = sum(1 for b in result if b['npcNameId'] > 0)
        print(f"Vanilla bosses: {len(result)} (entity matched: {matched}, "
              f"named via NpcName: {named}, unnamed: {len(result) - named})")
        for b in result:
            if b['npcNameId'] <= 0:
                print(f"  unnamed: entity-row flag={b['clearedEventFlagId']} "
                      f"map={b['map']} model={b['enemyModel']} npc={b['npcParamId']}")
        return

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
