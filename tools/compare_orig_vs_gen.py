"""Compare original vs generated MASSEDIT files and classify differences."""
import re
import json
import os
import sys
import io
from collections import defaultdict
from pathlib import Path

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')

BASE = Path(__file__).parent.parent / 'data'
ORIG_DIR = BASE / 'massedit'
GEN_DIR = BASE / 'massedit_generated'
DB_PATH = BASE / 'items_database.json'

PAIRS = [
    "Equipment - Armaments",
    "Equipment - Armour",
    "Equipment - Ashes of War",
    "Equipment - Spirits",
    "Equipment - Talismans",
    "Magic - Incantations",
    "Magic - Sorceries",
]

# DLC maps
DLC_MAP_PREFIXES = ('m20', 'm21', 'm22', 'm25')

def parse_massedit(filepath):
    """Parse MASSEDIT file, return dict of {entry_id: {field: value}}."""
    entries = defaultdict(dict)
    pattern = re.compile(r'param WorldMapPointParam: id (\d+): (\w+): = (.+);')
    with open(filepath, 'r', encoding='utf-8') as f:
        for line in f:
            m = pattern.match(line.strip())
            if m:
                entry_id = int(m.group(1))
                field = m.group(2)
                value = m.group(3)
                entries[entry_id][field] = value
    return dict(entries)

def load_items_db(path):
    """Load items database and build eventFlag -> item info mapping."""
    print("Loading items database...")
    with open(path, 'r', encoding='utf-8') as f:
        data = json.load(f)

    flag_to_info = {}
    for entry in data:
        flag = entry.get('eventFlag')
        if flag:
            flag_to_info[str(flag)] = entry

    print(f"  Loaded {len(data)} entries, {len(flag_to_info)} with event flags")
    return flag_to_info

def get_item_names(info):
    """Get item names from database entry."""
    if not info:
        return "?"
    items = info.get('items', [])
    names = [item.get('name', '?') for item in items]
    return ", ".join(names) if names else "?"

def classify_entry(info):
    """Return a short classification string."""
    if not info:
        return "NOT IN DB"

    map_id = info.get('map', '')
    source = info.get('source', '')
    is_dlc = any(map_id.startswith(p) for p in DLC_MAP_PREFIXES)

    region = "DLC" if is_dlc else "Base"
    return f"{region} / {source}"

def compare_pair(name, flag_db):
    """Compare one pair of MASSEDIT files."""
    orig_path = ORIG_DIR / f"{name}.MASSEDIT"
    gen_path = GEN_DIR / f"{name}.MASSEDIT"

    if not orig_path.exists():
        print(f"  ORIG NOT FOUND: {orig_path}")
        return None
    if not gen_path.exists():
        print(f"  GEN NOT FOUND: {gen_path}")
        return None

    orig_entries = parse_massedit(orig_path)
    gen_entries = parse_massedit(gen_path)

    # Extract textDisableFlagId1 for each entry
    def get_flags(entries):
        flag_map = {}  # flag -> entry_id
        for eid, fields in entries.items():
            flag = fields.get('textDisableFlagId1')
            if flag:
                flag_map[flag] = eid
        return flag_map

    orig_flags = get_flags(orig_entries)
    gen_flags = get_flags(gen_entries)

    orig_flag_set = set(orig_flags.keys())
    gen_flag_set = set(gen_flags.keys())

    matched = orig_flag_set & gen_flag_set
    in_gen_only = gen_flag_set - orig_flag_set
    in_orig_only = orig_flag_set - gen_flag_set

    print(f"\n{'='*90}")
    print(f"  {name}")
    print(f"{'='*90}")
    print(f"  Original entries: {len(orig_entries)} (unique flags: {len(orig_flag_set)})")
    print(f"  Generated entries: {len(gen_entries)} (unique flags: {len(gen_flag_set)})")
    print(f"  Matched (same flag in both): {len(matched)}")
    print(f"  In generated only: {len(in_gen_only)}")
    print(f"  In original only: {len(in_orig_only)}")

    # Classify new entries in generated
    classification_counts = defaultdict(int)
    gen_only_details = []

    if in_gen_only:
        print(f"\n  --- Entries in GENERATED but NOT in original ({len(in_gen_only)}) ---")
        for flag in sorted(in_gen_only, key=lambda x: int(x)):
            info = flag_db.get(flag)
            cls = classify_entry(info)
            item_names = get_item_names(info)
            map_id = info.get('map', '?') if info else '?'
            source = info.get('source', '?') if info else '?'
            classification_counts[cls] += 1
            gen_only_details.append((flag, item_names, map_id, source, cls))
            print(f"    flag={flag:<12} map={map_id:<16} source={source:<10} [{item_names}]")

        print(f"\n  Classification summary (new in generated):")
        for cls, count in sorted(classification_counts.items(), key=lambda x: -x[1]):
            print(f"    {cls}: {count}")

    orig_only_details = []
    if in_orig_only:
        print(f"\n  --- Entries in ORIGINAL but NOT in generated ({len(in_orig_only)}) ---")
        for flag in sorted(in_orig_only, key=lambda x: int(x)):
            info = flag_db.get(flag)
            item_names = get_item_names(info)
            map_id = info.get('map', '?') if info else '?'
            source = info.get('source', '?') if info else '?'
            cls = classify_entry(info)
            orig_only_details.append((flag, item_names, map_id, source, cls))
            print(f"    flag={flag:<12} map={map_id:<16} source={source:<10} [{item_names}]")

    return {
        'name': name,
        'orig_entries': len(orig_entries),
        'gen_entries': len(gen_entries),
        'orig_flags': len(orig_flag_set),
        'gen_flags': len(gen_flag_set),
        'matched': len(matched),
        'gen_only': len(in_gen_only),
        'orig_only': len(in_orig_only),
        'classification': dict(classification_counts),
        'gen_only_details': gen_only_details,
        'orig_only_details': orig_only_details,
    }


def main():
    flag_db = load_items_db(DB_PATH)

    results = []
    for name in PAIRS:
        result = compare_pair(name, flag_db)
        if result:
            results.append(result)

    # Final summary table
    print(f"\n\n{'='*90}")
    print(f"  SUMMARY TABLE")
    print(f"{'='*90}")
    print(f"  {'Category':<30} {'OrigFlg':>8} {'GenFlg':>8} {'Match':>6} {'GenOnly':>8} {'OrigOnly':>9}")
    print(f"  {'-'*30} {'-'*8} {'-'*8} {'-'*6} {'-'*8} {'-'*9}")
    for r in results:
        print(f"  {r['name']:<30} {r['orig_flags']:>8} {r['gen_flags']:>8} {r['matched']:>6} {r['gen_only']:>8} {r['orig_only']:>9}")

    # Classification summary across all categories
    print(f"\n  CLASSIFICATION OF NEW ENTRIES (generated only) ACROSS ALL CATEGORIES:")
    all_cls = defaultdict(int)
    for r in results:
        for cls, count in r['classification'].items():
            all_cls[cls] += count

    print(f"  {'Classification':<30} {'Count':>6}")
    print(f"  {'-'*30} {'-'*6}")
    for cls, count in sorted(all_cls.items(), key=lambda x: -x[1]):
        print(f"  {cls:<30} {count:>6}")
    print(f"  {'TOTAL':<30} {sum(all_cls.values()):>6}")


if __name__ == '__main__':
    main()
