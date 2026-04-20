#!/usr/bin/env python3
"""Generate World - Spirit Springs.MASSEDIT and World - Spiritspring Hawks.MASSEDIT
from MSB MountJumps/LockedMountJumps regions and c4210 hawk enemies."""

import sys
import io
import os
import tempfile

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')

import config
from pythonnet import load
load('coreclr')
import clr
from System.Reflection import Assembly, BindingFlags
from System import Array, Type as SysType, Object
from System.IO import File as SysFile

asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
clr.AddReference(str(config.SOULSFORMATS_DLL))
import SoulsFormats

from massedit_common import OUT_DIR, UNDERGROUND_AREAS, DLC_AREAS, OVERWORLD_AREAS, resolve_location_id

ERR_MOD_DIR = config.require_err_mod_dir()
_str_type = SysType.GetType('System.String')
_msbe_read = asm.GetType('SoulsFormats.MSBE').GetMethod('Read',
    BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str_type]), None)


def rfb(rm, data, suf='.bin'):
    tmp = os.path.join(tempfile.gettempdir(), '_mfg_tmp' + suf)
    if hasattr(data, 'ToArray'):
        SysFile.WriteAllBytes(tmp, data.ToArray())
    else:
        SysFile.WriteAllBytes(tmp, data)
    r = rm.Invoke(None, Array[Object]([tmp]))
    os.unlink(tmp)
    return r


def write_massedit(entries, out_name, icon_id, text_id, start_row, zoom=1):
    """Write a MASSEDIT file from a list of entry dicts."""
    lines = []
    row_id = start_row
    for e in entries:
        area = e['area']
        gx = e['gx']
        gz = e['gz']

        if area in UNDERGROUND_AREAS:
            disp = 'dispMask01'
        elif area in DLC_AREAS:
            disp = 'pad2_0'
        else:
            disp = 'dispMask00'

        lines.append(f'param WorldMapPointParam: id {row_id}: iconId: = {icon_id};')
        lines.append(f'param WorldMapPointParam: id {row_id}: {disp}: = 1;')
        lines.append(f'param WorldMapPointParam: id {row_id}: areaNo: = {area};')
        if area in OVERWORLD_AREAS or area in DLC_AREAS or gx > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: gridXNo: = {gx};')
            lines.append(f'param WorldMapPointParam: id {row_id}: gridZNo: = {gz};')
        lines.append(f'param WorldMapPointParam: id {row_id}: posX: = {e["x"]:.3f};')
        if e['y'] != 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: posY: = {e["y"]:.3f};')
        lines.append(f'param WorldMapPointParam: id {row_id}: posZ: = {e["z"]:.3f};')
        lines.append(f'param WorldMapPointParam: id {row_id}: textId1: = {text_id};')
        map_code = f'm{area:02d}_{gx:02d}_{gz:02d}_00'
        loc_id = resolve_location_id(map_code)
        if loc_id > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: textId2: = {loc_id};')
        flag = e.get('flag', 0)
        if flag > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: clearedEventFlagId: = {flag};')
        lines.append(f'param WorldMapPointParam: id {row_id}: selectMinZoomStep: = {zoom};')
        row_id += 1

    out_path = OUT_DIR / out_name
    with open(out_path, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')
    return len(entries)


def main():
    MSB_DIR = ERR_MOD_DIR / 'map' / 'MapStudio'

    springs = []
    hawks = []
    seen_springs = set()
    seen_hawks = set()

    print("Scanning MSBs...")
    for msb_path in sorted(MSB_DIR.glob('*.msb.dcx')):
        map_name = msb_path.name.replace('.msb.dcx', '')
        parts = map_name.split('_')
        if len(parts) < 4:
            continue
        area = int(parts[0][1:])
        gx = int(parts[1])
        gz = int(parts[2])
        try:
            msb = rfb(_msbe_read, SoulsFormats.DCX.Decompress(str(msb_path)), '.msb')
        except:
            continue

        # Spirit Springs: MountJumps/Falls regions
        for rtype in ['MountJumps', 'LockedMountJumps',
                      'MountJumpFalls', 'LockedMountJumpFalls']:
            coll = getattr(msb.Regions, rtype, None)
            if not coll:
                continue
            for r in coll:
                x = round(float(r.Position.X), 3)
                y = round(float(r.Position.Y), 3)
                z = round(float(r.Position.Z), 3)
                key = (area, round(x, 0), round(z, 0))
                if key in seen_springs:
                    continue
                seen_springs.add(key)
                springs.append({
                    'area': area, 'gx': gx, 'gz': gz,
                    'x': x, 'y': y, 'z': z,
                })

        # ERR FakeSpiritSpring in Others regions
        for r in msb.Regions.Others:
            rname = str(r.Name) if hasattr(r, 'Name') else ''
            if 'FakeSpiritSpringJump' not in rname:
                continue
            x = round(float(r.Position.X), 3)
            y = round(float(r.Position.Y), 3)
            z = round(float(r.Position.Z), 3)
            key = (area, round(x, 0), round(z, 0))
            if key in seen_springs:
                continue
            seen_springs.add(key)
            springs.append({
                'area': area, 'gx': gx, 'gz': gz,
                'x': x, 'y': y, 'z': z,
            })

        # DLC AEG463_200 assets (spirit springs without MountJumps region)
        for p in msb.Parts.Assets:
            if str(p.ModelName) != 'AEG463_200':
                continue
            x = round(float(p.Position.X), 3)
            y = round(float(p.Position.Y), 3)
            z = round(float(p.Position.Z), 3)
            key = (area, round(x, 0), round(z, 0))
            if key in seen_springs:
                continue
            seen_springs.add(key)
            springs.append({
                'area': area, 'gx': gx, 'gz': gz,
                'x': x, 'y': y, 'z': z,
            })

        # Spiritspring Hawks: c4210 enemies with EntityID ending in 0980 or 0971
        for p in msb.Parts.Enemies:
            eid = int(p.EntityID)
            model = str(p.ModelName)
            if eid <= 0 or model != 'c4210':
                continue
            suffix = eid % 10000
            if suffix not in (980, 971):
                continue
            key = (area, eid)
            if key in seen_hawks:
                continue
            seen_hawks.add(key)
            hawks.append({
                'area': area, 'gx': gx, 'gz': gz,
                'x': round(float(p.Position.X), 3),
                'y': round(float(p.Position.Y), 3),
                'z': round(float(p.Position.Z), 3),
                'flag': eid,  # EntityID = kill flag = spring unlock flag
            })

    springs.sort(key=lambda s: (s['area'], s['gx'], s['gz']))
    hawks.sort(key=lambda h: (h['area'], h['gx'], h['gz']))

    print(f"  {len(springs)} spirit springs, {len(hawks)} spiritspring hawks")

    # Write Spirit Springs (icon 404 = MENU_MAP_Range, tutorialId 301620 = "Spiritspring Jumping")
    n = write_massedit(springs, 'World - Spirit Springs.MASSEDIT',
                       icon_id=404, text_id=900301620, start_row=8600000)
    print(f"Written {n} springs to World - Spirit Springs.MASSEDIT")

    # Write Hawks (icon 403, textId 904210304 = "167. Spiritspring Stormhawk")
    # clearedEventFlagId = hawk EntityID (set when hawk killed = spring unlocked)
    n = write_massedit(hawks, 'World - Spiritspring Hawks.MASSEDIT',
                       icon_id=403, text_id=904210304, start_row=8650000)
    print(f"Written {n} hawks to World - Spiritspring Hawks.MASSEDIT")


if __name__ == '__main__':
    main()
