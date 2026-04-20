#!/usr/bin/env python3
"""
Analyze ALL template events in the 90005xxx range.
For each event ID, sample calls, extract int32 args, check if any arg
value exists as a lot ID in ItemLotParam_map or ItemLotParam_enemy.
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


# Import helpers from extract_all_items
from extract_all_items import load_paramdefs, read_param, param_to_dict


def main():
    t0 = time.time()

    # ── Step 1: Load regulation.bin, build lot ID sets ──
    print('=== Loading regulation.bin ===')
    reg_path = ERR_MOD_DIR / 'regulation.bin'
    bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(reg_path))
    print(f'  {bnd.Files.Count} files in regulation')

    paramdefs = load_paramdefs()
    print(f'  {len(paramdefs)} paramdefs loaded')

    print('--- ItemLotParam_map ---')
    ilp_map = read_param(bnd, 'ItemLotParam_map', paramdefs)
    lot_ids_map = set()
    for row in ilp_map.Rows:
        lot_ids_map.add(int(row.ID))
    print(f'  {len(lot_ids_map)} lot IDs')

    print('--- ItemLotParam_enemy ---')
    ilp_enemy = read_param(bnd, 'ItemLotParam_enemy', paramdefs)
    lot_ids_enemy = set()
    for row in ilp_enemy.Rows:
        lot_ids_enemy.add(int(row.ID))
    print(f'  {len(lot_ids_enemy)} lot IDs')

    all_lot_ids = lot_ids_map | lot_ids_enemy
    print(f'  Combined: {len(all_lot_ids)} unique lot IDs')

    # ── Step 2: Scan all EMEVD files ──
    print('\n=== Scanning EMEVD files ===')
    emevd_dir = ERR_MOD_DIR / 'event'
    emevd_files = sorted(emevd_dir.glob('*.emevd.dcx'))
    print(f'  {len(emevd_files)} EMEVD files')

    # event_id -> list of (args_bytes, map_name)
    event_calls = defaultdict(list)

    for emevd_path in emevd_files:
        map_name = emevd_path.name.replace('.emevd.dcx', '')
        try:
            tmp2 = os.path.join(tempfile.gettempdir(), '_mfg_emevd_analyze.tmp')
            SysFile.WriteAllBytes(tmp2, SoulsFormats.DCX.Decompress(str(emevd_path)).ToArray())
            emevd = _emevd_read.Invoke(None, Array[Object]([tmp2]))
            os.unlink(tmp2)
        except Exception as e:
            print(f'  ERROR reading {emevd_path.name}: {e}')
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

    print(f'  Found {len(event_calls)} unique event IDs in range 90005000-90006000')
    total_calls = sum(len(v) for v in event_calls.values())
    print(f'  Total calls: {total_calls}')

    # ── Step 3: Analyze each event ──
    print('\n=== Analyzing events ===\n')

    # For each event, check each arg offset for lot IDs
    results_with_lots = []
    results_without_lots = []

    for event_id in sorted(event_calls.keys()):
        calls = event_calls[event_id]
        call_count = len(calls)

        # Determine max args length across all calls
        max_len = max(len(a) for a, _ in calls)

        # Check each int32 offset (skip 0=slot, 4=event_id)
        lot_offsets = {}  # offset -> {'map': set, 'enemy': set, 'both': set, 'samples': list}

        for offset in range(8, max_len, 4):
            map_hits = set()
            enemy_hits = set()
            all_values = []

            for args, map_name in calls:
                if len(args) < offset + 4:
                    continue
                val = struct.unpack_from('<i', args, offset)[0]
                all_values.append(val)
                if val in lot_ids_map:
                    map_hits.add(val)
                if val in lot_ids_enemy:
                    enemy_hits.add(val)

            if map_hits or enemy_hits:
                # Calculate what fraction of calls have a lot at this offset
                hit_count = 0
                for args, _ in calls:
                    if len(args) < offset + 4:
                        continue
                    val = struct.unpack_from('<i', args, offset)[0]
                    if val in all_lot_ids:
                        hit_count += 1

                lot_offsets[offset] = {
                    'map': map_hits,
                    'enemy': enemy_hits,
                    'hit_count': hit_count,
                    'total_checked': len([a for a, _ in calls if len(a) >= offset + 4]),
                    'samples': sorted(map_hits | enemy_hits)[:10],
                }

        if lot_offsets:
            results_with_lots.append((event_id, call_count, lot_offsets))
        else:
            results_without_lots.append((event_id, call_count))

    # ── Step 4: Report ──
    # Sort by call count descending
    results_with_lots.sort(key=lambda x: -x[1])
    results_without_lots.sort(key=lambda x: -x[1])

    KNOWN_EVENTS = {90005200, 90005210, 90005300, 90005301, 90005860}

    print('=' * 80)
    print('EVENTS WITH LOT IDS FOUND')
    print('=' * 80)
    for event_id, call_count, lot_offsets in results_with_lots:
        known_tag = ' [KNOWN]' if event_id in KNOWN_EVENTS else ' [NEW!]'
        print(f'\nEVENT {event_id} ({call_count} calls){known_tag}:')
        for offset in sorted(lot_offsets.keys()):
            info = lot_offsets[offset]
            sources = []
            if info['map']:
                sources.append(f'{len(info["map"])} in map')
            if info['enemy']:
                sources.append(f'{len(info["enemy"])} in enemy')
            hit_pct = info['hit_count'] * 100 // info['total_checked'] if info['total_checked'] > 0 else 0
            print(f'  offset {offset:2d} (arg[{offset//4}]): {info["hit_count"]}/{info["total_checked"]} calls match ({hit_pct}%) | {", ".join(sources)}')
            print(f'    samples: {info["samples"][:8]}')

    print('\n' + '=' * 80)
    print('EVENTS WITH NO LOT IDS (sorted by call count)')
    print('=' * 80)
    for event_id, call_count in results_without_lots:
        # Show first call's args for context
        sample_args = event_calls[event_id][0][0]
        arg_vals = []
        for off in range(8, len(sample_args), 4):
            if off + 4 <= len(sample_args):
                arg_vals.append(struct.unpack_from('<i', sample_args, off)[0])
        print(f'EVENT {event_id} ({call_count} calls): NO LOTS FOUND | sample args: {arg_vals[:8]}')

    # ── Summary ──
    print('\n' + '=' * 80)
    print('SUMMARY')
    print('=' * 80)
    print(f'Events with lots: {len(results_with_lots)}')
    print(f'Events without lots: {len(results_without_lots)}')
    new_events = [e for e, _, _ in results_with_lots if e not in KNOWN_EVENTS]
    if new_events:
        print(f'\nNEW events with lot IDs: {new_events}')
    else:
        print('\nNo new events with lot IDs found.')

    elapsed = time.time() - t0
    print(f'\nFinished in {elapsed:.1f}s')


if __name__ == '__main__':
    main()
