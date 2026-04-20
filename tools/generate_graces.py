#!/usr/bin/env python3
"""Generate World - Graces.MASSEDIT from BonfireWarpParam."""

import json
import sys
import io

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')

import config
from pythonnet import load
load('coreclr')
import clr
from System.Reflection import Assembly, BindingFlags
from System import Array, Type as SysType, Object

asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
clr.AddReference(str(config.SOULSFORMATS_DLL))
import SoulsFormats

from extract_all_items import load_paramdefs, read_param, param_to_dict
from massedit_common import OUT_DIR, UNDERGROUND_AREAS, DLC_AREAS, OVERWORLD_AREAS

ERR_MOD_DIR = config.require_err_mod_dir()
bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(ERR_MOD_DIR / 'regulation.bin'))
paramdefs = load_paramdefs()


def main():
    print("Loading BonfireWarpParam...")
    bwp = read_param(bnd, 'BonfireWarpParam', paramdefs)
    fields = {'areaNo', 'gridXNo', 'gridZNo', 'posX', 'posY', 'posZ',
              'eventflagId', 'textId1', 'dispMask00', 'dispMask01', 'dispMask02'}
    data = param_to_dict(bwp, fields)

    lines = []
    row_id = 8800000
    count = 0
    for rid, row in sorted(data.items()):
        area = row.get('areaNo', 0)
        if area == 0:
            continue
        flag = row.get('eventflagId', 0)
        tid1 = row.get('textId1', 0)
        if flag <= 0 or tid1 <= 0:
            continue

        gx = row.get('gridXNo', 0)
        gz = row.get('gridZNo', 0)

        if row.get('dispMask01'):
            disp = 'dispMask01'
        elif row.get('dispMask02'):
            disp = 'pad2_0'
        elif area in DLC_AREAS:
            disp = 'pad2_0'
        elif area in UNDERGROUND_AREAS:
            disp = 'dispMask01'
        else:
            disp = 'dispMask00'

        lines.append(f'param WorldMapPointParam: id {row_id}: iconId: = 370;')
        lines.append(f'param WorldMapPointParam: id {row_id}: {disp}: = 1;')
        lines.append(f'param WorldMapPointParam: id {row_id}: areaNo: = {area};')
        if area in OVERWORLD_AREAS or area in DLC_AREAS or gx > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: gridXNo: = {gx};')
            lines.append(f'param WorldMapPointParam: id {row_id}: gridZNo: = {gz};')
        x = round(float(row.get('posX', 0)), 3)
        y = round(float(row.get('posY', 0)), 3)
        z = round(float(row.get('posZ', 0)), 3)
        lines.append(f'param WorldMapPointParam: id {row_id}: posX: = {x:.3f};')
        if y != 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: posY: = {y:.3f};')
        lines.append(f'param WorldMapPointParam: id {row_id}: posZ: = {z:.3f};')
        lines.append(f'param WorldMapPointParam: id {row_id}: textId1: = {tid1};')
        lines.append(f'param WorldMapPointParam: id {row_id}: textDisableFlagId1: = {flag};')
        lines.append(f'param WorldMapPointParam: id {row_id}: selectMinZoomStep: = 2;')
        row_id += 1
        count += 1

    out_path = OUT_DIR / 'World - Graces.MASSEDIT'
    with open(out_path, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')
    print(f"Written {count} graces to {out_path.name}")


if __name__ == '__main__':
    main()
