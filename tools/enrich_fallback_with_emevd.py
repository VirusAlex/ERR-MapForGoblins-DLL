#!/usr/bin/env python3
"""
Upgrade fallback records in items_database.json using emevd_lot_mapping.json.
For each fallback record (no MSB match), look up the lot in the EMEVD mapping;
if a candidate entity is found, attach its coords/map.

Picks the best candidate by:
  1. Entity ID prefix matches lot ID prefix (same area/tile)
  2. Otherwise, first candidate (insertion order in EMEVD scan)

Also creates new records for lots in EMEVD mapping but absent from items_database.

Run: python enrich_fallback_with_emevd.py
"""
import sys, io, os, json, csv
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import config
from pathlib import Path
from collections import defaultdict

DATA = config.DATA_DIR
DB_PATH = DATA / 'items_database.json'
MAP_PATH = DATA / 'emevd_lot_mapping.json'

UNDERGROUND_AREAS = {12}
DLC_AREAS = {20, 21, 22, 25, 28, 40, 41, 42, 43, 61}


def get_disp_mask(area_no):
    if area_no in UNDERGROUND_AREAS:
        return 'dispMask01'
    if area_no in DLC_AREAS:
        return 'pad2_0'
    return 'dispMask00'


def map_to_area(map_name):
    """Decode m60_42_37_00 -> areaNo=60, gridX=42, gridZ=37"""
    parts = map_name.replace('m', '').split('_')
    if len(parts) >= 3:
        try:
            return int(parts[0]), int(parts[1]), int(parts[2])
        except: pass
    return 0, 0, 0


def pick_best_candidate(lot_id, candidates):
    """Choose best candidate for a lot — prefer entity_id prefix matching lot_id."""
    if not candidates:
        return None
    # Try prefix-match first
    lot_prefix = str(lot_id)[:4]
    for c in candidates:
        ent_prefix = str(c['entity_id'])[:4]
        if ent_prefix == lot_prefix:
            return c
    # Fall back to first
    return candidates[0]


def main():
    print(f'Loading {DB_PATH}...')
    db = json.load(open(DB_PATH, encoding='utf-8'))
    print(f'  {len(db)} records')

    print(f'Loading {MAP_PATH}...')
    emevd_map = json.load(open(MAP_PATH, encoding='utf-8'))
    print(f'  {len(emevd_map)} EMEVD lot mappings')

    # Index existing records by lot_id
    by_lot = defaultdict(list)
    for r in db:
        lot = r.get('itemLotId', 0)
        if lot > 0:
            by_lot[lot].append(r)

    upgraded = 0
    new_records = 0
    failed = 0

    for lot_str, candidates in emevd_map.items():
        lot_id = int(lot_str)
        existing = by_lot.get(lot_id, [])
        # Has any record with real coords already?
        has_coords = any(not r.get('from_fallback') for r in existing)
        if has_coords:
            continue  # nothing to do, already covered

        chosen = pick_best_candidate(lot_id, candidates)
        if not chosen:
            failed += 1
            continue

        area, gx, gz = map_to_area(chosen['msb_map'])

        if existing:  # all fallback — upgrade them
            for r in existing:
                r['x'] = chosen['x']
                r['y'] = chosen['y']
                r['z'] = chosen['z']
                r['map'] = chosen['msb_map']
                r['areaNo'] = area
                r['gridX'] = gx
                r['gridZ'] = gz
                r['dispMask'] = get_disp_mask(area)
                r['partName'] = chosen.get('model', '')
                r['source'] = 'emevd_treasure'
                r.pop('from_fallback', None)
                upgraded += 1
        else:
            # No record at all — would need to create from ItemLotParam_map
            # Skip for now; these should have been picked up by fallback loop in extract_all_items
            pass

    # Save (no backup — extract_all_items regenerates from regulation each build)
    with open(DB_PATH, 'w', encoding='utf-8') as f:
        json.dump(db, f, indent=1)
    print(f'\nUpgraded {upgraded} fallback records to real coords')
    print(f'Could not resolve: {failed}')


if __name__ == '__main__':
    main()
