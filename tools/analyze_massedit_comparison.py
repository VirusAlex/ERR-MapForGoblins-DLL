"""Analyze original vs generated MASSEDIT files for Quest and Reforged categories."""
import re
from collections import defaultdict, Counter

def parse_massedit(filepath):
    """Parse MASSEDIT file into dict of {row_id: {field: value}}."""
    rows = defaultdict(dict)
    with open(filepath, 'r') as f:
        for line in f:
            m = re.match(r'param WorldMapPointParam: id (\d+): (\w+): = (.+);', line.strip())
            if m:
                row_id, field, value = m.groups()
                rows[int(row_id)][field] = value.strip()
    return dict(rows)

def analyze_quest_progression():
    base = r"G:\Games\Elden Ring\ERR saves piece\MapForGoblins\data\massedit"
    base_gen = r"G:\Games\Elden Ring\ERR saves piece\MapForGoblins\data\massedit_generated"

    orig = parse_massedit(f"{base}\\Quest - Progression.MASSEDIT")
    gen = parse_massedit(f"{base_gen}\\Quest - Progression.MASSEDIT")

    print("=" * 70)
    print("1. QUEST - PROGRESSION COMPARISON")
    print("=" * 70)
    print(f"Original entries: {len(orig)}")
    print(f"Generated entries: {len(gen)}")
    print()

    # Build flag->row maps
    orig_by_flag = {}
    for rid, fields in orig.items():
        flag = fields.get('textDisableFlagId1', '')
        orig_by_flag[flag] = (rid, fields)

    gen_by_flag = {}
    for rid, fields in gen.items():
        flag = fields.get('textDisableFlagId1', '')
        gen_by_flag[flag] = (rid, fields)

    matched_flags = set(orig_by_flag.keys()) & set(gen_by_flag.keys())
    orig_only_flags = set(orig_by_flag.keys()) - set(gen_by_flag.keys())
    gen_only_flags = set(gen_by_flag.keys()) - set(orig_by_flag.keys())

    print(f"Matched by textDisableFlagId1: {len(matched_flags)}")
    print(f"Original only (missing from generated): {len(orig_only_flags)}")
    print(f"Generated only (not in original): {len(gen_only_flags)}")
    print()

    # Differences in matched entries
    print("--- DIFFERENCES IN MATCHED ENTRIES ---")
    diff_count = 0
    for flag in sorted(matched_flags, key=lambda x: orig_by_flag[x][0]):
        o_rid, o_fields = orig_by_flag[flag]
        g_rid, g_fields = gen_by_flag[flag]
        diffs = []
        all_keys = set(o_fields.keys()) | set(g_fields.keys())
        for k in sorted(all_keys):
            ov = o_fields.get(k, '<missing>')
            gv = g_fields.get(k, '<missing>')
            if ov != gv:
                diffs.append(f"  {k}: orig={ov} gen={gv}")
        if diffs:
            diff_count += 1
            print(f"Flag {flag} (orig id {o_rid}, gen id {g_rid}):")
            for d in diffs:
                print(d)
    if diff_count == 0:
        print("  (no differences in matched entries)")
    print()

    # Original-only entries analysis
    print("--- ORIGINAL-ONLY ENTRIES (35 missing from generated) ---")
    npc_count = 0
    item_count = 0
    npc_entries = []
    item_entries = []

    for flag in sorted(orig_only_flags, key=lambda x: orig_by_flag[x][0]):
        rid, fields = orig_by_flag[flag]
        tid1 = fields.get('textId1', '?')
        area = fields.get('areaNo', '?')
        tid_str = str(tid1)
        is_npc = tid_str.startswith('9') and len(tid_str) == 7

        extras = []
        for i in range(2, 8):
            tk = f'textId{i}'
            if tk in fields:
                extras.append(f'{tk}={fields[tk]}')
        extra_str = ' | ' + ', '.join(extras) if extras else ''

        entry = f"  id={rid} flag={flag} textId1={tid1} area={area}{extra_str}"
        if is_npc:
            npc_count += 1
            npc_entries.append(entry)
        else:
            item_count += 1
            item_entries.append(entry)

    print(f"\nClassification: {npc_count} NPC position trackers, {item_count} item/other entries")
    print(f"\nNPC position trackers (textId1 = 9XXXXXXX):")
    for e in npc_entries:
        print(e)
    print(f"\nItem/other entries:")
    for e in item_entries:
        print(e)

    # Area distribution for missing entries
    print(f"\nArea distribution of missing entries:")
    area_counter = Counter()
    for flag in orig_only_flags:
        rid, fields = orig_by_flag[flag]
        area_counter[fields.get('areaNo', '?')] += 1
    for area, count in sorted(area_counter.items()):
        print(f"  area {area}: {count}")

    # Generated-only
    if gen_only_flags:
        print(f"\n--- GENERATED-ONLY ENTRIES (not in original) ---")
        for flag in sorted(gen_only_flags, key=lambda x: gen_by_flag[x][0]):
            rid, fields = gen_by_flag[flag]
            tid1 = fields.get('textId1', '?')
            area = fields.get('areaNo', '?')
            extras = []
            for i in range(2, 8):
                tk = f'textId{i}'
                if tk in fields:
                    extras.append(f'{tk}={fields[tk]}')
            extra_str = ' | ' + ', '.join(extras) if extras else ''
            print(f"  id={rid} flag={flag} textId1={tid1} area={area}{extra_str}")

def analyze_camp_contents():
    base = r"G:\Games\Elden Ring\ERR saves piece\MapForGoblins\data\massedit"
    rows = parse_massedit(f"{base}\\Reforged - camp contents.MASSEDIT")

    print()
    print("=" * 70)
    print("2. REFORGED - CAMP CONTENTS")
    print("=" * 70)
    print(f"Total entries (row IDs): {len(rows)}")
    print()

    # Analyze what fields are being set
    all_fields = set()
    for fields in rows.values():
        all_fields.update(fields.keys())
    print(f"Fields used: {sorted(all_fields)}")
    print()

    # Group by row ID patterns
    icon_counter = Counter()
    area_counter = Counter()
    text_ids = set()
    text_disable_flags = set()

    for rid, fields in sorted(rows.items()):
        icon = fields.get('iconId', '-')
        icon_counter[icon] += 1
        area = fields.get('areaNo', '-')
        area_counter[area] += 1
        for i in range(1, 8):
            tk = f'textId{i}'
            if tk in fields:
                text_ids.add(fields[tk])
            tfk = f'textDisableFlagId{i}'
            if tfk in fields:
                text_disable_flags.add(fields[tfk])

    print(f"Icon IDs: {dict(icon_counter)}")
    print(f"Areas: {dict(sorted(area_counter.items()))}")
    print()

    # Show all entries
    print("All entries:")
    for rid, fields in sorted(rows.items()):
        text_parts = []
        for i in range(1, 8):
            tk = f'textId{i}'
            tfk = f'textDisableFlagId{i}'
            if tk in fields:
                text_parts.append(f"text{i}={fields[tk]}(flag={fields.get(tfk, '?')})")
        area = fields.get('areaNo', '-')
        icon = fields.get('iconId', '-')
        print(f"  id={rid}: icon={icon} area={area} | {'; '.join(text_parts)}")

    print()
    print(f"Unique textId values: {sorted(text_ids)}")

def analyze_items_and_changes():
    base = r"G:\Games\Elden Ring\ERR saves piece\MapForGoblins\data\massedit"
    rows = parse_massedit(f"{base}\\Reforged - items and changes.MASSEDIT")

    print()
    print("=" * 70)
    print("3. REFORGED - ITEMS AND CHANGES")
    print("=" * 70)
    print(f"Total entries (row IDs): {len(rows)}")
    print()

    # Analyze fields
    all_fields = set()
    for fields in rows.values():
        all_fields.update(fields.keys())
    print(f"Fields used: {sorted(all_fields)}")
    print()

    # Categorize by iconId
    icon_counter = Counter()
    area_counter = Counter()
    text_id1_counter = Counter()

    # Group by icon
    by_icon = defaultdict(list)
    for rid, fields in sorted(rows.items()):
        icon = fields.get('iconId', '-')
        icon_counter[icon] += 1
        area = fields.get('areaNo', '-')
        area_counter[area] += 1
        tid1 = fields.get('textId1', '-')
        text_id1_counter[tid1] += 1
        by_icon[icon].append((rid, fields))

    print(f"Icon ID distribution:")
    for icon, count in sorted(icon_counter.items(), key=lambda x: -x[1]):
        print(f"  iconId {icon}: {count} entries")
    print()

    print(f"Area distribution:")
    for area, count in sorted(area_counter.items()):
        print(f"  area {area}: {count} entries")
    print()

    print(f"textId1 distribution (top 20):")
    for tid, count in text_id1_counter.most_common(20):
        print(f"  textId1 {tid}: {count} entries")
    print()

    # Check for patterns - are these appending text slots to existing rows?
    # or creating new rows?
    has_pos = 0
    no_pos = 0
    has_only_text = 0
    for rid, fields in rows.items():
        has_position = 'posX' in fields or 'posZ' in fields
        has_text_only = not has_position and any(f.startswith('textId') for f in fields)
        if has_position:
            has_pos += 1
        else:
            no_pos += 1
        if has_text_only:
            has_only_text += 1

    print(f"Entries with position data: {has_pos}")
    print(f"Entries without position (text-only updates): {no_pos}")
    print(f"Entries that are text-only updates: {has_only_text}")
    print()

    # Show samples by icon
    for icon in sorted(by_icon.keys()):
        entries = by_icon[icon]
        print(f"--- iconId {icon} ({len(entries)} entries) ---")
        for rid, fields in entries[:5]:  # Show first 5
            text_parts = []
            for i in range(1, 8):
                tk = f'textId{i}'
                if tk in fields:
                    text_parts.append(f"t{i}={fields[tk]}")
            area = fields.get('areaNo', '-')
            has_pos = 'posX' in fields
            pos_str = f"pos=({fields.get('posX','?')},{fields.get('posZ','?')})" if has_pos else "NO POS"
            print(f"  id={rid}: area={area} {pos_str} | {'; '.join(text_parts)}")
        if len(entries) > 5:
            print(f"  ... and {len(entries) - 5} more")

    # Check if row IDs overlap with existing game rows
    print()
    print("Row ID ranges:")
    rids = sorted(rows.keys())
    if rids:
        # Group consecutive ranges
        ranges = []
        start = rids[0]
        prev = rids[0]
        for r in rids[1:]:
            if r - prev > 100:
                ranges.append((start, prev))
                start = r
            prev = r
        ranges.append((start, prev))
        for s, e in ranges:
            count = sum(1 for r in rids if s <= r <= e)
            print(f"  {s}-{e}: {count} entries")

def analyze_quest_details():
    base = r"G:\Games\Elden Ring\ERR saves piece\MapForGoblins\data\massedit"
    orig = parse_massedit(f"{base}\\Quest - Progression.MASSEDIT")

    print()
    print("=" * 70)
    print("EXTRA: QUEST PROGRESSION DETAILS")
    print("=" * 70)

    # Count how many entries share flag 580600
    flag_groups = defaultdict(list)
    for rid, fields in sorted(orig.items()):
        flag = fields.get('textDisableFlagId1', '?')
        flag_groups[flag].append((rid, fields))

    shared_flags = {f: entries for f, entries in flag_groups.items() if len(entries) > 1}
    if shared_flags:
        print(f"\nFlags shared by multiple entries:")
        for flag, entries in sorted(shared_flags.items()):
            print(f"  Flag {flag}: {len(entries)} entries")
            for rid, fields in entries:
                area = fields.get('areaNo', '?')
                tid1 = fields.get('textId1', '?')
                grid = f"grid=({fields.get('gridXNo','-')},{fields.get('gridZNo','-')})"
                print(f"    id={rid} area={area} {grid} textId1={tid1}")

    # textId1 classification
    print(f"\ntextId1 classification for all 48 entries:")
    base_npc = []
    dlc_npc = []
    for rid, fields in sorted(orig.items()):
        tid1 = fields.get('textId1', '0')
        t_int = int(tid1)
        if 9000000 <= t_int <= 9199999:
            base_npc.append((rid, fields))
        elif 9200000 <= t_int <= 9299999:
            dlc_npc.append((rid, fields))

    print(f"  Base game NPC references (9000000-9199999): {len(base_npc)}")
    for rid, f in base_npc:
        print(f"    id={rid} textId1={f['textId1']} area={f.get('areaNo','?')} flag={f.get('textDisableFlagId1','?')}")

    print(f"  DLC NPC references (9200000-9299999): {len(dlc_npc)}")
    for rid, f in dlc_npc:
        print(f"    id={rid} textId1={f['textId1']} area={f.get('areaNo','?')} flag={f.get('textDisableFlagId1','?')}")

    # Analyze the 2 generated-only entries
    gen = parse_massedit(r"G:\Games\Elden Ring\ERR saves piece\MapForGoblins\data\massedit_generated\Quest - Progression.MASSEDIT")
    print(f"\nGenerated-only entries details:")
    gen_by_flag = {}
    for rid, fields in gen.items():
        flag = fields.get('textDisableFlagId1', '')
        gen_by_flag[flag] = (rid, fields)

    orig_by_flag = {}
    for rid, fields in orig.items():
        flag = fields.get('textDisableFlagId1', '')
        orig_by_flag[flag] = (rid, fields)

    gen_only = set(gen_by_flag.keys()) - set(orig_by_flag.keys())
    for flag in sorted(gen_only):
        rid, fields = gen_by_flag[flag]
        print(f"  id={rid} flag={flag}")
        for k, v in sorted(fields.items()):
            print(f"    {k}: {v}")

def analyze_camp_text_ids():
    """Check what text IDs in camp contents mean."""
    base = r"G:\Games\Elden Ring\ERR saves piece\MapForGoblins\data\massedit"
    rows = parse_massedit(f"{base}\\Reforged - camp contents.MASSEDIT")

    print()
    print("=" * 70)
    print("EXTRA: CAMP CONTENTS TEXT ID ANALYSIS")
    print("=" * 70)

    # Row ID patterns - they look like existing WorldMapPointParam IDs being patched
    print("Row ID patterns:")
    rids = sorted(rows.keys())
    # 50XXYYYY pattern - looks like existing Grace/marker IDs
    for rid in rids:
        rid_str = str(rid)
        print(f"  id={rid} -> {rid_str[:2]}_{rid_str[2:4]}_{rid_str[4:]}")

    # textId ranges
    all_text = set()
    for fields in rows.values():
        for i in range(1, 8):
            tk = f'textId{i}'
            if tk in fields:
                all_text.add(int(fields[tk]))

    print(f"\ntextId value ranges:")
    for t in sorted(all_text):
        if 10500000 <= t <= 10500999:
            desc = "camp-specific text (10500XXX)"
        elif t == 10501000:
            desc = "camp item category?"
        elif t == 10502000:
            desc = "merchant/smithing?"
        elif t == 10503000:
            desc = "cookbooks?"
        elif t == 10507000:
            desc = "special?"
        elif t == 10511000:
            desc = "DLC?"
        else:
            desc = "?"
        print(f"  {t}: {desc}")

if __name__ == "__main__":
    analyze_quest_progression()
    analyze_camp_contents()
    analyze_items_and_changes()
    analyze_quest_details()
    analyze_camp_text_ids()
