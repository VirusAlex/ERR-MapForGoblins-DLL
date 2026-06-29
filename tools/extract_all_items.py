#!/usr/bin/env python3
"""
Extract all treasure items from ERR mod MSB files + regulation.bin.
Outputs items_database.json with positions, items, event flags, and categories.
"""

import json
import re
import struct
import sys
import io
import os
import time
from collections import defaultdict
from pathlib import Path

import config

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')

# Paths
ERR_MOD_DIR      = config.require_err_mod_dir()
MSB_DIR          = ERR_MOD_DIR / 'map' / 'MapStudio'
MSGBND_PATH      = ERR_MOD_DIR / 'msg' / 'engus' / 'item_dlc02.msgbnd.dcx'
OUTPUT_DIR       = config.DATA_DIR
PARAMDEF_DIR     = config.PARAMDEF_DIR

from unreachable import is_unreachable_in_err
from emevd_broken import is_spawn_broken, report as report_broken_spawns

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


def _safe_unlink(path):
    try:
        os.unlink(path)
    except PermissionError:
        pass


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
    tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + f'_mfg_tmp{suffix}')
    if hasattr(data, 'ToArray'):
        SysFile.WriteAllBytes(tmp, data.ToArray())
    else:
        SysFile.WriteAllBytes(tmp, data)
    result = read_method.Invoke(None, Array[Object]([tmp]))
    _safe_unlink(tmp)
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
    # Merge the base FMG AND its _dlcNN variants (e.g. GoodsName.fmg +
    # GoodsName_dlc01.fmg). DLC-exclusive item names live ONLY in the _dlc FMGs;
    # skipping them left those items nameless, so the name-based loot categories
    # silently dropped DLC key items (Cookbook / Map Fragment / Bell Bearing /
    # Crystal Tear / Great Rune) on the non-ERR profiles. (ERR re-bakes DLC names
    # into the base FMG, so for ERR the _dlc files are redundant - merge is safe.)
    names = {}
    base = fmg_filename[:-4] if fmg_filename.lower().endswith('.fmg') else fmg_filename
    for f in bnd.Files:
        leaf = str(f.Name).replace('\\', '/').rsplit('/', 1)[-1]
        if leaf == fmg_filename or (leaf.startswith(base + '_dlc') and leaf.lower().endswith('.fmg')):
            fmg = _read_from_bytes(_fmg_read, f.Bytes, '.fmg')
            for e in fmg.Entries:
                text = str(e.Text) if e.Text else ''
                if text and text != '[ERROR]':
                    names[int(e.ID)] = text
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

    print('=== Known ERR EMEVD spawn issues ===')
    report_broken_spawns()

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
        lot_fields.update([f'lotItemId0{i}', f'lotItemCategory0{i}', f'lotItemNum0{i}',
                           f'lotItemBasePoint0{i}'])
    lot_fields.add('getItemFlagId')
    item_lots = param_to_dict(ilp, lot_fields)
    print(f'  {len(item_lots)} item lots')

    print('--- ItemLotParam_enemy ---')
    ilp_enemy = read_param(bnd, 'ItemLotParam_enemy', paramdefs)
    enemy_lot_fields = set()
    for i in range(1, 9):
        enemy_lot_fields.update([f'lotItemId0{i}', f'lotItemCategory0{i}', f'lotItemNum0{i}',
                                 f'lotItemBasePoint0{i}'])
    enemy_lot_fields.add('getItemFlagId')
    item_lots_enemy = param_to_dict(ilp_enemy, enemy_lot_fields)
    print(f'  {len(item_lots_enemy)} enemy item lots')

    print('--- NpcParam (enemy drop lots) ---')
    npc_param = read_param(bnd, 'NpcParam', paramdefs)
    npc_lots = param_to_dict(npc_param, {'itemLotId_map', 'itemLotId_enemy', 'nameId'})
    # Build NPC ID -> lot mappings (prefer map lot, fallback to enemy lot)
    npc_to_map_lot = {}
    npc_to_enemy_lot = {}
    npc_to_name_id = {}  # npcParamId -> NpcName FMG id (for named NPCs only)
    for npc_id, fields in npc_lots.items():
        lot_map = fields.get('itemLotId_map', 0)
        lot_enemy = fields.get('itemLotId_enemy', 0)
        name_id = fields.get('nameId', 0)
        if lot_map > 0:
            npc_to_map_lot[npc_id] = lot_map
        if lot_enemy > 0:
            npc_to_enemy_lot[npc_id] = lot_enemy
        if name_id > 0:
            npc_to_name_id[npc_id] = name_id
    print(f'  {len(npc_lots)} NPCs, {len(npc_to_map_lot)} with map lots, '
          f'{len(npc_to_enemy_lot)} with enemy lots, {len(npc_to_name_id)} with NpcName')


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
    # Lots whose MSB Treasure events bind ONLY to unreachable DummyAssets
    # (eid=0, no group IDs). These are orphan rows in ItemLotParam_map that
    # the engine never delivers - used to skip them in the fallback enrich
    # pass and avoid fabricating phantom map markers from byte-pattern
    # coincidences in EMEVD args (e.g. flag args of 2009:0 RegisterLadder).
    unreachable_only_lots = set()
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
            # GameEditionDisable=1 marks the part as disabled in this build
            # (debug/test placements or vanilla parts ERR removed). The engine
            # never spawns them, so treasures bound to them never deliver.
            if int(getattr(p, 'GameEditionDisable', 0) or 0) == 1:
                continue
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

        # An asset keeps ONE pickup binding: when several Treasure events
        # reference the same part with DIFFERENT lots, the one iterated last
        # wins during map load and the earlier lots are never awarded.
        # (Verified in-game at Ruin-Strewn Precipice: corpse AEG099_620_9000
        # carries lots 39200000 Golden Rune + 39200080 Rune Arc - only the
        # Rune Arc drops. Only 2 such parts exist in the whole game.)
        # A marker for an overridden lot would point at unobtainable loot.
        part_last_lot = {}
        for t in msb.Events.Treasures:
            pn = str(t.TreasurePartName) if t.TreasurePartName else ''
            il = int(t.ItemLotID)
            if il > 0 and pn:
                part_last_lot[pn] = il

        # Group treasure events by lot and prefer the live part when a
        # single ItemLotID is bound to both a Parts.Assets and a
        # Parts.DummyAssets entry. Lots bound only to a dummy are skipped
        # entirely - the engine never spawns them.
        by_lot = {}
        for t in msb.Events.Treasures:
            part_name = str(t.TreasurePartName) if t.TreasurePartName else ''
            item_lot_id = int(t.ItemLotID)
            if item_lot_id <= 0 or not part_name:
                continue
            if part_last_lot.get(part_name) != item_lot_id:
                # Overridden by a later Treasure event on the same part -
                # never awarded in-game. Keep it out of enrich too.
                unreachable_only_lots.add(item_lot_id)
                print(f"  skipping overridden treasure lot {item_lot_id} on "
                      f"{map_info['map']}/{part_name} (part's final lot = "
                      f"{part_last_lot.get(part_name)})")
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
                # zero EntityID or EntityGroupID - otherwise no EMEVD can
                # target it and the lot is unreachable in-game.
                reachable = []
                for r in refs:
                    eid, groups = dummy_entity_ids.get(r[0], (0, ()))
                    if eid != 0 or groups:
                        reachable.append(r)
                if not reachable:
                    unreachable_only_lots.add(lot_id)
                    continue
                part_name, _, pos = reachable[0]
                bucket = 'reachable_dummy'

            # Manual exclude list for assets ERR displaced (down into terrain
            # or up out of reach) vs vanilla. The check is conditional on
            # actual vs vanilla Y, so it self-disarms if a future ERR update
            # fixes the position. Also mark the lot unreachable so the EMEVD
            # enrich-fallback pass doesn't re-glue it back by byte-match.
            if is_unreachable_in_err(map_info['map'], part_name, pos['y']):
                unreachable_only_lots.add(lot_id)
                continue

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
            # Skip enemies disabled in this build (debug placements, leftover
            # vanilla entities replaced by ERR, etc.). The engine never spawns
            # them, so their drops never trigger and any map marker is phantom.
            if int(getattr(p, 'GameEditionDisable', 0) or 0) == 1:
                continue
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
            # Skip drops whose spawn chain never completes in the current
            # ERR EMEVD (registered in emevd_broken.py; currently m30_08
            # c4020_9000). The check loads ERR EMEVD live, so if a future
            # ERR version changes the event the filter self-disarms.
            if is_spawn_broken(map_info['map'], name):
                continue
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

    # ── Cross-tile placeholder remap ──
    # Some overworld assets (caravan loot, instance-specific chests) live in
    # aggregate "_01"/"_02"/"_12" placeholder MSBs but encode their real
    # owner tile in the partName prefix, e.g.
    #   placeholder map  m60_11_10_02
    #   partName         "m60_47_40_00-AEG100_101_1001"
    #   owner tile       m60_47_40_00
    # The placeholder uses a coarser tile grid (4x4 fine tiles for _02/_12,
    # 2x2 for _01), so positions in placeholder local coords need to be
    # offset back into owner-tile local coords before we can render an
    # icon on the right map fragment.
    SUFFIX_SCALE = {'01': 2, '02': 4, '12': 4}
    FINE_TILE_SIZE = 256
    _prefix_re = re.compile(r'^m(\d{2})_(\d{2})_(\d{2})_(\d{2})-')
    remapped = 0
    for tr in treasures:
        m = _prefix_re.match(tr.get('partName', ''))
        if not m: continue
        own_area, own_gx, own_gz, own_p3 = (int(g) for g in m.groups())
        cur_map = tr['map']
        # Skip if partName already matches the MSB (regular asset)
        if cur_map.startswith(f'm{own_area:02d}_{own_gx:02d}_{own_gz:02d}_{own_p3:02d}'):
            continue
        # Only handle the same-area placeholder case (m60→m60, m61→m61).
        # Cross-area cases (m34_NN → m60_XX, legacy dungeons) need
        # WorldMapLegacyConvParam handling and are out of scope here.
        if own_area != tr['areaNo']: continue
        suffix = cur_map[-2:]  # e.g. '02'
        scale = SUFFIX_SCALE.get(suffix)
        if scale is None: continue
        agg_size = FINE_TILE_SIZE * scale
        # Local-coord offset from placeholder origin to owner origin.
        # Both tiles are center-origin in local coords.
        offset_x = tr['p1'] * agg_size + agg_size / 2 \
                 - own_gx * FINE_TILE_SIZE - FINE_TILE_SIZE / 2
        offset_z = tr['p2'] * agg_size + agg_size / 2 \
                 - own_gz * FINE_TILE_SIZE - FINE_TILE_SIZE / 2
        tr['x'] += offset_x
        tr['z'] += offset_z
        tr['p1'] = own_gx
        tr['p2'] = own_gz
        tr['map'] = f'm{own_area:02d}_{own_gx:02d}_{own_gz:02d}_{own_p3:02d}'
        remapped += 1
    print(f'    Remapped {remapped} placeholder records to fine-grid owner tiles')

    # ── EMEVD: template event drops (scarabs, mini-bosses, etc.) ──
    print('\n=== Scanning EMEVD for template event drops ===')

    _emevd_read = asm.GetType('SoulsFormats.EMEVD').GetMethod(
        'Read', BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
        None, Array[SysType]([_str_type]), None)

    # Template events that award items: event_id -> (entity_offset, lot_offset, min_args)
    # Each entry verified against DarkScript3 ER emedf:
    # the template body MUST contain `2003:36 Award Items (Including Clients)`
    # (or `2003:04 Award Item Lot`) with the lot arg bound to caller srcByte
    # (lot_offset - 8). Templates without a real award instruction are NOT listed
    # here (e.g. 90005200/90005210 are animation-only "ambush wake-up" templates,
    # 90005881-90005885 are boss-state machines with no item drop).
    #
    # Offsets:
    #   Scarab/enemy drops:               entity@8,  lot@16
    #   Boss rewards (90005860+):         entity@16, lot@24
    #   NPC quest rewards (90005750):     entity@8,  lot@16
    #   NPC invasion rewards (90005774):  entity@8,  lot@12
    #   Hostile NPC defeat (90005792):    entity@20, lot@24  (X12_4 char, X16_4 lot)
    #   Painting pickups (90005632):      entity@8,  lot@16
    #   Great Runes (90005110):           entity@8,  lot@20
    #   Larval Tears (90005390):          entity@8,  lot@28
    #   NPC quest reward variant 90005753: entity@8, lot@16 (asset-tied; e.g. Volcanic Storm)
    TEMPLATE_EVENTS = {
        # Scarab/enemy drops
        90005300: (8, 16, 20),
        90005301: (8, 16, 20),
        # Boss rewards (field bosses, dungeon bosses).
        # 90005860/61: boss entity = X8_4 (args@16), award lot = X16_4 (args@24).
        # 90005880 has a DIFFERENT layout: boss entity = X12_4 (args@20), lot = X16_4
        # (args@24) - verified against common_func (WaitFor(CharacterDead(X12_4)) +
        # AwardItemsIncludingClients(X16_4)). Reading entity@16 here grabbed X8_4 (a
        # flag, not an MSB part), so the reward never matched a position and the loot
        # marker was silently dropped (e.g. Convergence's Red Wolf of Radagon ->
        # Warding Remnant). entity@20 fixes every 90005880-awarded boss.
        90005860: (16, 24, 28),
        90005861: (16, 24, 28),
        90005880: (20, 24, 28),
        # NPC quest/dialog rewards
        90005750: (8, 16, 20),
        90005753: (8, 16, 20),
        # NPC invasion rewards (90005774 = "pseudo multi reward")
        90005774: (8, 12, 16),
        # Hostile NPC defeat: X0_4=defeat flag, X12_4=char, X16_4=lot.
        # entity offset = 8 (params start) + 12 (X12_4) = 20 in args.
        # lot offset = 8 + 16 = 24.
        90005792: (20, 24, 28),
        # Painting pickups
        90005632: (8, 16, 20),
        # Great Runes
        90005110: (8, 20, 24),
        # Larval Tears (boss morph)
        90005390: (8, 28, 32),
        # DLC forging / special
        90005555: (8, 12, 16),
    }

    # Event IDs whose first param (args byte 8) carries an NPC defeat /
    # completion flag. Used for marker hide-on-kill (clearedEventFlagId).
    # Bosses: 90005860/61/80 - X0_4 = boss defeat flag.
    # Hostile NPCs: 90005792 - X0_4 = invader defeat flag.
    BOSS_EVENTS = {90005860, 90005861, 90005880, 90005792}

    # Scarab / strong-enemy drop templates (90005300/301). Their X0_4 (args byte 8)
    # is the kill flag the template SetEventFlag's on death ([12] SetEventFlag(X0_4)).
    # Unlike boss rewards (90005860+), these have NO 2004:76 RegisterItemAwardOnFlag -
    # they just AwardItems(IncludingClients)(lot), which does NOT set the lot's
    # getItemFlagId. So a scarab marker hidden via the lot's getItemFlagId never hides
    # (the flag is never set in-game - confirmed report: Determination/Unsheathe icons
    # stayed after looting). The reliable signal is X0_4, so we capture it and use it
    # as the marker's hide flag instead of the lot flag.
    SCARAB_EVENTS = {90005300, 90005301}

    emevd_dir = ERR_MOD_DIR / 'event'
    emevd_calls = []  # (entityId, lotId, map_name, eventId, defeatFlag)

    for emevd_path in sorted(emevd_dir.glob('*.emevd.dcx')):
        map_name = emevd_path.name.replace('.emevd.dcx', '')
        try:
            tmp2 = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_mfg_emevd.tmp')
            SysFile.WriteAllBytes(tmp2, SoulsFormats.DCX.Decompress(str(emevd_path)).ToArray())
            emevd = _emevd_read.Invoke(None, Array[Object]([tmp2]))
            _safe_unlink(tmp2)
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
                # Extract defeat/kill flag (offset 8 = X0_4) for boss events AND
                # scarab drops - both SetEventFlag(X0_4) on death.
                defeat_flag = (struct.unpack_from('<i', args, 8)[0]
                               if event_id in BOSS_EVENTS or event_id in SCARAB_EVENTS else 0)
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
            tmp2 = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_mfg_msb2.tmp')
            SysFile.WriteAllBytes(tmp2, SoulsFormats.DCX.Decompress(str(msb_path)).ToArray())
            msb = _read_from_bytes(_msbe_read, SoulsFormats.DCX.Decompress(str(msb_path)), '.msb')
        except:
            continue
        for p in msb.Parts.Enemies:
            if int(getattr(p, 'GameEditionDisable', 0) or 0) == 1:
                continue
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

    # ── EMEVD: Event-flag-driven unique drops (common.emevd event 1200 template) ──
    #
    # The vanilla ER reward mechanism for unique items like spirit ashes that drop
    # from named bosses works via a generic template `event 1200` in common.emevd:
    #
    #   common.emevd ev0 [N]: RunEvent(slot, eventId=1200,
    #                                  params=[trigger_flag, lot_id, _, shared_flag])
    #
    # Event 1200 body:
    #   [0] 2004:76  RegisterItemAwardOnFlag(trigger_flag, lot_id)  -- engine-level
    #   [1] END IF Event Flag(shared_flag)  -- skip if already awarded
    #   [2] IF Event Flag(trigger_flag)     -- wait until trigger fires
    #   [4] Award Items (Including Clients)(lot_id)  -- deliver
    #
    # When a per-map emevd event sets the trigger_flag (e.g. ev30002800 sets flag
    # 9200 on Tombsward Cemetery Shade death), event 1200 awards the matching lot
    # (e.g. lot 20000 -> Lhutel the Headless).
    #
    # Neither the trigger event nor the RunEvent call carry the entity_id
    # explicitly, so we recover it heuristically: walk the trigger event's
    # instructions, find any entity_id reference matching an MSB enemy in the
    # same map; prefer boss-like entities (eid%1000 in 800..899).
    print('\n=== Scanning common.emevd for RunEvent(1200, ...) - flag→lot map ===')
    flag_to_lot = {}
    common_path = ERR_MOD_DIR / 'event' / 'common.emevd.dcx'
    if common_path.exists():
        try:
            tmp_c = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_mfg_common.tmp')
            SysFile.WriteAllBytes(tmp_c, SoulsFormats.DCX.Decompress(str(common_path)).ToArray())
            common_em = _emevd_read.Invoke(None, Array[Object]([tmp_c]))
            _safe_unlink(tmp_c)
            # Scan EVERY event, not just ev0. Vanilla/ERR register all their
            # RunEvent(1200,...) award calls in common ev0 (the constructor), but
            # overhauls can put theirs in a separate event - e.g. The Convergence
            # registers its custom boss/chest drops in common ev9008002. Limiting
            # the scan to ev0 silently dropped every such award (e.g. Convergence's
            # Stormcaller Boss -> Stormcaller's Dirk, flag 9207 -> lot 20070). The
            # eid_call==1200 filter below keeps this precise regardless of event id.
            for event in common_em.Events:
                for instr in event.Instructions:
                    if int(instr.Bank) != 2000 or int(instr.ID) != 0:
                        continue  # RunEvent (2000:00) only
                    args = bytes(instr.ArgData) if instr.ArgData else b''
                    if len(args) < 24:
                        continue
                    eid_call = struct.unpack_from('<i', args, 4)[0]
                    if eid_call != 1200:
                        continue
                    trig_flag = struct.unpack_from('<i', args, 8)[0]
                    lot_id    = struct.unpack_from('<i', args, 12)[0]
                    if trig_flag > 0 and lot_id > 0:
                        flag_to_lot[trig_flag] = lot_id
        except Exception as e:
            print(f'  WARN: failed to scan common.emevd: {e}')
    print(f'  {len(flag_to_lot)} (flag→lot) pairs registered via event 1200')

    # Find Set Event Flag instructions that trigger these lots, grouped by map+event
    setter_events = []  # (flag_id, map_name, event_id)
    for emevd_path in sorted(emevd_dir.glob('*.emevd.dcx')):
        map_name = emevd_path.name.replace('.emevd.dcx', '')
        if map_name in ('common', 'common_func'):
            continue
        try:
            tmp2 = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_mfg_emevd2.tmp')
            SysFile.WriteAllBytes(tmp2, SoulsFormats.DCX.Decompress(str(emevd_path)).ToArray())
            emevd = _emevd_read.Invoke(None, Array[Object]([tmp2]))
            _safe_unlink(tmp2)
        except Exception:
            continue
        for event in emevd.Events:
            for instr in event.Instructions:
                bank = int(instr.Bank); iid = int(instr.ID)
                # 2003:66 = Set Event Flag, 2003:69 = Set Network-Connected Event Flag.
                # Both have layout (u8 target_type, u32 flag_id, u8 state) packed as i32×3.
                if bank != 2003 or iid not in (66, 69):
                    continue
                args = bytes(instr.ArgData) if instr.ArgData else b''
                if len(args) < 12:
                    continue
                flag_id = struct.unpack_from('<i', args, 4)[0]
                state   = struct.unpack_from('<i', args, 8)[0]
                if state != 1 or flag_id not in flag_to_lot:
                    continue
                setter_events.append((flag_id, map_name, int(event.ID)))
    print(f'  {len(setter_events)} Set Event Flag(<trigger>, 1) instructions found')

    # For each setter event, find the entity it references in MSB; prefer boss eid.
    #
    # entity_to_pos so far only contains entities referenced by TEMPLATE_EVENTS
    # callers - boss entities like Tombsward's 30000800 aren't included because
    # they're spawned/managed by per-map emevd events, not by common_func
    # templates. Extend entity_to_pos by scanning every MSB enemy for the maps
    # that have setter events.
    emevd1200_matched = 0
    seen_pairs = set()  # (entity_id, lot_id) - dedup
    maps_with_setters = {m for _, m, _ in setter_events}
    for msb_path in sorted(MSB_DIR.glob('*.msb.dcx')):
        map_info_msb = parse_map_name(msb_path.name)
        if not map_info_msb:
            continue
        if map_info_msb['map'] not in maps_with_setters:
            continue
        if map_info_msb.get('p3') == 99:
            continue
        try:
            msb = _read_from_bytes(_msbe_read, SoulsFormats.DCX.Decompress(str(msb_path)), '.msb')
        except Exception:
            continue
        for p in msb.Parts.Enemies:
            if int(getattr(p, 'GameEditionDisable', 0) or 0) == 1:
                continue
            eid = int(p.EntityID)
            if eid <= 0 or eid in entity_to_pos:
                continue
            entity_to_pos[eid] = {
                'x': float(p.Position.X), 'y': float(p.Position.Y), 'z': float(p.Position.Z),
                'map': map_info_msb['map'], 'areaNo': map_info_msb['areaNo'],
                'p1': map_info_msb['p1'], 'p2': map_info_msb['p2'],
                'name': str(p.Name), 'model': str(p.ModelName),
                'npcParam': int(p.NPCParamID),
            }
    map_to_entities = defaultdict(set)
    for eid, info in entity_to_pos.items():
        map_to_entities[info['map']].add(eid)

    # Re-read each map's emevd once and cache event-id -> referenced entities
    map_event_entities = {}  # (map_name, event_id) -> set of entity_ids
    maps_needed = {m for _, m, _ in setter_events}
    for map_name in sorted(maps_needed):
        em_path = emevd_dir / f'{map_name}.emevd.dcx'
        if not em_path.exists():
            continue
        try:
            tmp3 = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_mfg_emevd3.tmp')
            SysFile.WriteAllBytes(tmp3, SoulsFormats.DCX.Decompress(str(em_path)).ToArray())
            em = _emevd_read.Invoke(None, Array[Object]([tmp3]))
            _safe_unlink(tmp3)
        except Exception:
            continue
        valid_entities = map_to_entities.get(map_name, set())
        for event in em.Events:
            event_id = int(event.ID)
            refs = set()
            for instr in event.Instructions:
                args = bytes(instr.ArgData) if instr.ArgData else b''
                for i in range(0, len(args) - 3, 4):
                    v = struct.unpack_from('<i', args, i)[0]
                    if v in valid_entities:
                        refs.add(v)
            if refs:
                map_event_entities[(map_name, event_id)] = refs

    for flag_id, map_name, event_id in setter_events:
        lot_id = flag_to_lot[flag_id]
        refs = map_event_entities.get((map_name, event_id), set())
        if not refs:
            continue
        # Prefer boss-like entities (last 3 digits in 800-899 range)
        boss_like = sorted(e for e in refs if 800 <= (e % 1000) <= 899)
        chosen = boss_like[0] if boss_like else sorted(refs)[0]
        dedup_key = (chosen, lot_id)
        if dedup_key in seen_pairs:
            continue
        seen_pairs.add(dedup_key)
        pos = entity_to_pos[chosen]
        entry = {
            'map': pos['map'], 'areaNo': pos['areaNo'],
            'p1': pos['p1'], 'p2': pos['p2'],
            'x': pos['x'], 'y': pos['y'], 'z': pos['z'],
            'itemLotId': lot_id, 'partName': pos['name'],
            'source': 'emevd', 'enemyModel': pos['model'],
            'npcParamId': pos.get('npcParam', 0),
            'defeatFlag': flag_id, 'emevdEventId': event_id,
        }
        treasures.append(entry)
        emevd1200_matched += 1
    print(f'  {emevd1200_matched} event-1200 unique drops matched to entities')

    # ── EMEVD: scripted LITERAL item awards in per-map events ──
    # Some enemy/boss drops are delivered by a bespoke per-map event that calls
    # AwardItems(IncludingClients) (2003:36) / AwardItemLot (2003:04) with a
    # LITERAL lot id, gated on a specific enemy's death - not via a common_func
    # template (handled above) nor the event-1200 flag mechanism. Example: the
    # Fort Haight tower (m60_46_36 ev1046362300) waits for the Godrick Knight
    # (1046360250) to die, then AwardItems(1046360700) = Ash of War: Bloody Slash.
    # These lots otherwise only surface as a 0,0,0 fallback (or not at all, for
    # short ids decode_itemlot_id can't map), so the marker was missing or sat in
    # the tile corner. We pair each literally-awarded lot with the enemy
    # referenced in the same event: a death-checked one (IFCharacterDeadAlive,
    # 4:0) first, else a boss-like 800-899 entity, else the sole referenced enemy.
    print('\n=== Scanning per-map EMEVD for scripted literal AwardItems drops ===')
    # Roundtable Hold (m18) is not on the world map; its AwardItems are starting
    # gifts / merchant stock, not field drops - skip it.
    HUB_MAPS = {'m18_00_00_00'}
    # Rune Piece / Runic Trace goods have a dedicated AEG099_821 tracker; don't
    # fabricate generic loot markers for them here.
    RUNE_GOODS = {800010, 800011}
    existing_lot_ids = {t['itemLotId'] for t in treasures}

    award_events = []  # (map_name, event_id, [lots], death_vals, all_vals)
    for emevd_path in sorted(emevd_dir.glob('*.emevd.dcx')):
        map_name = emevd_path.name.replace('.emevd.dcx', '')
        if map_name in ('common', 'common_func') or map_name in HUB_MAPS:
            continue
        try:
            tmp4 = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_mfg_emevd4.tmp')
            SysFile.WriteAllBytes(tmp4, SoulsFormats.DCX.Decompress(str(emevd_path)).ToArray())
            em = _emevd_read.Invoke(None, Array[Object]([tmp4]))
            _safe_unlink(tmp4)
        except Exception:
            continue
        for event in em.Events:
            evparams = list(event.Parameters) if event.Parameters else []
            # instruction indices whose arg offset 0 (the lot id) is a parameter
            # placeholder - their literal ArgData is a default (usually 0), so the
            # real lot comes from the caller. Skip those to avoid phantom lots.
            param_lot_instr = {int(p.InstructionIndex) for p in evparams
                               if int(p.TargetStartByte) == 0}
            lots = []
            death_vals = set()
            all_vals = set()
            for idx, instr in enumerate(event.Instructions):
                bank = int(instr.Bank); iid = int(instr.ID)
                args = bytes(instr.ArgData) if instr.ArgData else b''
                if bank == 2003 and iid in (4, 36) and len(args) >= 4 \
                        and idx not in param_lot_instr:
                    lot = struct.unpack_from('<i', args, 0)[0]
                    if lot > 0:
                        lots.append(lot)
                vals = [struct.unpack_from('<i', args, i)[0]
                        for i in range(0, len(args) - 3, 4)]
                all_vals.update(vals)
                if bank == 4 and iid == 0:  # IF Character Dead/Alive
                    death_vals.update(vals)
            if lots:
                award_events.append((map_name, int(event.ID), lots, death_vals, all_vals))

    # Index MSB enemies for the maps that have literal-award events.
    award_maps = {m for m, _, _, _, _ in award_events}
    award_enemy_pos = {}
    award_enemy_set = defaultdict(set)
    for msb_path in sorted(MSB_DIR.glob('*.msb.dcx')):
        mi = parse_map_name(msb_path.name)
        if not mi or mi['map'] not in award_maps or mi['p3'] == 99:
            continue
        try:
            msb = _read_from_bytes(_msbe_read, SoulsFormats.DCX.Decompress(str(msb_path)), '.msb')
        except Exception:
            continue
        for p in msb.Parts.Enemies:
            if int(getattr(p, 'GameEditionDisable', 0) or 0) == 1:
                continue
            eid = int(p.EntityID)
            if eid <= 0:
                continue
            award_enemy_set[mi['map']].add(eid)
            if eid not in award_enemy_pos:
                award_enemy_pos[eid] = {
                    'x': float(p.Position.X), 'y': float(p.Position.Y), 'z': float(p.Position.Z),
                    'map': mi['map'], 'areaNo': mi['areaNo'], 'p1': mi['p1'], 'p2': mi['p2'],
                    'name': str(p.Name), 'model': str(p.ModelName), 'npcParam': int(p.NPCParamID),
                }

    scripted_matched = 0
    seen_scripted = set()
    for map_name, evid, lots, death_vals, all_vals in award_events:
        ents = award_enemy_set.get(map_name, set())
        death = sorted(e for e in death_vals if e in ents)
        allm = sorted(e for e in all_vals if e in ents)
        # Menu/shop guard: a NON-death event awarding many distinct lots is an
        # upgrade/exchange menu (e.g. a spirit-tuner offering Twinned Spirit
        # Jellyfish +0..+10), not a set of drops - it would stack N markers on
        # one NPC. Real combat drops are death-gated or award only a few lots.
        if not death and len(set(lots)) > 3:
            continue
        if death:
            cand = death
        else:
            boss = [e for e in allm if 800 <= (e % 1000) <= 899]
            if boss:
                cand = boss
            elif len(allm) == 1:
                cand = allm
            else:
                continue  # ambiguous - no clear enemy to pin the drop on
        chosen = sorted([e for e in cand if 800 <= (e % 1000) <= 899] or cand)[0]
        pos = award_enemy_pos.get(chosen)
        if not pos:
            continue
        for lot in set(lots):
            if lot in existing_lot_ids:
                continue  # already harvested by a treasure/enemy/template pass
            lot_def = item_lots.get(lot) or item_lots_enemy.get(lot)
            if not lot_def:
                continue
            if any(lot_def.get(f'lotItemId0{s}', 0) in RUNE_GOODS for s in range(1, 9)):
                continue
            if (chosen, lot) in seen_scripted:
                continue
            seen_scripted.add((chosen, lot))
            treasures.append({
                'map': pos['map'], 'areaNo': pos['areaNo'],
                'p1': pos['p1'], 'p2': pos['p2'],
                'x': pos['x'], 'y': pos['y'], 'z': pos['z'],
                'itemLotId': lot, 'partName': pos['name'],
                'source': 'emevd', 'enemyModel': pos['model'],
                'npcParamId': pos.get('npcParam', 0),
            })
            scripted_matched += 1
    print(f'  {scripted_matched} scripted literal-award drops matched to entities')

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

    def lot_is_guaranteed(lot):
        """A lot is GUARANTEED (the item always drops) when it has no "nothing"
        slot - i.e. no empty slot (lotItemId 0) carrying draw weight. With no
        empty slot competing, the weighted item slot(s) always win. This is how
        vanilla wires one-time set drops (e.g. the Battlemage Set lots 703-706:
        single item at basePoint 1000, no empty slot). Probabilistic/pity drops
        always have a heavy empty slot (e.g. nothing 985 vs item 15)."""
        for slot in range(1, 9):
            iid = lot.get(f'lotItemId0{slot}', 0)
            bp = lot.get(f'lotItemBasePoint0{slot}', 0)
            if iid == 0 and bp > 0:
                return False
        return True

    for tr in treasures:
        lot_id = tr['itemLotId']

        # For enemy drops, scan sequential lot entries (base+0, base+1, ...)
        # Each sub-entry may have its own getItemFlagId (one-time unique drops)
        is_enemy = tr.get('source') == 'enemy'
        lots_to_check = []

        if (is_enemy or tr.get('source') == 'emevd') and lot_id in item_lots_enemy:
            # Scan base + sequential sub-lots, STOPPING at the first gap.
            # Without the break, the scan walks across the gap and picks up
            # sub-lots that belong to a *different* NpcParam's chain - e.g.
            # base 337000000 (c3370 NpcParam 33700065) would otherwise bleed
            # into 337000800..337000807 (NpcParam 33700865) and falsely award
            # Shining Horned Headband (lot 337000805) to every c3370 in MSB.
            for offset in range(1000):
                sub_lot = item_lots_enemy.get(lot_id + offset)
                if sub_lot is None:
                    break  # gap in sequence - chain belongs to another NPC
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
        # Scarab / strong-enemy drops (90005300/301) award via AwardItems(Including
        # Clients) with no 2004:76 registration, so the lot's getItemFlagId is never
        # set in-game - the marker would never hide. Use the kill flag X0_4 the
        # template SetEventFlag's on death (captured as defeatFlag) as the hide flag.
        if tr.get('emevdEventId') in (90005300, 90005301) and tr.get('defeatFlag'):
            event_flag = tr['defeatFlag']
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
                    'guaranteed': lot_is_guaranteed(sub_lot),
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
            'guaranteed': lot_is_guaranteed(base_lot),
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
        # Skip lots whose only MSB binding was to unreachable DummyAssets -
        # the engine never spawns them, and the EMEVD-enrich pass would
        # otherwise glue them to a random entity by byte-pattern coincidence.
        if lot_id in unreachable_only_lots:
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

    # Sidecar: NpcParam ID → NpcName FMG ID (only rows with nameId > 0).
    # Consumed by generate_loot_massedit.py + generate_hostile_npcs.py to
    # label drops/markers with named-NPC names (Millicent, Vyke, ...) via
    # the `id + 700000000` NpcName offset convention.
    npcname_path = OUTPUT_DIR / 'npc_name_ids.json'
    with open(npcname_path, 'w', encoding='utf-8') as f:
        json.dump({str(k): v for k, v in npc_to_name_id.items()}, f, indent=2)
    print(f'Saved {len(npc_to_name_id)} NpcParam→NpcName mappings to {npcname_path.name}')

    out_path = OUTPUT_DIR / 'items_database.json'
    with open(out_path, 'w', encoding='utf-8') as f:
        json.dump(database, f, indent=2, ensure_ascii=False)

    # Sidecar: lots that MSB bound only to unreachable DummyAssets.
    # Consumed by enrich_fallback_with_emevd.py to avoid fabricating phantom
    # markers from EMEVD byte-pattern matches.
    unreach_path = OUTPUT_DIR / 'unreachable_msb_lots.json'
    with open(unreach_path, 'w', encoding='utf-8') as f:
        json.dump(sorted(unreachable_only_lots), f)
    print(f'Saved {len(unreachable_only_lots)} unreachable-only lot IDs to {unreach_path.name}')

    elapsed = time.time() - t0
    print(f'\nFinished in {elapsed:.1f}s')
    print(f'Saved {len(database)} records to {out_path}')


if __name__ == '__main__':
    main()
