#!/usr/bin/env python3
"""Generate World - Imp Statues.MASSEDIT from MSB assets with seal entity IDs (suffix 570/575/565/611)."""

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


# Seal types by entity ID suffix
SEAL_SUFFIXES = {570, 575, 565, 611}

# Actual imp statue seal models (the stone imp face you use keys on)
SEAL_MODELS = {'AEG027_078', 'AEG027_079'}


def main():
    MSB_DIR = ERR_MOD_DIR / 'map' / 'MapStudio'

    print("Scanning MSBs for Imp Statue seals...")
    seals = []
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
            eid = int(p.EntityID) if hasattr(p, 'EntityID') else 0
            if eid <= 0:
                continue
            model = str(p.ModelName)
            if model not in SEAL_MODELS:
                continue
            suffix = eid % 1000
            if suffix not in SEAL_SUFFIXES:
                continue

            x = round(float(p.Position.X), 3)
            y = round(float(p.Position.Y), 3)
            z = round(float(p.Position.Z), 3)
            key = (area, eid)
            if key in seen:
                continue
            seen.add(key)
            # Activation flag = tile_base + seal suffix (NOT entity ID)
            if area in (60, 61):
                prefix = 10 if area == 60 else 20
                tile_base = prefix * 100000000 + gx * 1000000 + gz * 10000
            else:
                tile_base = area * 1000000 + gx * 10000
            activation_flag = tile_base + suffix

            seals.append({
                'area': area, 'gx': gx, 'gz': gz,
                'x': x, 'y': y, 'z': z,
                'eid': eid, 'suffix': suffix,
                'flag': activation_flag,
            })

    seals.sort(key=lambda s: (s['area'], s['gx'], s['gz']))
    print(f"  {len(seals)} unique imp statue seals")

    # Deduplicate by position (different assets at same seal location)
    deduped = []
    seen_pos = set()
    for s in seals:
        key = (s['area'], round(s['x'], 0), round(s['z'], 0))
        if key in seen_pos:
            continue
        seen_pos.add(key)
        deduped.append(s)

    print(f"  {len(deduped)} after position dedup")

    # Generate MASSEDIT
    lines = []
    row_id = 7800000
    for s in deduped:
        area = s['area']
        gx = s['gx']
        gz = s['gz']

        if area in UNDERGROUND_AREAS:
            disp = 'dispMask01'
        elif area in DLC_AREAS:
            disp = 'pad2_0'
        else:
            disp = 'dispMask00'

        lines.append(f'param WorldMapPointParam: id {row_id}: iconId: = 369;')
        lines.append(f'param WorldMapPointParam: id {row_id}: {disp}: = 1;')
        lines.append(f'param WorldMapPointParam: id {row_id}: areaNo: = {area};')
        if area in OVERWORLD_AREAS or area in DLC_AREAS or gx > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: gridXNo: = {gx};')
            lines.append(f'param WorldMapPointParam: id {row_id}: gridZNo: = {gz};')
        lines.append(f'param WorldMapPointParam: id {row_id}: posX: = {s["x"]:.3f};')
        if s['y'] != 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: posY: = {s["y"]:.3f};')
        lines.append(f'param WorldMapPointParam: id {row_id}: posZ: = {s["z"]:.3f};')
        # Key type as first line: Stonesword Key or Imbued Sword Key
        if s['suffix'] == 565:
            lines.append(f'param WorldMapPointParam: id {row_id}: textId1: = 500008186;')  # Imbued Sword Key
        else:
            lines.append(f'param WorldMapPointParam: id {row_id}: textId1: = 500008000;')  # Stonesword Key
        # Seal unlock flag = entity ID
        lines.append(f'param WorldMapPointParam: id {row_id}: textDisableFlagId1: = {s["flag"]};')
        # Location name for dungeons (second line)
        map_code = f'm{area:02d}_{gx:02d}_{gz:02d}_00'
        loc_id = resolve_location_id(map_code)
        if loc_id > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: textId2: = {loc_id};')
            lines.append(f'param WorldMapPointParam: id {row_id}: textDisableFlagId2: = {s["flag"]};')
        lines.append(f'param WorldMapPointParam: id {row_id}: selectMinZoomStep: = 1;')
        row_id += 1

    out_path = OUT_DIR / 'World - Imp Statues.MASSEDIT'
    with open(out_path, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')
    print(f"Written {len(deduped)} entries to {out_path.name}")


if __name__ == '__main__':
    main()
