#!/usr/bin/env python3
"""
Scan ALL tile EMEVDs for gathering-node collection events.
Extract mapping (tile, entity_id) -> passed_flag_id.

ERR-specific pattern:
- Event 0 of each tile calls a "gathering-node handler" common event
  via RunEvent(bank=2000, id=0). The event ID is usually like XX002250
  (tile-local, ending in 250) but that varies.
- RunEvent's ArgData carries (index, event_id, flag_id, 30000, 20000, 16572, ...,
  entity_id, ..., flag_id). EntityID matches MSB Part's AEG099_XX0_XXXX asset.

Output: data/gathering_node_flags.json
  {
    "m32_00_00_00": {
      "32001201": 32000201,   # EntityID -> passed flag
      ...
    },
    ...
  }
"""
import sys, io, os, tempfile, struct, json, glob, re
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
import config

from pythonnet import load; load('coreclr')
import clr
clr.AddReference(str(config.SOULSFORMATS_DLL))
from System.Reflection import Assembly, BindingFlags
from System import Array, Type as SysType, Object
from System.IO import File as SysFile
import SoulsFormats


def _safe_unlink(path):
    try:
        os.unlink(path)
    except PermissionError:
        pass


asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
_str_type = SysType.GetType('System.String')
_emevd_read = asm.GetType('SoulsFormats.EMEVD').GetMethod('Read',
    BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str_type]), None)


def load_emevd(path):
    data = SoulsFormats.DCX.Decompress(str(path)).ToArray()
    tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_mfg_emevd_gn.tmp')
    SysFile.WriteAllBytes(tmp, data)
    e = _emevd_read.Invoke(None, Array[Object]([tmp]))
    _safe_unlink(tmp)
    return e


def extract_gathering_mapping(emevd, tile_name):
    """For Event 0's RunEvent calls, find (entity_id, passed_flag) pairs.

    ERR's gathering-node handler calls have a recognisable shape: the RunEvent
    ArgData starts with [index, event_id_called, passed_flag, <small animation
    params>, ..., entity_id, ..., passed_flag_again]. The passed_flag and
    entity_id share the high u32 bytes (area identifier) - that's the strongest
    signal we rely on.
    """
    result = {}
    area_prefix = None
    # Tile name like "m32_00_00_00" → area = 32
    try:
        area = int(tile_name[1:3])
    except:
        area = None

    for ev in emevd.Events:
        if int(ev.ID) != 0: continue  # Initializer
        for ins in ev.Instructions:
            try:
                bank = int(ins.Bank); iid = int(ins.ID)
                if bank != 2000 or iid != 0:
                    continue
                ab = bytes(ins.ArgData) if ins.ArgData else b''
            except: continue
            if len(ab) < 48:
                continue

            vals = [struct.unpack_from('<I', ab, i)[0] for i in range(0, len(ab) - 3, 4)]
            if len(vals) < 12:
                continue

            # Pattern detection: find any pair where one value looks like an entity ID
            # (matches area*10**6 + ... range) and another looks like a passed_flag
            # that the same handler uses. We cross-check by requiring that the
            # SAME flag appears twice in the ArgData (start and end) - that's a
            # distinctive fingerprint of the ERR gathering-node handler layout.
            from collections import Counter
            c = Counter(vals)
            dup_flags = [v for v, cnt in c.items() if cnt >= 2 and 1_000_000 < v < 0x80000000]
            if not dup_flags:
                continue

            # Also find exactly one entity-ID-shaped value (sharing area with flag)
            for passed_flag in dup_flags:
                # Entity_id usually sits alongside the flag, sharing the high u32
                # bytes. Exclude the called event id (vals[1]) up-front - it also
                # lives in the same area group.
                high = passed_flag // 1_000_000
                event_called = vals[1]
                candidates = [v for v in vals
                              if v != passed_flag
                              and v != event_called
                              and v // 1_000_000 == high
                              and c[v] == 1
                              and 1_000_000 < v < 0x80000000]
                if len(candidates) != 1:
                    continue
                entity_id = candidates[0]
                result[str(entity_id)] = passed_flag
                break  # one pair per instruction
    return result


def extract_scene_mapping(emevd):
    """Scan common.emevd for ERR NPC-scene handler calls.

    ERR wraps certain AEG463_840/AEG463_600 flower-and-body scenes in a
    central dispatcher (event 1043612000) that calls handler events
    1043613110 / 1043603110 via RunEvent(bank=2000, id=0). Each call
    carries the completion flag as X5 (vals[5]) and the MSB name_suffix
    of the scene asset as X7 (vals[9]). The dispatcher short-circuits
    the RunEvent if CheckEventFlag(X5) is already set - meaning X5 is
    the "scene already played / item already looted" flag we want.

    Same (suffix, X5) pair is effectively global, so we return a
    model-suffix-keyed mapping.
    """
    SCENE_HANDLERS = {1043613110, 1043603110}
    by_suffix = {}
    for ev in emevd.Events:
        for ins in ev.Instructions:
            try:
                bank = int(ins.Bank); iid = int(ins.ID)
                if bank != 2000 or iid != 0: continue
                ab = bytes(ins.ArgData) if ins.ArgData else b''
            except: continue
            if len(ab) < 40: continue
            vals = [struct.unpack_from('<I', ab, j)[0] for j in range(0, len(ab) - 3, 4)]
            if len(vals) < 10: continue
            called = vals[1]
            if called not in SCENE_HANDLERS: continue
            flag = vals[5]   # X5 - completion gate checked by dispatcher
            suffix = vals[9] # X7 - MSB name_suffix
            if flag < 1_000_000 or flag >= 0x80000000: continue
            if not (9000 <= suffix <= 9999): continue
            by_suffix[suffix] = flag
    return by_suffix


def main():
    event_dir = config.require_err_mod_dir() / 'event'
    pattern = re.compile(r'^m(\d+)_(\d+)_(\d+)_(\d+)\.emevd\.dcx$')

    by_tile_entity = {}
    total_pairs = 0
    files_scanned = 0
    files_with_nodes = 0

    emevds = sorted(event_dir.glob('m*_*_*_*.emevd.dcx'))
    print(f'Scanning {len(emevds)} per-tile EMEVDs...')

    for fpath in emevds:
        m = pattern.match(fpath.name)
        if not m: continue
        tile_name = fpath.name.replace('.emevd.dcx', '')
        try:
            emevd = load_emevd(fpath)
        except Exception as e:
            print(f'  {tile_name}: load error: {e}')
            continue
        files_scanned += 1
        mapping = extract_gathering_mapping(emevd, tile_name)
        if mapping:
            by_tile_entity[tile_name] = mapping
            total_pairs += len(mapping)
            files_with_nodes += 1
        if files_scanned % 50 == 0:
            print(f'  ...scanned {files_scanned} tiles, found pairs in {files_with_nodes}')

    # Scan common.emevd for ERR NPC-scene handler pattern
    print('\nScanning common.emevd for ERR NPC-scene handlers...')
    by_name_suffix = {}
    common_path = event_dir / 'common.emevd.dcx'
    if common_path.exists():
        try:
            ce = load_emevd(common_path)
            scene_mapping = extract_scene_mapping(ce)
            # Key by MSB name pattern - only AEG463_840 (flower) and AEG463_600 (body)
            # participate in these scenes. Applied globally across tiles.
            for suffix, flag in scene_mapping.items():
                for model in ('AEG463_840', 'AEG463_600'):
                    by_name_suffix[f'{model}_{suffix}'] = flag
            print(f'  Found {len(scene_mapping)} (suffix, flag) pairs '
                  f'-> {len(by_name_suffix)} (name, flag) entries')
        except Exception as e:
            print(f'  common.emevd: load error: {e}')

    out_path = config.DATA_DIR / 'gathering_node_flags.json'
    out = {
        'by_tile_entity': by_tile_entity,
        'by_name_suffix': by_name_suffix,
    }
    with open(out_path, 'w') as f:
        json.dump(out, f, indent=2, sort_keys=True)
    print(f'\nTotal: {total_pairs} (entity_id, flag) pairs across {files_with_nodes} tiles')
    print(f'       {len(by_name_suffix)} ERR-scene (name, flag) pairs')
    print(f'Saved to {out_path}')

    sample = by_tile_entity.get('m32_00_00_00', {})
    if sample:
        print(f'\nSample m32_00_00_00 (per-tile entity):')
        for eid, flag in sorted(sample.items()):
            print(f'  entity {eid} -> flag {flag}')
    if by_name_suffix:
        print(f'\nERR-scene mapping (by name):')
        for name, flag in sorted(by_name_suffix.items()):
            print(f'  {name} -> flag {flag}')


if __name__ == '__main__':
    main()
