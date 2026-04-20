#!/usr/bin/env python3
"""
Scan all EMEVD files for item-award references.
For each ItemLotID found in instruction/initializer arg bytes, record
any nearby 4-byte values that match MSB EntityIDs — these are likely
spawn positions for scripted treasure.

Output: data/emevd_lot_mapping.json
  { lot_id (str): [{map, entity_id, spawn_x/y/z, source: 'inst'|'init', event_id}] }
"""
import sys, io, os, tempfile, struct, json
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import config
from pathlib import Path
from collections import defaultdict
from pythonnet import load
load('coreclr')
import clr
clr.AddReference(str(config.SOULSFORMATS_DLL))
from System.Reflection import Assembly, BindingFlags
from System import Array, Type as SysType, Object
from System.IO import File as SysFile
import SoulsFormats

asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
_str_type = SysType.GetType('System.String')
_emevd_read = asm.GetType('SoulsFormats.EMEVD').GetMethod('Read',
    BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str_type]), None)
_param_read = asm.GetType('SoulsFormats.PARAM').GetMethod('Read',
    BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str_type]), None)


def load_emevd(path):
    data = SoulsFormats.DCX.Decompress(str(path)).ToArray()
    tmp = os.path.join(tempfile.gettempdir(), '_mfg_emevd.tmp')
    SysFile.WriteAllBytes(tmp, data)
    e = _emevd_read.Invoke(None, Array[Object]([tmp]))
    os.unlink(tmp)
    return e


# ── Load lot IDs from regulation.bin ──
def load_lot_ids():
    """Return (map_lots, enemy_lots) sets of ItemLotParam IDs."""
    reg_path = config.require_err_mod_dir() / 'regulation.bin'
    bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(reg_path))
    map_lots = set()
    enemy_lots = set()
    for f in bnd.Files:
        fn = str(f.Name)
        if 'ItemLotParam_map' in fn:
            tmp = os.path.join(tempfile.gettempdir(), '_mfg_p.tmp')
            # f.Bytes is Memory<byte>; need ToArray()
            arr = f.Bytes.ToArray() if hasattr(f.Bytes, 'ToArray') else f.Bytes
            SysFile.WriteAllBytes(tmp, arr)
            p = _param_read.Invoke(None, Array[Object]([tmp]))
            os.unlink(tmp)
            for row in p.Rows:
                map_lots.add(int(row.ID))
        elif 'ItemLotParam_enemy' in fn:
            tmp = os.path.join(tempfile.gettempdir(), '_mfg_p.tmp')
            # f.Bytes is Memory<byte>; need ToArray()
            arr = f.Bytes.ToArray() if hasattr(f.Bytes, 'ToArray') else f.Bytes
            SysFile.WriteAllBytes(tmp, arr)
            p = _param_read.Invoke(None, Array[Object]([tmp]))
            os.unlink(tmp)
            for row in p.Rows:
                enemy_lots.add(int(row.ID))
    return map_lots, enemy_lots


def scan_arg_blob(arg_bytes, lot_set, entity_set):
    """Find all (lot_id, entity_id) co-occurrences in one arg blob.

    Returns list of (lot_id, [entity_ids_in_blob]).
    """
    vals = []
    # Scan every 4-byte aligned (step by 4 to match common arg layout)
    # But also try unaligned in case args have different offset
    for i in range(0, len(arg_bytes) - 3):
        vals.append((i, struct.unpack_from('<I', arg_bytes, i)[0]))
    lots = [v for i, v in vals if v in lot_set]
    ents = [v for i, v in vals if v in entity_set]
    return lots, ents


def main():
    print('Loading MSB entity index...')
    ei_path = config.DATA_DIR / 'msb_entity_index.json'
    entity_idx = json.load(open(ei_path, encoding='utf-8'))
    entity_set = set(int(k) for k in entity_idx.keys())
    print(f'  {len(entity_set)} entities')

    print('Loading ItemLot IDs from regulation...')
    map_lots, enemy_lots = load_lot_ids()
    lot_set = map_lots | enemy_lots
    print(f'  {len(map_lots)} map lots, {len(enemy_lots)} enemy lots, {len(lot_set)} total')

    event_dir = config.require_err_mod_dir() / 'event'
    emevd_files = sorted(event_dir.glob('*.emevd.dcx'))
    print(f'Scanning {len(emevd_files)} EMEVDs...')

    # lot -> list of candidate mappings
    mapping = defaultdict(list)

    for idx, p in enumerate(emevd_files):
        if (idx + 1) % 100 == 0:
            print(f'  [{idx+1}/{len(emevd_files)}]')
        try:
            emevd = load_emevd(p)
        except Exception as e:
            continue

        map_name = p.name.replace('.emevd.dcx', '')

        # Collect all arg blobs from every instruction in every event
        for evt in emevd.Events:
            event_id = int(evt.ID)
            for inst in evt.Instructions:
                ab = bytes(inst.ArgData) if inst.ArgData else b''
                if not ab: continue
                lots, ents = scan_arg_blob(ab, lot_set, entity_set)
                # Filter trivial lot IDs (0-9999) that collide with common small integers
                lots = [l for l in lots if l >= 10000]
                if lots and ents:
                    for lot in lots:
                        for ent in ents:
                            mapping[lot].append({
                                'map_emevd': map_name,
                                'event_id': event_id,
                                'entity_id': ent,
                                'bank': int(inst.Bank),
                                'idx': int(inst.ID),
                            })

    print(f'\nFound {len(mapping)} lots with entity candidates')
    print(f'Total (lot, entity) records: {sum(len(v) for v in mapping.values())}')

    # Attach entity positions, filter noise
    enriched = {}
    for lot, recs in mapping.items():
        # Deduplicate by entity_id
        seen = set()
        uniq = []
        for r in recs:
            if r['entity_id'] in seen: continue
            seen.add(r['entity_id'])
            # Skip trivial IDs (likely counts/flags mistaken for entities)
            if r['entity_id'] < 10000: continue
            ent_info = entity_idx.get(str(r['entity_id']))
            if not ent_info: continue
            # Require MSB map prefix matches EMEVD map (same tile) to reduce cross-map collisions
            if ent_info['map'] != r['map_emevd']:
                continue
            uniq.append({**r, **{
                'msb_map': ent_info['map'],
                'x': ent_info['x'], 'y': ent_info['y'], 'z': ent_info['z'],
                'model': ent_info['model'],
                'kind': ent_info['kind'],
            }})
        if uniq:
            enriched[str(lot)] = uniq

    out_path = config.DATA_DIR / 'emevd_lot_mapping.json'
    with open(out_path, 'w', encoding='utf-8') as f:
        json.dump(enriched, f, indent=1)
    print(f'Saved {len(enriched)} resolved lots to {out_path}')


if __name__ == '__main__':
    main()
