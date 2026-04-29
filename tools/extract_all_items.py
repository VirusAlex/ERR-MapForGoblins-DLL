#!/usr/bin/env python3
"""
Extract all treasure items from ERR mod MSB files + regulation.bin.
Outputs items_database.json with positions, items, event flags, and categories.
"""

import json
import struct
import sys
import io
import os
import time
from pathlib import Path

import config

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')

# Paths
ERR_MOD_DIR      = config.require_err_mod_dir()
MSB_DIR          = ERR_MOD_DIR / 'map' / 'MapStudio'
MSGBND_PATH      = ERR_MOD_DIR / 'msg' / 'engus' / 'item_dlc02.msgbnd.dcx'
OUTPUT_DIR       = config.DATA_DIR
PARAMDEF_DIR     = config.PARAMDEF_DIR

UNDERGROUND_AREAS = {12}
DLC_AREAS = {20, 21, 22, 25, 28, 40, 41, 42, 43, 61}

# .NET init
import tempfile
from pythonnet import load
load('coreclr')
import clr
from System.Reflection import Assembly, BindingFlags
from System import Array, Type as SysType, Object
from System.IO import File as SysFile

asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
clr.AddReference(str(config.SOULSFORMATS_DLL))
import SoulsFormats

# Andre.SoulsFormats uses Read(string) / Read(Memory<byte>), not Read(byte[])
# We use temp files + Read(string) via reflection as a bridge.
_str_type = SysType.GetType('System.String')

def _get_read_str(type_name):
    cls = asm.GetType(type_name)
    return cls.GetMethod('Read',
        BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
        None, Array[SysType]([_str_type]), None)

_param_read = _get_read_str('SoulsFormats.PARAM')
_bnd4_read  = _get_read_str('SoulsFormats.BND4')
_fmg_read   = _get_read_str('SoulsFormats.FMG')
_msbe_read  = _get_read_str('SoulsFormats.MSBE')


def _read_from_bytes(read_method, data, suffix='.bin'):
    tmp = os.path.join(tempfile.gettempdir(), f'_mfg_tmp{suffix}')
    if hasattr(data, 'ToArray'):
        SysFile.WriteAllBytes(tmp, data.ToArray())
    else:
        SysFile.WriteAllBytes(tmp, data)
    result = read_method.Invoke(None, Array[Object]([tmp]))
    os.unlink(tmp)
    return result


# Helpers
def load_paramdefs():
    defs = {}
    for xml_path in PARAMDEF_DIR.glob('*.xml'):
        try:
            pdef = SoulsFormats.PARAMDEF.XmlDeserialize(str(xml_path), False)
            if pdef and pdef.ParamType:
                defs[str(pdef.ParamType)] = pdef
        except Exception:
            pass
    return defs


def read_param(bnd, name, paramdefs):
    for f in bnd.Files:
        if name in str(f.Name):
            param = _read_from_bytes(_param_read, f.Bytes, '.param')
            pt = str(param.ParamType) if param.ParamType else ''
            if pt in paramdefs:
                param.ApplyParamdef(paramdefs[pt])
            return param
    return None


def param_to_dict(param, fields):
    result = {}
    for row in param.Rows:
        row_id = int(row.ID)
        entry = {}
        if row.Cells:
            for cell in row.Cells:
                fn = str(cell.Def.InternalName)
                if fn in fields:
                    val = cell.Value
                    if hasattr(val, 'ToString'):
                        val = int(str(val)) if str(val).isdigit() else str(val)
                    entry[fn] = val
        result[row_id] = entry
    return result


def read_fmg_names(bnd, fmg_filename):
    names = {}
    for f in bnd.Files:
        fname = str(f.Name)
        if fname.endswith(fmg_filename) and '_dlc' not in fname.lower():
            fmg = _read_from_bytes(_fmg_read, f.Bytes, '.fmg')
            for e in fmg.Entries:
                text = str(e.Text) if e.Text else ''
                if text and text != '[ERROR]':
                    names[int(e.ID)] = text
            break
    return names


def parse_map_name(msb_filename):
    name = msb_filename.replace('.msb.dcx', '').replace('.msb', '')
    parts = name.split('_')
    if len(parts) < 4:
        return None
    try:
        area = int(parts[0][1:])
        p1 = int(parts[1])
        p2 = int(parts[2])
        p3 = int(parts[3])
        return {'map': name, 'areaNo': area, 'p1': p1, 'p2': p2, 'p3': p3}
    except ValueError:
        return None


def get_disp_mask(area):
    if area in UNDERGROUND_AREAS:
        return 'dispMask01'
    elif area in DLC_AREAS:
        return 'pad2_0'
    return 'dispMask00'


def get_grid(area, p1, p2):
    if area in (60, 61) or area in DLC_AREAS:
        return p1, p2
    return p1, 0


# Category classification
GOODS_TYPE_MAP = {
    0: 'consumable', 1: 'consumable', 2: 'consumable',
    3: 'key_item', 5: 'key_item', 7: 'key_item',
    9: 'sorcery', 10: 'incantation',
    11: 'spirit_ash', 12: 'spirit_ash',
    14: 'crafting_material', 15: 'crafting_material',
    16: 'remembrance',
}

WEP_TYPE_MAP = {
    0: 'axe', 1: 'slash_sword', 2: 'thrust_sword', 3: 'greatsword',
    4: 'colossal_sword', 5: 'curved_sword', 6: 'curved_greatsword',
    7: 'katana', 8: 'twinblade', 9: 'hammer',
    10: 'great_hammer', 11: 'flail', 12: 'axe',
    13: 'greataxe', 14: 'spear', 15: 'great_spear',
    16: 'halberd', 17: 'reaper', 18: 'fist',
    19: 'claw', 20: 'whip', 21: 'colossal_weapon',
    23: 'light_bow', 24: 'bow', 25: 'greatbow',
    26: 'crossbow', 27: 'ballista',
    28: 'staff', 29: 'seal',
    30: 'small_shield', 31: 'medium_shield', 32: 'greatshield',
    33: 'torch',
    # DLC additions
    34: 'thrusting_shield', 35: 'hand_to_hand',
    36: 'throwing_blade', 37: 'backhand_blade',
    38: 'perfume_bottle', 39: 'beast_claw',
    40: 'light_greatsword', 41: 'great_katana',
}


def categorize_item(lot_cat, item_id, goods_db, weapon_db):
    if lot_cat == 2:
        wep_info = weapon_db.get(item_id, {})
        wt = wep_info.get('wepType', -1)
        sub = WEP_TYPE_MAP.get(wt, 'weapon')
        if wt in (28, 29):
            return 'magic_catalyst', sub
        if wt in (23, 24, 25, 26, 27):
            return 'ranged_weapon', sub
        if wt in (30, 31, 32, 34):
            return 'shield', sub
        return 'armament', sub

    if lot_cat == 3:
        return 'armour', 'armour'

    if lot_cat == 4:
        return 'talisman', 'talisman'

    if lot_cat == 5:
        return 'ash_of_war', 'ash_of_war'

    if lot_cat == 1:
        goods_info = goods_db.get(item_id, {})
        gt = goods_info.get('goodsType', 0)
        sub = GOODS_TYPE_MAP.get(gt, 'consumable')

        if sub == 'sorcery':
            return 'sorcery', 'sorcery'
        if sub == 'incantation':
            return 'incantation', 'incantation'
        if sub == 'spirit_ash':
            return 'spirit_ash', 'spirit_ash'
        if sub == 'crafting_material':
            return 'crafting_material', 'crafting_material'
        if sub == 'key_item':
            return 'key_item', 'key_item'
        if sub == 'remembrance':
            return 'key_item', 'remembrance'
        return 'consumable', 'consumable'

    return 'unknown', 'unknown'


def decode_itemlot_id(lot_id):
    """
    Derive map tile from ItemLotParam_map ID encoding.
    Overworld m60: 10XXYY#### → gridX=XX, gridZ=YY
    Legacy dungeons: AA00####  → area=AA
    DLC dungeons: AA##0####    → area=AA, sub=##
    """
    s = str(lot_id)

    # Overworld m60/m61: 10-digit IDs starting with 10 (for m60) or 21 (for m61)
    if len(s) == 10 and s.startswith('10'):
        gridX = int(s[2:4])
        gridZ = int(s[4:6])
        if 19 <= gridX <= 63 and 25 <= gridZ <= 62:
            return {'map': f'm60_{gridX:02d}_{gridZ:02d}_00', 'areaNo': 60,
                    'gridX': gridX, 'gridZ': gridZ}

    # DLC overworld m61: IDs starting with 21 and 10 digits
    if len(s) == 10 and s.startswith('21'):
        gridX = int(s[2:4])
        gridZ = int(s[4:6])
        return {'map': f'm61_{gridX:02d}_{gridZ:02d}_00', 'areaNo': 61,
                'gridX': gridX, 'gridZ': gridZ}

    # Legacy / indoor dungeons: 8-digit IDs
    if len(s) == 8:
        area = int(s[:2])
        sub = int(s[2:4])
        if 10 <= area <= 50:
            return {'map': f'm{area}_{sub:02d}_00_00', 'areaNo': area,
                    'gridX': sub, 'gridZ': 0}

    # DLC areas with 10-digit IDs: 20XXYY#### etc.
    if len(s) == 10:
        area = int(s[:2])
        if area in DLC_AREAS:
            sub = int(s[2:4])
            return {'map': f'm{area}_{sub:02d}_00_00', 'areaNo': area,
                    'gridX': sub, 'gridZ': 0}

    return None


def main():
    t0 = time.time()

    print('=== Loading regulation.bin ===')
    reg_path = ERR_MOD_DIR / 'regulation.bin'
    bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(reg_path))
    print(f'  {bnd.Files.Count} files in regulation')

    paramdefs = load_paramdefs()
    print(f'  {len(paramdefs)} paramdefs loaded')

    print('\n--- ItemLotParam_map ---')
    ilp = read_param(bnd, 'ItemLotParam_map', paramdefs)
    lot_fields = set()
    for i in range(1, 9):
        lot_fields.update([f'lotItemId0{i}', f'lotItemCategory0{i}', f'lotItemNum0{i}'])
    lot_fields.add('getItemFlagId')
    item_lots = param_to_dict(ilp, lot_fields)
    print(f'  {len(item_lots)} item lots')

    print('--- ItemLotParam_enemy ---')
    ilp_enemy = read_param(bnd, 'ItemLotParam_enemy', paramdefs)
    enemy_lot_fields = set()
    for i in range(1, 9):
        enemy_lot_fields.update([f'lotItemId0{i}', f'lotItemCategory0{i}', f'lotItemNum0{i}'])
    enemy_lot_fields.add('getItemFlagId')
    item_lots_enemy = param_to_dict(ilp_enemy, enemy_lot_fields)
    print(f'  {len(item_lots_enemy)} enemy item lots')

    print('--- NpcParam (enemy drop lots) ---')
    npc_param = read_param(bnd, 'NpcParam', paramdefs)
    npc_lots = param_to_dict(npc_param, {'itemLotId_map', 'itemLotId_enemy'})
    # Build NPC ID -> lot mappings (prefer map lot, fallback to enemy lot)
    npc_to_map_lot = {}
    npc_to_enemy_lot = {}
    for npc_id, fields in npc_lots.items():
        lot_map = fields.get('itemLotId_map', 0)
        lot_enemy = fields.get('itemLotId_enemy', 0)
        if lot_map > 0:
            npc_to_map_lot[npc_id] = lot_map
        if lot_enemy > 0:
            npc_to_enemy_lot[npc_id] = lot_enemy
    print(f'  {len(npc_lots)} NPCs, {len(npc_to_map_lot)} with map lots, {len(npc_to_enemy_lot)} with enemy lots')


    print('--- EquipParamWeapon ---')
    wparam = read_param(bnd, 'EquipParamWeapon', paramdefs)
    weapon_db = param_to_dict(wparam, {'wepType', 'sortId'})
    print(f'  {len(weapon_db)} weapons')

    print('--- EquipParamGoods ---')
    gparam = read_param(bnd, 'EquipParamGoods', paramdefs)
    goods_db = param_to_dict(gparam, {'goodsType', 'sortId'})
    print(f'  {len(goods_db)} goods')

    print('--- EquipParamProtector ---')
    pparam = read_param(bnd, 'EquipParamProtector', paramdefs)
    protector_db = param_to_dict(pparam, {'sortId'})
    print(f'  {len(protector_db)} protectors')

    print('--- EquipParamAccessory ---')
    aparam = read_param(bnd, 'EquipParamAccessory', paramdefs)
    accessory_db = param_to_dict(aparam, {'sortId'})
    print(f'  {len(accessory_db)} accessories')

    print('--- EquipParamGem ---')
    gemparam = read_param(bnd, 'EquipParamGem', paramdefs)
    gem_db = param_to_dict(gemparam, {'sortId'})
    print(f'  {len(gem_db)} gems')

    print('\n=== Loading FMG item names ===')
    msgbnd = _read_from_bytes(_bnd4_read, SoulsFormats.DCX.Decompress(str(MSGBND_PATH)), '.bnd')

    weapon_names = read_fmg_names(msgbnd, 'WeaponName.fmg')
    goods_names  = read_fmg_names(msgbnd, 'GoodsName.fmg')
    armor_names  = read_fmg_names(msgbnd, 'ProtectorName.fmg')
    talisman_names = read_fmg_names(msgbnd, 'AccessoryName.fmg')
    gem_names    = read_fmg_names(msgbnd, 'GemName.fmg')
    print(f'  Weapons: {len(weapon_names)}, Goods: {len(goods_names)}, '
          f'Armor: {len(armor_names)}, Talismans: {len(talisman_names)}, '
          f'Gems: {len(gem_names)}')

    name_dbs = {
        1: goods_names,
        2: weapon_names,
        3: armor_names,
        4: talisman_names,
        5: gem_names,
    }

    print('\n=== Scanning MSB files ===')
    msb_files = sorted(MSB_DIR.glob('*.msb.dcx'))
    print(f'  {len(msb_files)} MSB files to scan')

    treasures = []
    msb_errors = 0
    for idx, msb_path in enumerate(msb_files):
        if (idx + 1) % 100 == 0:
            print(f'  [{idx+1}/{len(msb_files)}] {msb_path.name}...')

        map_info = parse_map_name(msb_path.name)
        if not map_info:
            continue

        if map_info['p3'] == 99:  # test/debug variants
            continue

        try:
            msb = _read_from_bytes(_msbe_read, SoulsFormats.DCX.Decompress(str(msb_path)), '.msb')
        except Exception as e:
            msb_errors += 1
            continue

        live_positions = {}
        dummy_positions = {}
        dummy_entity_ids = {}
        for p in msb.Parts.Assets:
            live_positions[str(p.Name)] = {
                'x': float(p.Position.X),
                'y': float(p.Position.Y),
                'z': float(p.Position.Z),
            }
        for p in msb.Parts.DummyAssets:
            name = str(p.Name)
            dummy_positions[name] = {
                'x': float(p.Position.X),
                'y': float(p.Position.Y),
                'z': float(p.Position.Z),
            }
            # Capture EntityID + EntityGroupIDs to tell activator-reachable
            # DummyAssets from structurally-inert ones. A DummyAsset with
            # zero EntityID and zero EntityGroupIDs cannot be addressed by
            # any EMEVD instruction and is therefore unreachable.
            try:
                eid = int(p.EntityID) if hasattr(p, 'EntityID') else 0
            except Exception:
                eid = 0
            groups = []
            try:
                for g in getattr(p, 'EntityGroupIDs', []) or []:
                    if int(g) != 0:
                        groups.append(int(g))
            except Exception:
                pass
            dummy_entity_ids[name] = (eid, tuple(groups))

        # Group treasure events by lot and prefer the live part when a
        # single ItemLotID is bound to both a Parts.Assets and a
        # Parts.DummyAssets entry. Lots bound only to a dummy are skipped
        # entirely — the engine never spawns them.
        by_lot = {}
        for t in msb.Events.Treasures:
            part_name = str(t.TreasurePartName) if t.TreasurePartName else ''
            item_lot_id = int(t.ItemLotID)
            if item_lot_id <= 0 or not part_name:
                continue
            if part_name in live_positions:
                bucket, pos = 'live', live_positions[part_name]
            elif part_name in dummy_positions:
                bucket, pos = 'dummy', dummy_positions[part_name]
            else:
                continue
            by_lot.setdefault(item_lot_id, []).append((part_name, bucket, pos))

        for lot_id, refs in by_lot.items():
            live_refs = [r for r in refs if r[1] == 'live']
            if live_refs:
                part_name, bucket, pos = live_refs[0]
            else:
                # Only keep a dummy-only lot if the DummyAsset has a non-
                # zero EntityID or EntityGroupID — otherwise no EMEVD can
                # target it and the lot is unreachable in-game.
                reachable = []
                for r in refs:
                    eid, groups = dummy_entity_ids.get(r[0], (0, ()))
                    if eid != 0 or groups:
                        reachable.append(r)
                if not reachable:
                    continue
                part_name, _, pos = reachable[0]
                bucket = 'reachable_dummy'
            treasures.append({
                'map': map_info['map'],
                'areaNo': map_info['areaNo'],
                'p1': map_info['p1'],
                'p2': map_info['p2'],
                'x': pos['x'],
                'y': pos['y'],
                'z': pos['z'],
                'itemLotId': lot_id,
                'partName': part_name,
                'source': 'treasure',
                'partBucket': bucket,
            })

        # Enemy drops: NPC → NpcParam → itemLotId_map or itemLotId_enemy
        for p in msb.Parts.Enemies:
            npc_id = int(p.NPCParamID)
            lot_id = npc_to_map_lot.get(npc_id, 0)
            lot_source = 'map'
            if lot_id <= 0:
                lot_id = npc_to_enemy_lot.get(npc_id, 0)
                lot_source = 'enemy'
            if lot_id <= 0:
                continue
            name = str(p.Name)
            model = str(p.ModelName) if hasattr(p, 'ModelName') else ''
            treasures.append({
                'map': map_info['map'],
                'areaNo': map_info['areaNo'],
                'p1': map_info['p1'],
                'p2': map_info['p2'],
                'x': float(p.Position.X),
                'y': float(p.Position.Y),
                'z': float(p.Position.Z),
                'itemLotId': lot_id,
                'partName': name,
                'source': 'enemy',
                'enemyModel': model,
                'npcParamId': npc_id,
                'lotSource': lot_source,
            })

    print(f'  Found {len(treasures)} placements ({msb_errors} MSB errors)')
    enemy_count = sum(1 for t in treasures if t.get('source') == 'enemy')
    print(f'    ({len(treasures) - enemy_count} treasures + {enemy_count} enemy drops)')

    # ── EMEVD: template event drops (scarabs, mini-bosses, etc.) ──
    print('\n=== Scanning EMEVD for template event drops ===')

    _emevd_read = asm.GetType('SoulsFormats.EMEVD').GetMethod(
        'Read', BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
        None, Array[SysType]([_str_type]), None)

    # Template events that award items: event_id -> (entity_offset, lot_offset, min_args)
    # Scarab/enemy drops: entity at offset 8, lot at offset 16
    # Boss rewards (90005860+): entity at offset 16, lot at offset 24
    # NPC quest rewards (90005750): entity at offset 8, lot at offset 16
    # NPC invasion rewards (90005774): entity at offset 8, lot at offset 12
    # Painting pickups (90005632): entity at offset 8, lot at offset 16
    # Great Runes (90005110): entity at offset 8, lot at offset 20
    # Larval Tears (90005390): entity at offset 8, lot at offset 28
    TEMPLATE_EVENTS = {
        # Scarab/enemy drops
        90005200: (8, 16, 20),
        90005210: (8, 16, 20),
        90005300: (8, 16, 20),
        90005301: (8, 16, 20),
        # Boss rewards (field bosses, dungeon bosses)
        90005860: (16, 24, 28),
        90005861: (16, 24, 28),
        90005880: (16, 24, 28),
        90005881: (16, 24, 28),
        90005882: (16, 24, 28),
        90005883: (16, 24, 28),
        90005885: (16, 24, 28),
        # NPC quest/dialog rewards
        90005750: (8, 16, 20),
        # NPC invasion rewards
        90005774: (8, 12, 16),
        90005792: (8, 24, 28),
        # Painting pickups
        90005632: (8, 16, 20),
        # Great Runes
        90005110: (8, 20, 24),
        # Larval Tears (boss morph)
        90005390: (8, 28, 32),
        # DLC forging / special
        90005555: (8, 12, 16),
    }

    # Boss reward event IDs (have defeat flag at offset 8)
    BOSS_EVENTS = {90005860, 90005861, 90005880, 90005881, 90005882, 90005883, 90005885}

    emevd_dir = ERR_MOD_DIR / 'event'
    emevd_calls = []  # (entityId, lotId, map_name, eventId, defeatFlag)

    for emevd_path in sorted(emevd_dir.glob('*.emevd.dcx')):
        map_name = emevd_path.name.replace('.emevd.dcx', '')
        try:
            tmp2 = os.path.join(tempfile.gettempdir(), '_mfg_emevd.tmp')
            SysFile.WriteAllBytes(tmp2, SoulsFormats.DCX.Decompress(str(emevd_path)).ToArray())
            emevd = _emevd_read.Invoke(None, Array[Object]([tmp2]))
            os.unlink(tmp2)
        except:
            continue

        for event in emevd.Events:
            for instr in event.Instructions:
                if int(instr.Bank) != 2000:
                    continue
                args = bytes(instr.ArgData)
                if len(args) < 8:
                    continue
                event_id = struct.unpack_from('<i', args, 4)[0]
                if event_id not in TEMPLATE_EVENTS:
                    continue
                entity_off, lot_off, min_len = TEMPLATE_EVENTS[event_id]
                if len(args) < min_len:
                    continue
                entity_id = struct.unpack_from('<i', args, entity_off)[0]
                lot_id = struct.unpack_from('<i', args, lot_off)[0]
                # Extract defeat flag for boss events (offset 8 = args[2])
                defeat_flag = struct.unpack_from('<i', args, 8)[0] if event_id in BOSS_EVENTS else 0
                if lot_id > 0 and entity_id > 0:
                    emevd_calls.append((entity_id, lot_id, map_name, event_id, defeat_flag))

    print(f'  {len(emevd_calls)} template event calls with lot IDs')

    # Match EntityIDs to MSB positions (already collected in part_positions per MSB)
    # Re-scan MSBs for EntityID → position mapping
    entity_to_pos = {}
    emevd_entity_ids = {e[0] for e in emevd_calls}
    for msb_path in sorted(MSB_DIR.glob('*.msb.dcx')):
        map_info = parse_map_name(msb_path.name)
        if not map_info or map_info['p3'] == 99:
            continue
        try:
            tmp2 = os.path.join(tempfile.gettempdir(), '_mfg_msb2.tmp')
            SysFile.WriteAllBytes(tmp2, SoulsFormats.DCX.Decompress(str(msb_path)).ToArray())
            msb = _read_from_bytes(_msbe_read, SoulsFormats.DCX.Decompress(str(msb_path)), '.msb')
        except:
            continue
        for p in msb.Parts.Enemies:
            eid = int(p.EntityID)
            if eid in emevd_entity_ids and eid not in entity_to_pos:
                entity_to_pos[eid] = {
                    'x': float(p.Position.X), 'y': float(p.Position.Y), 'z': float(p.Position.Z),
                    'map': map_info['map'], 'areaNo': map_info['areaNo'],
                    'p1': map_info['p1'], 'p2': map_info['p2'],
                    'name': str(p.Name), 'model': str(p.ModelName),
                    'npcParam': int(p.NPCParamID),
                }

    emevd_matched = 0
    seen_emevd = set()
    for entity_id, lot_id, emevd_map, event_id, defeat_flag in emevd_calls:
        pos = entity_to_pos.get(entity_id)
        if not pos:
            continue
        dedup_key = (entity_id, lot_id)
        if dedup_key in seen_emevd:
            continue
        seen_emevd.add(dedup_key)
        entry = {
            'map': pos['map'], 'areaNo': pos['areaNo'],
            'p1': pos['p1'], 'p2': pos['p2'],
            'x': pos['x'], 'y': pos['y'], 'z': pos['z'],
            'itemLotId': lot_id, 'partName': pos['name'],
            'source': 'emevd', 'enemyModel': pos['model'],
            'npcParamId': pos.get('npcParam', 0),
        }
        if defeat_flag > 0:
            entry['defeatFlag'] = defeat_flag
            entry['emevdEventId'] = event_id
        treasures.append(entry)
        emevd_matched += 1

    print(f'  {emevd_matched} EMEVD drops matched to positions ({len(entity_to_pos)}/{len(emevd_entity_ids)} entities found)')

    print('\n=== Cross-referencing data ===')
    # Build set of all MSB treasure base lot IDs (to avoid treating them as sub-lots)
    treasure_base_lots = {tr['itemLotId'] for tr in treasures if tr.get('source') == 'treasure'}
    print(f'  {len(treasure_base_lots)} unique treasure base lot IDs')

    database = []
    no_lot = 0
    no_items = 0

    def extract_lot_items(lot):
        """Extract items from an ItemLotParam entry."""
        items = []
        for slot in range(1, 9):
            item_id = lot.get(f'lotItemId0{slot}', 0)
            cat = lot.get(f'lotItemCategory0{slot}', 0)
            num = lot.get(f'lotItemNum0{slot}', 0)
            if item_id <= 0 or cat <= 0:
                continue
            name = name_dbs.get(cat, {}).get(item_id, '')
            broad_cat, sub_cat = categorize_item(cat, item_id, goods_db, weapon_db)
            items.append({
                'id': item_id, 'category': cat, 'num': num,
                'name': name, 'broad_category': broad_cat, 'sub_category': sub_cat,
            })
        return items

    for tr in treasures:
        lot_id = tr['itemLotId']

        # For enemy drops, scan sequential lot entries (base+0, base+1, ...)
        # Each sub-entry may have its own getItemFlagId (one-time unique drops)
        is_enemy = tr.get('source') == 'enemy'
        lots_to_check = []

        if (is_enemy or tr.get('source') == 'emevd') and lot_id in item_lots_enemy:
            # Check base and sequential entries in enemy lots
            for offset in range(1000):  # up to 1000 sub-entries (some NPCs have many variants)
                sub_lot = item_lots_enemy.get(lot_id + offset)
                if sub_lot is not None:
                    lots_to_check.append((lot_id + offset, sub_lot))
        elif lot_id in item_lots:
            # Treasure: scan base + sequential sub-lots (chests can have multiple items)
            # Stop at sub-lots that are themselves another treasure's base lot
            lots_to_check.append((lot_id, item_lots[lot_id]))
            for offset in range(1, 20):
                sub_id = lot_id + offset
                if sub_id in treasure_base_lots:
                    break  # this is another treasure, not a sub-lot
                sub_lot = item_lots.get(sub_id)
                if sub_lot is None:
                    break  # gap in sequence, stop
                lots_to_check.append((sub_id, sub_lot))
        else:
            lot = item_lots_enemy.get(lot_id)
            if lot:
                lots_to_check.append((lot_id, lot))

        if not lots_to_check:
            no_lot += 1
            continue

        # Use the first lot for basic items, but collect flagged sub-lots separately
        base_lot_id, base_lot = lots_to_check[0]
        event_flag = base_lot.get('getItemFlagId', 0)
        items = extract_lot_items(base_lot)

        # For sub-lots with their own getItemFlagId, create separate records
        # Applies to both enemy drops and treasure chests with multiple items
        if len(lots_to_check) > 1:
            for sub_lot_id, sub_lot in lots_to_check[1:]:
                sub_flag = sub_lot.get('getItemFlagId', 0)
                if sub_flag <= 0:
                    continue
                sub_items = extract_lot_items(sub_lot)
                if not sub_items:
                    continue
                # Create a separate record for this flagged sub-lot
                gridX, gridZ = get_grid(tr['areaNo'], tr['p1'], tr['p2'])
                sub_record = {
                    'map': tr['map'],
                    'x': round(tr['x'], 3), 'y': round(tr['y'], 3), 'z': round(tr['z'], 3),
                    'areaNo': tr['areaNo'], 'gridX': gridX, 'gridZ': gridZ,
                    'dispMask': get_disp_mask(tr['areaNo']),
                    'itemLotId': sub_lot_id, 'eventFlag': sub_flag,
                    'partName': tr['partName'],
                    'items': sub_items,
                    'primary_category': sub_items[0]['broad_category'],
                    'source': tr.get('source', 'treasure'),
                }
                if tr.get('enemyModel'):
                    sub_record['enemyModel'] = tr['enemyModel']
                if tr.get('npcParamId'):
                    sub_record['npcParamId'] = tr['npcParamId']
                if tr.get('defeatFlag'):
                    sub_record['defeatFlag'] = tr['defeatFlag']
                if tr.get('emevdEventId'):
                    sub_record['emevdEventId'] = tr['emevdEventId']
                database.append(sub_record)

        if not items:
            no_items += 1
            continue

        gridX, gridZ = get_grid(tr['areaNo'], tr['p1'], tr['p2'])

        record = {
            'map': tr['map'],
            'x': round(tr['x'], 3),
            'y': round(tr['y'], 3),
            'z': round(tr['z'], 3),
            'areaNo': tr['areaNo'],
            'gridX': gridX,
            'gridZ': gridZ,
            'dispMask': get_disp_mask(tr['areaNo']),
            'itemLotId': lot_id,
            'eventFlag': event_flag,
            'partName': tr['partName'],
            'items': items,
            'primary_category': items[0]['broad_category'],
            'source': tr.get('source', 'treasure'),
        }
        if tr.get('enemyModel'):
            record['enemyModel'] = tr['enemyModel']
        if tr.get('npcParamId'):
            record['npcParamId'] = tr['npcParamId']
        if tr.get('defeatFlag'):
            record['defeatFlag'] = tr['defeatFlag']
        if tr.get('emevdEventId'):
            record['emevdEventId'] = tr['emevdEventId']
        if tr.get('partBucket'):
            record['partBucket'] = tr['partBucket']
        database.append(record)

    print(f'  {len(database)} records (no lot: {no_lot}, no items: {no_items})')

    # Items present in ItemLotParam_map but not matched to any MSB treasure
    print('\n=== Fallback: unmatched ItemLotParam_map entries ===')
    matched_lot_ids = {r['itemLotId'] for r in database}
    fallback_count = 0

    for lot_id, lot in item_lots.items():
        if lot_id in matched_lot_ids or lot_id <= 0:
            continue

        event_flag = lot.get('getItemFlagId', 0)

        items = []
        for slot in range(1, 9):
            item_id = lot.get(f'lotItemId0{slot}', 0)
            cat = lot.get(f'lotItemCategory0{slot}', 0)
            num = lot.get(f'lotItemNum0{slot}', 0)
            if item_id <= 0 or cat <= 0:
                continue
            name = name_dbs.get(cat, {}).get(item_id, '')
            broad_cat, sub_cat = categorize_item(cat, item_id, goods_db, weapon_db)
            items.append({
                'id': item_id, 'category': cat, 'num': num,
                'name': name, 'broad_category': broad_cat, 'sub_category': sub_cat,
            })

        if not items:
            continue

        map_info = decode_itemlot_id(lot_id)
        if not map_info:
            continue

        record = {
            'map': map_info['map'],
            'x': 0.0, 'y': 0.0, 'z': 0.0,
            'areaNo': map_info['areaNo'],
            'gridX': map_info.get('gridX', 0),
            'gridZ': map_info.get('gridZ', 0),
            'dispMask': get_disp_mask(map_info['areaNo']),
            'itemLotId': lot_id,
            'eventFlag': event_flag,
            'partName': '',
            'items': items,
            'primary_category': items[0]['broad_category'],
            'from_fallback': True,
        }
        database.append(record)
        fallback_count += 1

    print(f'  Added {fallback_count} fallback records')
    print(f'  Total: {len(database)} records')

    print('\n=== Category breakdown ===')
    cat_counts = {}
    for rec in database:
        cat = rec['primary_category']
        cat_counts[cat] = cat_counts.get(cat, 0) + 1
    for cat, cnt in sorted(cat_counts.items(), key=lambda x: -x[1]):
        print(f'  {cat}: {cnt}')

    out_path = OUTPUT_DIR / 'items_database.json'
    with open(out_path, 'w', encoding='utf-8') as f:
        json.dump(database, f, indent=2, ensure_ascii=False)

    elapsed = time.time() - t0
    print(f'\nFinished in {elapsed:.1f}s')
    print(f'Saved {len(database)} records to {out_path}')


if __name__ == '__main__':
    main()
