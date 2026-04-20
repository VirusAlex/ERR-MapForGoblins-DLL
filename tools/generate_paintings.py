#!/usr/bin/env python3
"""Generate World - Paintings.MASSEDIT from EMEVD painting template events + MSB positions."""

import sys
import io
import os
import tempfile
import struct

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
emevd_read = asm.GetType('SoulsFormats.EMEVD').GetMethod('Read',
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

    # Step 1: Find painting events in EMEVD (flags 580000-580199)
    print("Scanning EMEVD for painting events...")
    paintings = []
    seen_flags = set()

    for emevd_path in sorted((ERR_MOD_DIR / 'event').glob('*.emevd.dcx')):
        fname = emevd_path.name.replace('.emevd.dcx', '')
        try:
            emevd = rfb(emevd_read, SoulsFormats.DCX.Decompress(str(emevd_path)), '.emevd')
        except:
            continue
        for ev in emevd.Events:
            if int(ev.ID) != 0:
                continue
            for instr in ev.Instructions:
                bank = int(instr.Bank)
                if bank != 2000:
                    continue
                raw = bytes(instr.ArgData)
                ints = []
                for off in range(0, len(raw) - 3, 4):
                    ints.append(struct.unpack_from('<i', raw, off)[0])
                if len(ints) < 4:
                    continue
                template = ints[1]
                # Template 90005632: params = [0, tmpl, flag, entityId, textId]
                # Template 90005633: params = [0, tmpl, rewardFlag, flag, entityId, ...]
                # Also DLC templates (2045432550 etc.) with same layout as 90005633
                flag = 0
                entity_id = 0
                if template == 90005632 and len(ints) >= 5:
                    flag = ints[2]      # painting collection flag
                    entity_id = ints[3]  # MSB entity
                elif len(ints) >= 5:
                    # 90005633 or DLC: flag at index 3, entity at index 4
                    candidate_flag = ints[3]
                    if 580000 <= candidate_flag <= 580199:
                        flag = candidate_flag
                        entity_id = ints[4]

                if 580000 <= flag <= 580199 and entity_id > 0 and flag not in seen_flags:
                    seen_flags.add(flag)
                    paintings.append({
                        'flag': flag,
                        'entity_id': entity_id,
                        'map_file': fname,
                    })

    print(f"  {len(paintings)} painting events found")

    # Step 2: Build MSB entity index for positions
    print("Building MSB entity index...")
    entity_pos = {}
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
        for ptype in ['Assets', 'DummyAssets', 'Enemies', 'DummyEnemies']:
            parts = getattr(msb.Parts, ptype, None)
            if not parts:
                continue
            for p in parts:
                eid = int(p.EntityID) if hasattr(p, 'EntityID') else 0
                if eid > 0 and eid not in entity_pos:
                    entity_pos[eid] = (
                        area, gx, gz,
                        round(float(p.Position.X), 3),
                        round(float(p.Position.Y), 3),
                        round(float(p.Position.Z), 3),
                    )

    # Step 3: Resolve positions and generate MASSEDIT
    lines = []
    row_id = 7600000
    count = 0
    for p in sorted(paintings, key=lambda p: p['flag']):
        eid = p['entity_id']
        if eid not in entity_pos:
            print(f"  WARNING: entity {eid} not found for flag {p['flag']}")
            continue

        area, gx, gz, x, y, z = entity_pos[eid]

        if area in UNDERGROUND_AREAS:
            disp = 'dispMask01'
        elif area in DLC_AREAS:
            disp = 'pad2_0'
        else:
            disp = 'dispMask00'

        lines.append(f'param WorldMapPointParam: id {row_id}: iconId: = 407;')
        lines.append(f'param WorldMapPointParam: id {row_id}: {disp}: = 1;')
        lines.append(f'param WorldMapPointParam: id {row_id}: areaNo: = {area};')
        if area in OVERWORLD_AREAS or area in DLC_AREAS or gx > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: gridXNo: = {gx};')
            lines.append(f'param WorldMapPointParam: id {row_id}: gridZNo: = {gz};')
        lines.append(f'param WorldMapPointParam: id {row_id}: posX: = {x:.3f};')
        if y != 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: posY: = {y:.3f};')
        lines.append(f'param WorldMapPointParam: id {row_id}: posZ: = {z:.3f};')
        # Painting name from GoodsName FMG
        flag = p['flag']
        if flag >= 580100:
            goods_id = 2008200 + (flag - 580100) // 10
        else:
            goods_id = 8200 + (flag - 580000) // 10
        lines.append(f'param WorldMapPointParam: id {row_id}: textId1: = {500000000 + goods_id};')
        lines.append(f'param WorldMapPointParam: id {row_id}: textDisableFlagId1: = {p["flag"]};')
        # Location text for dungeons
        map_code = f'm{area:02d}_{gx:02d}_{gz:02d}_00'
        loc_id = resolve_location_id(map_code)
        if loc_id > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: textId2: = {loc_id};')
            lines.append(f'param WorldMapPointParam: id {row_id}: textDisableFlagId2: = {p["flag"]};')
        lines.append(f'param WorldMapPointParam: id {row_id}: selectMinZoomStep: = 1;')
        row_id += 1
        count += 1

    out_path = OUT_DIR / 'World - Paintings.MASSEDIT'
    with open(out_path, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')
    print(f"Written {count} entries to {out_path.name}")


if __name__ == '__main__':
    main()
