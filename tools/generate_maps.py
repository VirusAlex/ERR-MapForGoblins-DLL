#!/usr/bin/env python3
"""Generate World - Maps.MASSEDIT from items_database.json Map: items."""

import json
import sys
import io

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')

from massedit_common import DATA_DIR, OUT_DIR, UNDERGROUND_AREAS, DLC_AREAS, OVERWORLD_AREAS, resolve_location_id


def main():
    with open(DATA_DIR / 'items_database.json', encoding='utf-8') as f:
        db = json.load(f)

    print("Extracting Map items...")
    maps = []
    seen_flags = set()
    for item in db:
        flag = item.get('eventFlag', 0)
        if flag <= 0 or flag in seen_flags:
            continue
        for it in item.get('items', []):
            name = it.get('name', '')
            if not name.startswith('Map:'):
                continue
            seen_flags.add(flag)
            maps.append({
                'name': name,
                'goods_id': it.get('id', 0),
                'flag': flag,
                'area': item.get('areaNo', 0),
                'gx': item.get('gridX', 0),
                'gz': item.get('gridZ', 0),
                'x': round(item.get('x', 0), 3),
                'y': round(item.get('y', 0), 3),
                'z': round(item.get('z', 0), 3),
                'map': item.get('map', ''),
            })
            break

    maps.sort(key=lambda m: m['goods_id'])
    print(f"  {len(maps)} unique maps")

    lines = []
    row_id = 7500000
    for m in maps:
        area = m['area']
        gx = m['gx']
        gz = m['gz']

        if area in UNDERGROUND_AREAS:
            disp = 'dispMask01'
        elif area in DLC_AREAS:
            disp = 'pad2_0'
        else:
            disp = 'dispMask00'

        lines.append(f'param WorldMapPointParam: id {row_id}: iconId: = 406;')
        lines.append(f'param WorldMapPointParam: id {row_id}: {disp}: = 1;')
        lines.append(f'param WorldMapPointParam: id {row_id}: areaNo: = {area};')
        if area in OVERWORLD_AREAS or area in DLC_AREAS or gx > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: gridXNo: = {gx};')
            lines.append(f'param WorldMapPointParam: id {row_id}: gridZNo: = {gz};')
        lines.append(f'param WorldMapPointParam: id {row_id}: posX: = {m["x"]:.3f};')
        if m['y'] != 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: posY: = {m["y"]:.3f};')
        lines.append(f'param WorldMapPointParam: id {row_id}: posZ: = {m["z"]:.3f};')
        # Map name from GoodsName
        lines.append(f'param WorldMapPointParam: id {row_id}: textId1: = {500000000 + m["goods_id"]};')
        # Hide when collected
        lines.append(f'param WorldMapPointParam: id {row_id}: textDisableFlagId1: = {m["flag"]};')
        # Location for dungeons
        loc_id = resolve_location_id(m['map'])
        if loc_id > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: textId2: = {loc_id};')
            lines.append(f'param WorldMapPointParam: id {row_id}: textDisableFlagId2: = {m["flag"]};')
        lines.append(f'param WorldMapPointParam: id {row_id}: selectMinZoomStep: = 1;')
        row_id += 1

    out_path = OUT_DIR / 'World - Maps.MASSEDIT'
    with open(out_path, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')
    print(f"Written {len(maps)} entries to {out_path.name}")


if __name__ == '__main__':
    main()
