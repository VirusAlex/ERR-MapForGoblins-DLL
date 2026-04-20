#!/usr/bin/env python3
"""
Analyze ALL template events in the 90005xxx range (refined).
Filter out false positives: ignore lot IDs < 1000, require >25% hit rate.
For real lot offsets, show actual lot samples that resolve to items.
"""

import struct
import sys
import io
import os
import tempfile
import time
from pathlib import Path
from collections import defaultdict

import config

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')

ERR_MOD_DIR = config.require_err_mod_dir()
PARAMDEF_DIR = config.PARAMDEF_DIR

# .NET init
from pythonnet import load
load('coreclr')
import clr
from System.Reflection import Assembly, BindingFlags
from System import Array, Type as SysType, Object
from System.IO import File as SysFile

asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
clr.AddReference(str(config.SOULSFORMATS_DLL))
import SoulsFormats

_str_type = SysType.GetType('System.String')

def _get_read_str(type_name):
    cls = asm.GetType(type_name)
    return cls.GetMethod('Read',
        BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
        None, Array[SysType]([_str_type]), None)

_param_read = _get_read_str('SoulsFormats.PARAM')
_emevd_read = asm.GetType('SoulsFormats.EMEVD').GetMethod(
    'Read', BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str_type]), None)


def _read_from_bytes(read_method, data, suffix='.bin'):
    tmp = os.path.join(tempfile.gettempdir(), f'_mfg_tmp{suffix}')
    if hasattr(data, 'ToArray'):
        SysFile.WriteAllBytes(tmp, data.ToArray())
    else:
        SysFile.WriteAllBytes(tmp, data)
    result = read_method.Invoke(None, Array[Object]([tmp]))
    os.unlink(tmp)
    return result


from extract_all_items import load_paramdefs, read_param, param_to_dict

# Minimum lot ID to consider real (filter out 0, 1, 2, 10, 20, etc.)
MIN_LOT_ID = 1000
# Minimum fraction of calls that must match for an offset to count
MIN_HIT_RATE = 0.25


def main():
    t0 = time.time()

    print('=== Loading regulation.bin ===')
    reg_path = ERR_MOD_DIR / 'regulation.bin'
    bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(reg_path))

    paramdefs = load_paramdefs()

    # Build lot ID sets
    ilp_map = read_param(bnd, 'ItemLotParam_map', paramdefs)
    lot_ids_map = set()
    for row in ilp_map.Rows:
        lot_ids_map.add(int(row.ID))

    ilp_enemy = read_param(bnd, 'ItemLotParam_enemy', paramdefs)
    lot_ids_enemy = set()
    for row in ilp_enemy.Rows:
        lot_ids_enemy.add(int(row.ID))

    # "Real" lot IDs (>= MIN_LOT_ID)
    real_lots_map = {x for x in lot_ids_map if x >= MIN_LOT_ID}
    real_lots_enemy = {x for x in lot_ids_enemy if x >= MIN_LOT_ID}
    real_lots_all = real_lots_map | real_lots_enemy
    print(f'  {len(real_lots_map)} map lots, {len(real_lots_enemy)} enemy lots (>= {MIN_LOT_ID})')

    # Also build item lookup for showing what items a lot contains
    lot_fields = set()
    for i in range(1, 9):
        lot_fields.update([f'lotItemId0{i}', f'lotItemCategory0{i}'])
    lot_fields.add('getItemFlagId')
    item_lots_map = param_to_dict(ilp_map, lot_fields)
    item_lots_enemy = param_to_dict(ilp_enemy, lot_fields)

    # Load item names
    MSGBND_PATH = ERR_MOD_DIR / 'msg' / 'engus' / 'item_dlc02.msgbnd.dcx'
    _bnd4_read = _get_read_str('SoulsFormats.BND4')
    _fmg_read = _get_read_str('SoulsFormats.FMG')

    from extract_all_items import read_fmg_names
    msgbnd = _read_from_bytes(_bnd4_read, SoulsFormats.DCX.Decompress(str(MSGBND_PATH)), '.bnd')
    weapon_names = read_fmg_names(msgbnd, 'WeaponName.fmg')
    goods_names = read_fmg_names(msgbnd, 'GoodsName.fmg')
    armor_names = read_fmg_names(msgbnd, 'ProtectorName.fmg')
    talisman_names = read_fmg_names(msgbnd, 'AccessoryName.fmg')
    gem_names = read_fmg_names(msgbnd, 'GemName.fmg')
    name_dbs = {1: goods_names, 2: weapon_names, 3: armor_names, 4: talisman_names, 5: gem_names}

    def lot_to_items_str(lot_id):
        """Get a short description of items in a lot."""
        lot = item_lots_map.get(lot_id) or item_lots_enemy.get(lot_id)
        if not lot:
            return '???'
        items = []
        for i in range(1, 9):
            iid = lot.get(f'lotItemId0{i}', 0)
            cat = lot.get(f'lotItemCategory0{i}', 0)
            if iid <= 0 or cat <= 0:
                continue
            name = name_dbs.get(cat, {}).get(iid, f'cat{cat}:{iid}')
            items.append(name)
        return ', '.join(items) if items else '(empty lot)'

    # Scan EMEVD
    print('\n=== Scanning EMEVD files ===')
    emevd_dir = ERR_MOD_DIR / 'event'
    emevd_files = sorted(emevd_dir.glob('*.emevd.dcx'))

    event_calls = defaultdict(list)  # event_id -> [(args_bytes, map_name)]

    for emevd_path in emevd_files:
        map_name = emevd_path.name.replace('.emevd.dcx', '')
        try:
            tmp2 = os.path.join(tempfile.gettempdir(), '_mfg_emevd_analyze.tmp')
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
                if 90005000 <= event_id <= 90006000:
                    event_calls[event_id].append((args, map_name))

    total_calls = sum(len(v) for v in event_calls.values())
    print(f'  {len(event_calls)} unique events, {total_calls} total calls')

    # Analyze
    KNOWN_EVENTS = {90005200, 90005210, 90005300, 90005301, 90005860}

    results_with_lots = []
    results_without_lots = []

    for event_id in sorted(event_calls.keys()):
        calls = event_calls[event_id]
        call_count = len(calls)
        max_len = max(len(a) for a, _ in calls)

        lot_offsets = {}

        for offset in range(8, max_len, 4):
            map_hits = set()
            enemy_hits = set()
            hit_count = 0
            total_checked = 0

            for args, map_name in calls:
                if len(args) < offset + 4:
                    continue
                total_checked += 1
                val = struct.unpack_from('<i', args, offset)[0]
                if val < MIN_LOT_ID:
                    continue
                if val in real_lots_map:
                    map_hits.add(val)
                    hit_count += 1
                elif val in real_lots_enemy:
                    enemy_hits.add(val)
                    hit_count += 1

            if total_checked == 0:
                continue

            hit_rate = hit_count / total_checked

            if (map_hits or enemy_hits) and hit_rate >= MIN_HIT_RATE:
                lot_offsets[offset] = {
                    'map': map_hits,
                    'enemy': enemy_hits,
                    'hit_count': hit_count,
                    'total_checked': total_checked,
                    'hit_rate': hit_rate,
                }

        if lot_offsets:
            results_with_lots.append((event_id, call_count, lot_offsets))
        else:
            results_without_lots.append((event_id, call_count))

    results_with_lots.sort(key=lambda x: -x[1])

    print('\n' + '=' * 90)
    print('EVENTS WITH REAL LOT IDS (>= 1000, >= 25% hit rate)')
    print('=' * 90)

    for event_id, call_count, lot_offsets in results_with_lots:
        known_tag = ' [KNOWN]' if event_id in KNOWN_EVENTS else ' [NEW!]'
        print(f'\nEVENT {event_id} ({call_count} calls){known_tag}:')

        for offset in sorted(lot_offsets.keys()):
            info = lot_offsets[offset]
            all_lots = sorted(info['map'] | info['enemy'])
            sources = []
            if info['map']:
                sources.append(f'{len(info["map"])} in map')
            if info['enemy']:
                sources.append(f'{len(info["enemy"])} in enemy')

            print(f'  offset {offset:2d} (arg[{offset//4}]): '
                  f'{info["hit_count"]}/{info["total_checked"]} calls ({info["hit_rate"]*100:.0f}%) | '
                  f'{", ".join(sources)}')

            # Show up to 5 sample lots with their items
            for lot_id in all_lots[:5]:
                items_str = lot_to_items_str(lot_id)
                src = 'map' if lot_id in info['map'] else 'enemy'
                print(f'    {lot_id} ({src}): {items_str}')
            if len(all_lots) > 5:
                print(f'    ... and {len(all_lots) - 5} more lot IDs')

    # Summary of new events that actually award items
    print('\n' + '=' * 90)
    print('SUMMARY: NEW events with real lot IDs')
    print('=' * 90)
    for event_id, call_count, lot_offsets in results_with_lots:
        if event_id in KNOWN_EVENTS:
            continue
        # Find the best lot offset (highest hit rate)
        best_off = max(lot_offsets.keys(), key=lambda o: lot_offsets[o]['hit_rate'])
        info = lot_offsets[best_off]
        total_unique = len(info['map']) + len(info['enemy'])
        print(f'  {event_id}: {call_count} calls, lot at offset {best_off} '
              f'({info["hit_rate"]*100:.0f}% hit, {total_unique} unique lots)')

    elapsed = time.time() - t0
    print(f'\nFinished in {elapsed:.1f}s')


if __name__ == '__main__':
    main()
