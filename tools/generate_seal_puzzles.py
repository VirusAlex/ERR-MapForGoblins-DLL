#!/usr/bin/env python3
"""Generate World - Seal Puzzles.MASSEDIT from data/seal_puzzles.json.

Each per-seal activation flag (param[1] in CSEmkEvent template 90006051)
is the engine-set flag that fires when the seal is interacted with.
We bind that flag as textDisableFlagId1 so the icon hides automatically
after activation."""
import sys, io, json
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import config
from massedit_common import (OUT_DIR, UNDERGROUND_AREAS, DLC_AREAS,
                             OVERWORLD_AREAS, resolve_location_id_at)

ICON_ID = 438       # seal-puzzle icon (image 162, blue-cyan tint)
ROW_START = 8900000


def get_disp(area):
    if area in UNDERGROUND_AREAS: return 'dispMask01'
    if area in DLC_AREAS: return 'pad2_0'
    return 'dispMask00'


def main():
    puzzles_path = config.DATA_DIR / 'seal_puzzles.json'
    if not puzzles_path.exists():
        print(f"  {puzzles_path} missing - skip"); return
    puzzles = json.load(open(puzzles_path, encoding='utf-8'))

    lines = []
    row_id = ROW_START
    written = 0
    # Dedup: in ER overworld, m60_XX_YY_00 and m60_XX_YY_10 are phase
    # variants of the same map; identical EMEVD references both, so the
    # extractor finds every seal twice. Keep only the first occurrence
    # per (area, rounded_x, rounded_z, flag) tuple.
    seen = set()
    # Label per puzzle type, via ActionButtonText FMG (offset +800M):
    #   9503 = "Examine seal"  - base seal puzzles (template 90006050/51)
    #   9520 = "Light flame"   - chalices/lanterns/seal-release statues
    # Puzzles in seal_puzzles.json are tagged with a "label" field iff they
    # were added by the extra-puzzle extractor (Sellia chalices, Snow Town
    # statues, Siofra lanterns). Real seals don't have that field.
    SEAL_TEXT_ID = 800000000 + 9503   # "Examine seal"
    FLAME_TEXT_ID = 800000000 + 9520  # "Light flame"
    for p in puzzles:
        tile = p['tile']
        parts = tile.replace('m', '').split('_')
        if len(parts) < 4: continue
        area = int(parts[0])
        gx = int(parts[1])
        gz = int(parts[2])
        text_id = FLAME_TEXT_ID if p.get('label') else SEAL_TEXT_ID
        for s in p['seals']:
            part = s.get('part')
            if not part:
                continue  # no MSB part for this seal (shouldn't happen)
            x, y, z = part['x'], part['y'], part['z']
            flag = s['flag_activated']
            dedup_key = (area, gx, gz, round(x, 1), round(z, 1), flag)
            if dedup_key in seen:
                continue
            seen.add(dedup_key)
            disp = get_disp(area)
            lines.append(f'param WorldMapPointParam: id {row_id}: iconId: = {ICON_ID};')
            lines.append(f'param WorldMapPointParam: id {row_id}: {disp}: = 1;')
            lines.append(f'param WorldMapPointParam: id {row_id}: areaNo: = {area};')
            if area in OVERWORLD_AREAS or area in DLC_AREAS or gx > 0:
                lines.append(f'param WorldMapPointParam: id {row_id}: gridXNo: = {gx};')
                lines.append(f'param WorldMapPointParam: id {row_id}: gridZNo: = {gz};')
            lines.append(f'param WorldMapPointParam: id {row_id}: posX: = {x:.3f};')
            if y != 0:
                lines.append(f'param WorldMapPointParam: id {row_id}: posY: = {y:.3f};')
            lines.append(f'param WorldMapPointParam: id {row_id}: posZ: = {z:.3f};')
            # Label resolved at runtime via goblin_messages.cpp from
            # ActionButtonText FMG (+800M offset) - keeps localization.
            lines.append(f'param WorldMapPointParam: id {row_id}: textId1: = {text_id};')
            if flag > 0:
                lines.append(f'param WorldMapPointParam: id {row_id}: textDisableFlagId1: = {flag};')
            # Location subtitle for non-overworld tiles
            loc_id = resolve_location_id_at(tile, x, y, z)
            if loc_id > 0:
                lines.append(f'param WorldMapPointParam: id {row_id}: textId2: = {loc_id};')
                if flag > 0:
                    lines.append(f'param WorldMapPointParam: id {row_id}: textDisableFlagId2: = {flag};')
            lines.append(f'param WorldMapPointParam: id {row_id}: selectMinZoomStep: = 1;')
            row_id += 1
            written += 1

    out_path = OUT_DIR / 'World - Seal Puzzles.MASSEDIT'
    with open(out_path, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')
    print(f"Written {written} seal markers to {out_path.name}")


if __name__ == '__main__':
    main()
