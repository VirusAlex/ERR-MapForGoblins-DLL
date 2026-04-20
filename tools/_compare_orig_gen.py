import json, re, os, io, sys
from collections import defaultdict, Counter
from pathlib import Path

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')

sys.path.insert(0, str(Path(__file__).parent))
import config
BASE = str(config.PROJECT_DIR)

with open(f"{BASE}/data/items_database.json", "r", encoding="utf-8") as f:
    items_db = json.load(f)

flag_lookup = {}
for item in items_db:
    flag = item.get("eventFlag")
    if flag:
        flag_lookup[flag] = item

def parse_massedit(filepath):
    rows = defaultdict(dict)
    if not os.path.exists(filepath):
        print(f"  FILE NOT FOUND: {filepath}")
        return rows
    with open(filepath, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            m = re.match(r'param \w+: id (\d+): (\w+): = (.+);', line)
            if m:
                row_id = int(m.group(1))
                field = m.group(2)
                value = m.group(3).strip()
                rows[row_id][field] = value
    return rows

def get_flags(rows):
    flag_to_rows = {}
    for row_id, fields in rows.items():
        flag = fields.get("textDisableFlagId1")
        if flag and flag != "0":
            flag_int = int(flag)
            if flag_int not in flag_to_rows:
                flag_to_rows[flag_int] = []
            flag_to_rows[flag_int].append(row_id)
    return flag_to_rows

def classify_item(name):
    """Classify item by its name into a broad type."""
    n = name.lower()
    if 'golden rune' in n or "hero's rune" in n or "lord's rune" in n or "numen's rune" in n or "shadow realm rune" in n or "broken rune" in n or "rune of an unsung" in n or "marika's rune" in n:
        return "Golden/Hero/Lord Runes"
    if n == 'rune arc' or 'rune arc' in n:
        return "Rune Arcs"
    if 'stonesword key' in n:
        return "Stonesword Keys"
    if 'smithing stone' in n or 'smithing scadushard' in n or 'somber smithing' in n or 'ancient dragon smithing' in n or 'somber ancient' in n:
        return "Smithing Stones/Scadushards"
    if 'glovewort' in n or 'ghost glovewort' in n:
        return "Gloveworts"
    if 'grease' in n:
        return "Greases"
    if 'boluses' in n:
        return "Boluses"
    if 'prattling pate' in n:
        return "Prattling Pates"
    if 'glass shard' in n:
        return "Glass Shards"
    if 'raw meat dumpling' in n:
        return "Raw Meat Dumplings"
    if 'lamp oil' in n:
        return "Lamp Oil"
    if 'dragon heart' in n:
        return "Dragon Hearts"
    if 'sign of the all-knowing' in n:
        return "Signs of the All-Knowing"
    if 'lost ashes' in n:
        return "Lost Ashes"
    if 'dart' in n or 'kukri' in n or 'dagger' in n:
        return "Throwing Items (Darts/Kukri/Daggers)"
    if 'warming stone' in n or 'sunwarmth' in n:
        return "Warming/Sunwarmth Stones"
    if 'crystal dart' in n:
        return "Crystal Darts"
    return "Other"

def summarize_missing(flags, flag_lookup, orig_flags, orig_rows, label):
    """Summarize missing entries by item type."""
    by_type = defaultdict(list)
    by_source = Counter()
    by_broad = Counter()
    unknown = []

    for flag in sorted(flags):
        info = flag_lookup.get(flag)
        if info:
            items = info.get("items", [])
            name = items[0]["name"] if items else "???"
            item_type = classify_item(name)
            source = info.get("source", "unknown")
            broad = info.get("primary_category", "unknown")
            by_type[item_type].append(flag)
            by_source[source] += 1
            by_broad[broad] += 1
        else:
            unknown.append(flag)

    print(f"\n  {label} ({len(flags)} total)")
    print(f"  {'Item Type':<45} {'Count':>6}")
    print(f"  {'-'*45} {'-'*6}")
    for itype in sorted(by_type.keys(), key=lambda x: -len(by_type[x])):
        print(f"  {itype:<45} {len(by_type[itype]):>6}")
    if unknown:
        print(f"  {'NOT IN DATABASE':<45} {len(unknown):>6}")

    print(f"\n  By source: {dict(by_source.most_common())}")
    print(f"  By broad_category: {dict(by_broad.most_common())}")


def compare_pair(orig_path, gen_path, pair_name):
    print(f"\n{'='*100}")
    print(f"PAIR: {pair_name}")
    print(f"  Original:  {os.path.basename(orig_path)}")
    print(f"  Generated: {os.path.basename(gen_path)}")
    print(f"{'='*100}")

    orig_rows = parse_massedit(orig_path)
    gen_rows = parse_massedit(gen_path)

    orig_flags = get_flags(orig_rows)
    gen_flags = get_flags(gen_rows)

    only_in_orig = set(orig_flags.keys()) - set(gen_flags.keys())
    only_in_gen = set(gen_flags.keys()) - set(orig_flags.keys())
    in_both = set(orig_flags.keys()) & set(gen_flags.keys())

    print(f"\n  Original: {len(orig_rows)} rows, {len(orig_flags)} unique flags")
    print(f"  Generated: {len(gen_rows)} rows, {len(gen_flags)} unique flags")
    print(f"  Common: {len(in_both)} | Only in original: {len(only_in_orig)} | Only in generated: {len(only_in_gen)}")

    if only_in_orig:
        summarize_missing(only_in_orig, flag_lookup, orig_flags, orig_rows,
                         "IN ORIGINAL BUT NOT IN GENERATED")

    if only_in_gen:
        summarize_missing(only_in_gen, flag_lookup, gen_flags, gen_rows,
                         "IN GENERATED BUT NOT IN ORIGINAL")

    if not only_in_orig and not only_in_gen:
        print("\n  PERFECT MATCH - no differences found.")

    return {
        "pair": pair_name,
        "orig_flags": len(orig_flags),
        "gen_flags": len(gen_flags),
        "only_orig": len(only_in_orig),
        "only_gen": len(only_in_gen),
        "common": len(in_both)
    }

pairs = [
    (
        f"{BASE}/data/massedit/Loot - Consumables.MASSEDIT",
        f"{BASE}/data/massedit_generated/Loot - Consumables.MASSEDIT",
        "Loot - Consumables"
    ),
    (
        f"{BASE}/data/massedit/Loot - Unique_Drops.MASSEDIT",
        f"{BASE}/data/massedit_generated/Loot - Unique_Drops.MASSEDIT",
        "Loot - Unique_Drops"
    ),
    (
        f"{BASE}/data/massedit/Loot - Somber_Scarab.MASSEDIT",
        f"{BASE}/data/massedit_generated/Loot - Smithing Stones (Rare).MASSEDIT",
        "Loot - Somber_Scarab vs Smithing Stones (Rare)"
    ),
    (
        f"{BASE}/data/massedit/Loot - Stonesword_Keys.MASSEDIT",
        f"{BASE}/data/massedit_generated/Loot - Stonesword_Keys.MASSEDIT",
        "Loot - Stonesword_Keys"
    ),
    (
        f"{BASE}/data/massedit/Loot - Material Nodes (DOES NOT DISAPPEAR).MASSEDIT",
        f"{BASE}/data/massedit_generated/Loot - Material Nodes.MASSEDIT",
        "Loot - Material Nodes"
    ),
]

summaries = []
for orig, gen, name in pairs:
    s = compare_pair(orig, gen, name)
    summaries.append(s)

print(f"\n\n{'='*100}")
print("OVERALL SUMMARY TABLE")
print(f"{'='*100}")
print(f"{'Pair':<55} {'Orig':>6} {'Gen':>6} {'Both':>6} {'OnlyO':>6} {'OnlyG':>6}")
print("-"*85)
for s in summaries:
    print(f"{s['pair']:<55} {s['orig_flags']:>6} {s['gen_flags']:>6} {s['common']:>6} {s['only_orig']:>6} {s['only_gen']:>6}")
