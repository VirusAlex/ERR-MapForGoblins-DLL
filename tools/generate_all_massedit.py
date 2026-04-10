#!/usr/bin/env python3
"""
Generate MASSEDIT files from items_database.json.
Strategy: preserve existing file structure and text IDs,
update positions/flags, add new items to appropriate files.
"""

import json
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
FMG_DIR = DATA_DIR / 'msg'

# Files that should never be regenerated (non-item or mod-specific)
PRESERVE_FILES = {
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
    'Reforged - items and changes.MASSEDIT',
    'Loot - Material Nodes (DOES NOT DISAPPEAR).MASSEDIT',
}

# IconId by file
FILE_ICON = {
    'Equipment - Armaments.MASSEDIT': 380,
    'Equipment - Armour.MASSEDIT': 381,
    'Equipment - Talismans.MASSEDIT': 382,
    'Equipment - Spirits.MASSEDIT': 383,
    'Equipment - Ashes of War.MASSEDIT': 384,
    'Magic - Sorceries.MASSEDIT': 386,
    'Magic - Incantations.MASSEDIT': 385,
    'Magic - Memory Stones.MASSEDIT': 376,
    'Key - Cookbooks.MASSEDIT': 376,
    'Key - Crystal Tears.MASSEDIT': 392,
    'Key - Celestial Dew.MASSEDIT': 387,
    'Key - Imbued Sword Keys.MASSEDIT': 376,
    'Key - Larval Tears.MASSEDIT': 387,
    'Key - Lost Ashes.MASSEDIT': 396,
    'Key - Pots n Perfumes.MASSEDIT': 376,
    'Key - Seeds,Tears,Scadu,Ashes.MASSEDIT': 377,
    'Key - Whetblades.MASSEDIT': 376,
    'Loot - Ammo.MASSEDIT': 388,
    'Loot - Bell-Bearings.MASSEDIT': 376,
    'Loot - Consumables.MASSEDIT': 387,
    'Loot - Crafting Materials.MASSEDIT': 389,
    'Loot - MP-Fingers, Gestures, Pates.MASSEDIT': 376,
    'Loot - Reusables (Veil,Shackle).MASSEDIT': 376,
    'Loot - Somber_Scarab.MASSEDIT': 378,
    'Loot - Stonesword_Keys.MASSEDIT': 376,
    'Loot - Unique_Drops.MASSEDIT': 387,
}

# Start row IDs by file (matching existing conventions)
FILE_START_ID = {
    'Equipment - Armaments.MASSEDIT': 1,
    'Equipment - Armour.MASSEDIT': 401,
    'Equipment - Ashes of War.MASSEDIT': 701,
    'Equipment - Spirits.MASSEDIT': 1001,
    'Equipment - Talismans.MASSEDIT': 1101,
    'Key - Celestial Dew.MASSEDIT': 2801,
    'Key - Cookbooks.MASSEDIT': 2001,
    'Key - Crystal Tears.MASSEDIT': 2101,
    'Key - Imbued Sword Keys.MASSEDIT': 2201,
    'Key - Larval Tears.MASSEDIT': 2901,
    'Key - Lost Ashes.MASSEDIT': 3001,
    'Key - Pots n Perfumes.MASSEDIT': 3101,
    'Key - Seeds,Tears,Scadu,Ashes.MASSEDIT': 3801,
    'Key - Whetblades.MASSEDIT': 4201,
    'Loot - Ammo.MASSEDIT': 8001,
    'Loot - Bell-Bearings.MASSEDIT': 8501,
    'Loot - Consumables.MASSEDIT': 5101,
    'Loot - Crafting Materials.MASSEDIT': 6501,
    'Loot - MP-Fingers, Gestures, Pates.MASSEDIT': 7301,
    'Loot - Reusables (Veil,Shackle).MASSEDIT': 7501,
    'Loot - Somber_Scarab.MASSEDIT': 9801,
    'Loot - Stonesword_Keys.MASSEDIT': 9001,
    'Loot - Unique_Drops.MASSEDIT': 10401,
    'Magic - Incantations.MASSEDIT': 10801,
    'Magic - Memory Stones.MASSEDIT': 11001,
    'Magic - Sorceries.MASSEDIT': 11401,
}

# Categorization for NEW items (not already assigned to a file)
NEW_ITEM_CATEGORY_MAP = {
    'armament': 'Equipment - Armaments.MASSEDIT',
    'ranged_weapon': 'Equipment - Armaments.MASSEDIT',
    'magic_catalyst': 'Equipment - Armaments.MASSEDIT',
    'shield': 'Equipment - Armaments.MASSEDIT',
    'armour': 'Equipment - Armour.MASSEDIT',
    'talisman': 'Equipment - Talismans.MASSEDIT',
    'spirit_ash': 'Equipment - Spirits.MASSEDIT',
    'ash_of_war': 'Equipment - Ashes of War.MASSEDIT',
    'sorcery': 'Magic - Sorceries.MASSEDIT',
    'incantation': 'Magic - Incantations.MASSEDIT',
    'consumable': 'Loot - Consumables.MASSEDIT',
    'crafting_material': 'Loot - Crafting Materials.MASSEDIT',
    'key_item': 'Key - Seeds,Tears,Scadu,Ashes.MASSEDIT',
}


def parse_massedit_files():
    """Parse existing MASSEDIT files. Returns:
    - flag_to_text: event_flag -> {textId1, textDisableFlagId1, ...}
    - flag_to_file: event_flag -> filename
    - file_entries: filename -> {entry_id -> {fields}}
    """
    pattern = re.compile(r'param WorldMapPointParam: id (\d+): (\w+): = (.+);')
    flag_to_text = {}
    flag_to_file = {}
    file_entries = defaultdict(lambda: defaultdict(dict))

    for fname in sorted(os.listdir(MASSEDIT_DIR)):
        if not fname.endswith('.MASSEDIT'):
            continue

        with open(MASSEDIT_DIR / fname) as f:
            for line in f:
                m = pattern.match(line.strip())
                if not m:
                    continue
                eid = int(m.group(1))
                field = m.group(2)
                val = m.group(3)
                file_entries[fname][eid][field] = val

        for eid, fields in file_entries[fname].items():
            flag = int(fields.get('textDisableFlagId1', 0))
            if flag > 0:
                text_ids = {}
                for i in range(1, 9):
                    tid = int(fields.get(f'textId{i}', 0))
                    tflag = int(fields.get(f'textDisableFlagId{i}', 0))
                    if tid > 0:
                        text_ids[f'textId{i}'] = tid
                    if tflag > 0:
                        text_ids[f'textDisableFlagId{i}'] = tflag
                flag_to_text[flag] = text_ids
                flag_to_file[flag] = fname

    return flag_to_text, flag_to_file, file_entries


def load_fmg_entries():
    fmg_path = FMG_DIR / 'engus' / 'TitleLocations.fmg.json'
    with open(fmg_path, encoding='utf-8') as f:
        data = json.load(f)
    return {e['ID']: e['Text'] for e in data['Fmg']['Entries']}


def build_name_to_fmg(fmg_entries):
    name_to_id = {}
    for fmg_id, text in fmg_entries.items():
        if not text:
            continue
        clean = re.sub(r'^\d+\s+', '', text.strip())
        clean = re.sub(r'^-\s*', '', clean)
        name_to_id[clean.lower()] = fmg_id
    return name_to_id


def find_text_id(item_name, name_to_fmg, fmg_entries, next_id, new_fmg):
    if not item_name:
        return 0, next_id
    key = item_name.lower()
    if key in name_to_fmg:
        return name_to_fmg[key], next_id
    for fmg_id, text in fmg_entries.items():
        if text:
            ct = text.strip().lower()
            if ct in (f'1 {key}', key, f'- {key}'):
                return fmg_id, next_id
    new_fmg[next_id] = item_name
    name_to_fmg[key] = next_id
    return next_id, next_id + 1


def write_massedit_entry(lines, row_id, rec, icon_id):
    area = rec['areaNo']
    gridX = rec['gridX']
    gridZ = rec['gridZ']
    disp = rec['dispMask']
    x = rec['x']
    y = rec['y']
    z = rec['z']
    flag = rec.get('eventFlag', 0)
    text_ids = rec.get('_text_ids', {})
    text_id1 = rec.get('_textId1', 0)

    lines.append(f'param WorldMapPointParam: id {row_id}: iconId: = {icon_id};')
    lines.append(f'param WorldMapPointParam: id {row_id}: {disp}: = 1;')
    lines.append(f'param WorldMapPointParam: id {row_id}: areaNo: = {area};')

    if area in (60, 61) or gridX > 0:
        lines.append(f'param WorldMapPointParam: id {row_id}: gridXNo: = {gridX};')
    if area in (60, 61) or gridZ > 0:
        lines.append(f'param WorldMapPointParam: id {row_id}: gridZNo: = {gridZ};')

    has_pos = (x != 0.0 or z != 0.0)
    if has_pos:
        lines.append(f'param WorldMapPointParam: id {row_id}: posX: = {x:.3f};')
        if y != 0.0:
            lines.append(f'param WorldMapPointParam: id {row_id}: posY: = {y:.3f};')
        lines.append(f'param WorldMapPointParam: id {row_id}: posZ: = {z:.3f};')

    # Text IDs: prefer existing MASSEDIT assignments, fallback to FMG lookup
    has_text = False
    for i in range(1, 9):
        tk = f'textId{i}'
        fk = f'textDisableFlagId{i}'
        if tk in text_ids and text_ids[tk] > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: {tk}: = {text_ids[tk]};')
            has_text = True
        if fk in text_ids and text_ids[fk] > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: {fk}: = {text_ids[fk]};')

    if not has_text and text_id1 > 0:
        lines.append(f'param WorldMapPointParam: id {row_id}: textId1: = {text_id1};')
        if flag > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: textDisableFlagId1: = {flag};')

    lines.append(f'param WorldMapPointParam: id {row_id}: selectMinZoomStep: = 1;')


def main():
    print('=== Loading data ===')

    db_records = json.load(open(DB_PATH, encoding='utf-8'))
    print(f'  {len(db_records)} records in items database')

    flag_to_text, flag_to_file, file_entries = parse_massedit_files()
    print(f'  {len(flag_to_text)} event flag mappings from existing MASSEDIT')

    fmg_entries = load_fmg_entries()
    name_to_fmg = build_name_to_fmg(fmg_entries)
    print(f'  {len(fmg_entries)} FMG entries')

    flag_to_original_id = {}
    for fname, entries in file_entries.items():
        for eid, fields in entries.items():
            eflag = int(fields.get('textDisableFlagId1', 0))
            if eflag > 0:
                flag_to_original_id[eflag] = eid

    print('\n=== Assigning targets ===')
    next_fmg_id = 10600100
    new_fmg_entries = {}

    file_records = defaultdict(list)
    assigned = 0
    new_items = 0

    for rec in db_records:
        flag = rec.get('eventFlag', 0)

        # Try existing MASSEDIT assignment
        if flag > 0 and flag in flag_to_text:
            rec['_text_ids'] = flag_to_text[flag]
            rec['_original_id'] = flag_to_original_id.get(flag, 0)
            target = flag_to_file.get(flag, '')

            if target and target not in PRESERVE_FILES:
                file_records[target].append(rec)
                assigned += 1
                continue

        # New item: assign text and file
        rec['_text_ids'] = {}
        rec['_original_id'] = 0
        item_name = rec['items'][0]['name'] if rec['items'] else ''
        if item_name:
            tid, next_fmg_id = find_text_id(
                item_name, name_to_fmg, fmg_entries, next_fmg_id, new_fmg_entries
            )
            rec['_textId1'] = tid
        else:
            rec['_textId1'] = 0

        target = NEW_ITEM_CATEGORY_MAP.get(rec['primary_category'])
        if target and target not in PRESERVE_FILES:
            file_records[target].append(rec)
            new_items += 1

    print(f'  Existing assignments: {assigned}')
    print(f'  New items assigned: {new_items}')
    print(f'  New FMG entries: {len(new_fmg_entries)}')

    flag_target_file = {}
    for rec in db_records:
        flag = rec.get('eventFlag', 0)
        if flag > 0 and flag in flag_to_file:
            flag_target_file[flag] = flag_to_file[flag]

    print('\n=== Generating MASSEDIT files ===')
    total = 0
    files_written = 0
    preserved_entries_count = 0

    for fname in sorted(set(list(file_records.keys()) + list(FILE_START_ID.keys()))):
        if fname in PRESERVE_FILES:
            continue

        recs = file_records.get(fname, [])

        existing_unmatched = []
        if fname in file_entries:
            recs_flags = {r.get('eventFlag', 0) for r in recs if r.get('eventFlag', 0) > 0}
            for eid, fields in file_entries[fname].items():
                eflag = int(fields.get('textDisableFlagId1', 0))
                if eflag > 0 and eflag in recs_flags:
                    continue
                # Skip if this flag is being regenerated in a different file
                target = flag_target_file.get(eflag)
                if target and target != fname:
                    continue
                existing_unmatched.append((eid, fields))

        if not recs and not existing_unmatched:
            continue

        icon_id = FILE_ICON.get(fname, 387)
        start_id = FILE_START_ID.get(fname, 1)

        lines = []

        for eid, fields in sorted(existing_unmatched, key=lambda x: x[0]):
            for field_name in ['iconId', 'dispMask00', 'dispMask01', 'pad2_0',
                               'areaNo', 'gridXNo', 'gridZNo',
                               'posX', 'posY', 'posZ']:
                if field_name in fields:
                    lines.append(f'param WorldMapPointParam: id {eid}: {field_name}: = {fields[field_name]};')
            for i in range(1, 9):
                for tk in [f'textId{i}', f'textEnableFlagId{i}', f'textDisableFlagId{i}']:
                    if tk in fields:
                        lines.append(f'param WorldMapPointParam: id {eid}: {tk}: = {fields[tk]};')
            if 'selectMinZoomStep' in fields:
                lines.append(f'param WorldMapPointParam: id {eid}: selectMinZoomStep: = {fields["selectMinZoomStep"]};')
            preserved_entries_count += 1

        matched_recs = [r for r in recs if r.get('_original_id', 0) > 0]
        new_recs = [r for r in recs if r.get('_original_id', 0) <= 0]

        matched_recs.sort(key=lambda r: r['_original_id'])
        for rec in matched_recs:
            write_massedit_entry(lines, rec['_original_id'], rec, icon_id)

        # IDs from a high range per file to avoid collisions with existing entries
        new_recs.sort(key=lambda r: (
            r.get('from_fallback', False),
            r['areaNo'], r['gridX'], r['gridZ'],
            r['x'], r['z'],
        ))

        file_idx = sorted(FILE_START_ID.keys()).index(fname) if fname in FILE_START_ID else 0
        new_base = 4000000 + file_idx * 100000
        row_id = new_base
        for rec in new_recs:
            write_massedit_entry(lines, row_id, rec, icon_id)
            row_id += 1

        out_path = MASSEDIT_DIR / fname
        with open(out_path, 'w') as f:
            f.write('\n'.join(lines) + '\n')

        total_in_file = len(existing_unmatched) + len(matched_recs) + len(new_recs)
        total += total_in_file
        files_written += 1
        print(f'  {fname}: {total_in_file} entries (preserved={len(existing_unmatched)}, '
              f'matched={len(matched_recs)}, new={len(new_recs)})')

    if new_fmg_entries:
        print(f'\n=== Saving {len(new_fmg_entries)} new FMG entries ===')
        fmg_out = DATA_DIR / 'new_fmg_entries.json'
        with open(fmg_out, 'w', encoding='utf-8') as f:
            json.dump(
                [{'ID': k, 'Text': v} for k, v in sorted(new_fmg_entries.items())],
                f, indent=2, ensure_ascii=False,
            )

        for locale_dir in sorted(FMG_DIR.iterdir()):
            fmg_path = locale_dir / 'TitleLocations.fmg.json'
            if not fmg_path.exists():
                continue
            with open(fmg_path, encoding='utf-8') as f:
                fmg_data = json.load(f)
            existing_ids = {e['ID'] for e in fmg_data['Fmg']['Entries']}
            added = 0
            for fmg_id, text in sorted(new_fmg_entries.items()):
                if fmg_id not in existing_ids:
                    fmg_data['Fmg']['Entries'].append({'ID': fmg_id, 'Text': text})
                    added += 1
            if added > 0:
                fmg_data['Fmg']['Entries'].sort(key=lambda e: e['ID'])
                with open(fmg_path, 'w', encoding='utf-8') as f:
                    json.dump(fmg_data, f, indent=2, ensure_ascii=False)
                print(f'  {locale_dir.name}: +{added}')

    preserved = 0
    pattern = re.compile(r'param WorldMapPointParam: id (\d+):')
    for fname in PRESERVE_FILES:
        fpath = MASSEDIT_DIR / fname
        if fpath.exists():
            ids = set()
            with open(fpath) as f:
                for line in f:
                    m = pattern.match(line.strip())
                    if m:
                        ids.add(int(m.group(1)))
            preserved += len(ids)

    print(f'\n{"="*60}')
    print('GENERATION SUMMARY')
    print(f'{"="*60}')
    print(f'  Files regenerated: {files_written}')
    print(f'  Generated entries: {total}')
    print(f'  Preserved entries (untouched files): {preserved}')
    print(f'  New FMG text entries: {len(new_fmg_entries)}')
    print(f'\nRun: python tools/generate_data.py && rebuild DLL')


if __name__ == '__main__':
    main()
