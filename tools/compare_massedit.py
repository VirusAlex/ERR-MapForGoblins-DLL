#!/usr/bin/env python3
"""
Compare current MASSEDIT files with extracted items_database.json.
Reports: matched, missing from MASSEDIT, obsolete in MASSEDIT.
"""

import json
import math
import os
import re
import sys
import io
from pathlib import Path
from collections import defaultdict

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')

DATA_DIR = Path(__file__).parent.parent / 'data'
MASSEDIT_DIR = DATA_DIR / 'massedit'
DB_PATH = DATA_DIR / 'items_database.json'

NON_ITEM_FILES = {
    'World - Graces.MASSEDIT',
    'World - Imp Statues.MASSEDIT',
    'World - Summoning Pools.MASSEDIT',
    'World - Spirit_Springs.MASSEDIT',
    'World - Spiritspring_Hawks.MASSEDIT',
    'World - Paintings.MASSEDIT',
    'World - Hostile NPC.MASSEDIT',
    'Quest - Progression.MASSEDIT',
    'Quest - Deathroot.MASSEDIT',
    'Quest - Seedbed Curses.MASSEDIT',
    'Reforged - camp contents.MASSEDIT',
    'Reforged - Rune Pieces.MASSEDIT',
    'Reforged - Ember Pieces.MASSEDIT',
    'Loot - Material Nodes (DOES NOT DISAPPEAR).MASSEDIT',
}

MASSEDIT_CATEGORY_MAP = {
    'Equipment - Armaments.MASSEDIT': 'armament',
    'Equipment - Armour.MASSEDIT': 'armour',
    'Equipment - Ashes of War.MASSEDIT': 'ash_of_war',
    'Equipment - Spirits.MASSEDIT': 'spirit_ash',
    'Equipment - Talismans.MASSEDIT': 'talisman',
    'Key - Celestial Dew.MASSEDIT': 'key_item',
    'Key - Cookbooks.MASSEDIT': 'key_item',
    'Key - Crystal Tears.MASSEDIT': 'key_item',
    'Key - Imbued Sword Keys.MASSEDIT': 'key_item',
    'Key - Larval Tears.MASSEDIT': 'key_item',
    'Key - Lost Ashes.MASSEDIT': 'key_item',
    'Key - Pots n Perfumes.MASSEDIT': 'consumable',
    'Key - Seeds,Tears,Scadu,Ashes.MASSEDIT': 'key_item',
    'Key - Whetblades.MASSEDIT': 'key_item',
    'Loot - Ammo.MASSEDIT': 'consumable',
    'Loot - Bell-Bearings.MASSEDIT': 'key_item',
    'Loot - Consumables.MASSEDIT': 'consumable',
    'Loot - Crafting Materials.MASSEDIT': 'crafting_material',
    'Loot - MP-Fingers, Gestures, Pates.MASSEDIT': 'consumable',
    'Loot - Reusables (Veil,Shackle).MASSEDIT': 'key_item',
    'Loot - Somber_Scarab.MASSEDIT': 'consumable',
    'Loot - Stonesword_Keys.MASSEDIT': 'key_item',
    'Loot - Unique_Drops.MASSEDIT': 'key_item',
    'Magic - Incantations.MASSEDIT': 'incantation',
    'Magic - Memory Stones.MASSEDIT': 'key_item',
    'Magic - Sorceries.MASSEDIT': 'sorcery',
    'Reforged - items and changes.MASSEDIT': 'reforged',
}


def parse_massedit_files():
    pattern = re.compile(r'param WorldMapPointParam: id (\d+): (\w+): = (.+);')
    entries = {}  # entry_id -> {fields}
    entry_files = {}  # entry_id -> filename

    for fname in sorted(os.listdir(MASSEDIT_DIR)):
        if not fname.endswith('.MASSEDIT'):
            continue

        filepath = MASSEDIT_DIR / fname
        with open(filepath, 'r') as f:
            for line in f:
                m = pattern.match(line.strip())
                if not m:
                    continue
                entry_id = int(m.group(1))
                field = m.group(2)
                value = m.group(3)

                if entry_id not in entries:
                    entries[entry_id] = {}
                    entry_files[entry_id] = fname

                try:
                    if '.' in value:
                        entries[entry_id][field] = float(value)
                    else:
                        entries[entry_id][field] = int(value)
                except ValueError:
                    entries[entry_id][field] = value

    return entries, entry_files


def match_by_event_flag(massedit_entries, db_records):
    db_by_flag = {}
    for rec in db_records:
        flag = rec.get('eventFlag', 0)
        if flag > 0:
            db_by_flag[flag] = rec

    matches = {}
    for eid, fields in massedit_entries.items():
        flag = fields.get('textDisableFlagId1', 0)
        if flag > 0 and flag in db_by_flag:
            matches[eid] = db_by_flag[flag]

    return matches


def match_by_position(massedit_entries, db_records, matched_eids, matched_db_ids):
    db_by_area = defaultdict(list)
    for i, rec in enumerate(db_records):
        if i in matched_db_ids:
            continue
        db_by_area[rec['areaNo']].append((i, rec))

    pos_matches = {}
    threshold = 5.0

    for eid, fields in massedit_entries.items():
        if eid in matched_eids:
            continue

        area = fields.get('areaNo', -1)
        px = fields.get('posX', None)
        pz = fields.get('posZ', None)
        if px is None or pz is None or area < 0:
            continue

        best_dist = threshold
        best_rec = None
        best_idx = None

        for idx, rec in db_by_area.get(area, []):
            if rec['x'] == 0.0 and rec['z'] == 0.0:
                continue
            dx = px - rec['x']
            dz = pz - rec['z']
            dist = math.sqrt(dx*dx + dz*dz)
            if dist < best_dist:
                best_dist = dist
                best_rec = rec
                best_idx = idx

        if best_rec:
            pos_matches[eid] = best_rec
            matched_db_ids.add(best_idx)

    return pos_matches


def main():
    print('=== Parsing MASSEDIT files ===')
    massedit_entries, entry_files = parse_massedit_files()
    print(f'  {len(massedit_entries)} total MASSEDIT entries from {len(set(entry_files.values()))} files')

    print('\n=== Loading items database ===')
    db_records = json.load(open(DB_PATH, encoding='utf-8'))
    print(f'  {len(db_records)} records in database')

    item_entries = {}
    non_item_entries = {}
    for eid, fields in massedit_entries.items():
        fname = entry_files[eid]
        if fname in NON_ITEM_FILES:
            non_item_entries[eid] = fields
        else:
            item_entries[eid] = fields

    print(f'\n  Item entries (matchable): {len(item_entries)}')
    print(f'  Non-item entries (world features): {len(non_item_entries)}')

    print('\n=== Matching by event flag ===')
    flag_matches = match_by_event_flag(item_entries, db_records)
    print(f'  Matched by event flag: {len(flag_matches)}')

    matched_db_indices = set()
    matched_flags = set()
    for eid, rec in flag_matches.items():
        for i, r in enumerate(db_records):
            if r is rec:
                matched_db_indices.add(i)
                break
        matched_flags.add(rec.get('eventFlag', 0))

    print('\n=== Matching by position ===')
    pos_matches = match_by_position(item_entries, db_records,
                                     set(flag_matches.keys()), matched_db_indices)
    print(f'  Matched by position: {len(pos_matches)}')

    all_matches = {**flag_matches, **pos_matches}
    matched_eids = set(all_matches.keys())

    print('\n' + '='*60)
    print('COMPARISON REPORT')
    print('='*60)

    unmatched_me = {eid: item_entries[eid] for eid in item_entries if eid not in matched_eids}
    print(f'\n--- Unmatched MASSEDIT entries (potentially obsolete): {len(unmatched_me)} ---')

    by_file = defaultdict(list)
    for eid in unmatched_me:
        by_file[entry_files[eid]].append(eid)

    for fname in sorted(by_file):
        eids = sorted(by_file[fname])
        print(f'\n  {fname}: {len(eids)} unmatched')
        for eid in eids[:5]:
            fields = item_entries[eid]
            tid = fields.get('textId1', '?')
            flag = fields.get('textDisableFlagId1', 0)
            x = fields.get('posX', '?')
            z = fields.get('posZ', '?')
            print(f'    id={eid}, textId1={tid}, flag={flag}, pos=({x}, {z})')
        if len(eids) > 5:
            print(f'    ... and {len(eids) - 5} more')

    all_matched_db = set()
    for rec in all_matches.values():
        for i, r in enumerate(db_records):
            if r is rec:
                all_matched_db.add(i)
                break

    unmatched_db = [(i, r) for i, r in enumerate(db_records) if i not in all_matched_db]
    print(f'\n--- Items in game NOT in MASSEDIT: {len(unmatched_db)} ---')

    by_cat = defaultdict(list)
    for i, rec in unmatched_db:
        by_cat[rec['primary_category']].append(rec)

    for cat in sorted(by_cat, key=lambda c: -len(by_cat[c])):
        recs = by_cat[cat]
        print(f'\n  {cat}: {len(recs)} items not in MASSEDIT')
        for rec in recs[:3]:
            names = ', '.join(it['name'] for it in rec['items'] if it['name'])[:60]
            fb = ' [fallback]' if rec.get('from_fallback') else ''
            print(f'    map={rec["map"]}, items=[{names}], flag={rec["eventFlag"]}{fb}')
        if len(recs) > 3:
            print(f'    ... and {len(recs) - 3} more')

    print(f'\n--- Position discrepancies (matched entries) ---')
    pos_diffs = []
    for eid, rec in all_matches.items():
        fields = item_entries[eid]
        if rec['x'] == 0.0 and rec['z'] == 0.0:
            continue
        px = fields.get('posX')
        pz = fields.get('posZ')
        if px is None or pz is None:
            continue
        dx = abs(px - rec['x'])
        dz = abs(pz - rec['z'])
        dist = math.sqrt(dx*dx + dz*dz)
        if dist > 1.0:
            pos_diffs.append((eid, dist, fields, rec))

    pos_diffs.sort(key=lambda x: -x[1])
    print(f'  {len(pos_diffs)} entries with position diff > 1.0')
    for eid, dist, fields, rec in pos_diffs[:10]:
        fname = entry_files[eid]
        names = ', '.join(it['name'] for it in rec['items'] if it['name'])[:40]
        print(f'    id={eid} ({fname}): dist={dist:.1f}, '
              f'ME=({fields.get("posX", "?")}, {fields.get("posZ", "?")}), '
              f'DB=({rec["x"]}, {rec["z"]}), items=[{names}]')

    print(f'\n{"="*60}')
    print('SUMMARY')
    print(f'{"="*60}')
    print(f'  Total MASSEDIT entries: {len(massedit_entries)}')
    print(f'    Item-related: {len(item_entries)}')
    print(f'    Non-item (world features): {len(non_item_entries)}')
    print(f'  Items in game database: {len(db_records)}')
    print(f'  Matched: {len(all_matches)}')
    print(f'    By event flag: {len(flag_matches)}')
    print(f'    By position: {len(pos_matches)}')
    print(f'  Unmatched MASSEDIT (potentially obsolete): {len(unmatched_me)}')
    print(f'  Unmatched DB (missing from MASSEDIT): {len(unmatched_db)}')

    report = {
        'summary': {
            'total_massedit': len(massedit_entries),
            'item_entries': len(item_entries),
            'non_item_entries': len(non_item_entries),
            'db_records': len(db_records),
            'matched_total': len(all_matches),
            'matched_by_flag': len(flag_matches),
            'matched_by_position': len(pos_matches),
            'unmatched_massedit': len(unmatched_me),
            'unmatched_db': len(unmatched_db),
        },
        'unmatched_massedit_by_file': {
            fname: sorted(eids) for fname, eids in by_file.items()
        },
        'unmatched_db_by_category': {
            cat: len(recs) for cat, recs in by_cat.items()
        },
    }

    report_path = DATA_DIR / 'comparison_report.json'
    with open(report_path, 'w', encoding='utf-8') as f:
        json.dump(report, f, indent=2, ensure_ascii=False)
    print(f'\n  Report saved to {report_path}')


if __name__ == '__main__':
    main()
