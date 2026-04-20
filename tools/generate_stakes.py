#!/usr/bin/env python3
"""Generate World - Stakes of Marika.MASSEDIT from MSB AEG099_060 assets."""

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

from massedit_common import OUT_DIR, UNDERGROUND_AREAS, DLC_AREAS, OVERWORLD_AREAS, resolve_location_id, convert_legacy_coords

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


def main():
    MSB_DIR = ERR_MOD_DIR / 'map' / 'MapStudio'

    print("Scanning MSBs for Stakes of Marika (AEG099_060)...")
    stakes = []
    seen = set()

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

        for p in msb.Parts.Assets:
            if str(p.ModelName) != 'AEG099_060':
                continue
            x = round(float(p.Position.X), 3)
            y = round(float(p.Position.Y), 3)
            z = round(float(p.Position.Z), 3)
            key = (area, round(x, 0), round(z, 0))
            if key in seen:
                continue
            seen.add(key)
            stakes.append({
                'area': area, 'gx': gx, 'gz': gz,
                'x': x, 'y': y, 'z': z,
            })

    stakes.sort(key=lambda s: (s['area'], s['gx'], s['gz']))
    print(f"  {len(stakes)} unique stakes")

    # Generate MASSEDIT
    lines = []
    row_id = 7700000
    for s in stakes:
        area = s['area']
        gx = s['gx']
        gz = s['gz']

        if area in UNDERGROUND_AREAS:
            disp = 'dispMask01'
        elif area in DLC_AREAS:
            disp = 'pad2_0'
        else:
            disp = 'dispMask00'

        lines.append(f'param WorldMapPointParam: id {row_id}: iconId: = 405;')
        lines.append(f'param WorldMapPointParam: id {row_id}: {disp}: = 1;')
        lines.append(f'param WorldMapPointParam: id {row_id}: areaNo: = {area};')
        if area in OVERWORLD_AREAS or area in DLC_AREAS or gx > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: gridXNo: = {gx};')
            lines.append(f'param WorldMapPointParam: id {row_id}: gridZNo: = {gz};')
        lines.append(f'param WorldMapPointParam: id {row_id}: posX: = {s["x"]:.3f};')
        if s['y'] != 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: posY: = {s["y"]:.3f};')
        lines.append(f'param WorldMapPointParam: id {row_id}: posZ: = {s["z"]:.3f};')
        # Tutorial text 301540 = "Stakes of Marika"
        lines.append(f'param WorldMapPointParam: id {row_id}: textId1: = 900301540;')
        # Location name for dungeons
        map_code = f'm{area:02d}_{gx:02d}_{gz:02d}_00'
        loc_id = resolve_location_id(map_code)
        if loc_id > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: textId2: = {loc_id};')
        lines.append(f'param WorldMapPointParam: id {row_id}: selectMinZoomStep: = 1;')
        row_id += 1

    out_path = OUT_DIR / 'World - Stakes of Marika.MASSEDIT'
    with open(out_path, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')
    print(f"Written {len(stakes)} entries to {out_path.name}")


if __name__ == '__main__':
    main()
