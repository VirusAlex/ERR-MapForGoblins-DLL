#!/usr/bin/env python3
"""Show textId slot usage stats for all generated MASSEDIT files.
Reports how many entries have location name, enemy name, or both."""

import re
import sys
import io
import os
import glob

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')

MASSEDIT_DIR = os.path.join(os.path.dirname(__file__), '..', 'data', 'massedit_generated')

total_all = loc_all = enemy_all = both_all = 0

for fpath in sorted(glob.glob(os.path.join(MASSEDIT_DIR, '*.MASSEDIT'))):
    fname = os.path.basename(fpath).replace('.MASSEDIT', '')

    rows = {}
    for line in open(fpath):
        m = re.match(r'param WorldMapPointParam: id (\d+):', line)
        if not m:
            continue
        rid = int(m.group(1))
        if rid not in rows:
            rows[rid] = []
        tm = re.search(r'textId(\d): = (\d+)', line)
        if tm:
            rows[rid].append((int(tm.group(1)), int(tm.group(2))))

    total = len(rows)
    has_loc = has_enemy = has_both = 0
    for rid, tids in rows.items():
        loc = any(s >= 2 and t < 900000000 and t >= 1000 for s, t in tids)
        enemy = any(t >= 900000000 for s, t in tids)
        if loc:
            has_loc += 1
        if enemy:
            has_enemy += 1
        if loc and enemy:
            has_both += 1

    total_all += total
    loc_all += has_loc
    enemy_all += has_enemy
    both_all += has_both
    if total > 0:
        print(f'{fname:40s} {total:5d} | loc:{has_loc:4d} | enemy:{has_enemy:4d} | both:{has_both:3d}')

print(f'{"TOTAL":40s} {total_all:5d} | loc:{loc_all:4d} | enemy:{enemy_all:4d} | both:{both_all:3d}')
