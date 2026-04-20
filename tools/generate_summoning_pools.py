#!/usr/bin/env python3
"""Generate World - Summoning Pools.MASSEDIT from SignPuddleParam + MSB position matching."""

import sys
import io
import os
import tempfile
import math

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

from extract_all_items import load_paramdefs, read_param, param_to_dict
from massedit_common import OUT_DIR, UNDERGROUND_AREAS, DLC_AREAS, OVERWORLD_AREAS, resolve_location_id

ERR_MOD_DIR = config.require_err_mod_dir()
bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(ERR_MOD_DIR / 'regulation.bin'))
paramdefs = load_paramdefs()

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
    # Step 1: Read SignPuddleParam for positions and flags
    print("Loading SignPuddleParam...")
    sp = read_param(bnd, 'SignPuddleParam', paramdefs)
    sp_fields = {'unknown_0x24', 'unknown_0x28',
                 'unknown_0x2c', 'unknown_0x30', 'unknown_0x34',
                 'unknown_0x3c'}
    sp_data = param_to_dict(sp, sp_fields)

    pools = []
    for rid, row in sorted(sp_data.items()):
        if rid == 0:
            continue
        flag = int(row.get('unknown_0x3c', 0))
        map_ref = int(row.get('unknown_0x28', 0))
        x = round(float(row.get('unknown_0x2c', 0)), 3)
        y = round(float(row.get('unknown_0x30', 0)), 3)
        z = round(float(row.get('unknown_0x34', 0)), 3)

        # Decode dungeon map_ref: area + block*256
        if map_ref < 100000:
            area = map_ref % 256
            block = map_ref // 256
        else:
            area = 0  # overworld — will resolve via MSB
            block = 0

        pools.append({
            'rid': rid, 'flag': flag, 'map_ref': map_ref,
            'x': x, 'y': y, 'z': z,
            'area': area, 'block': block,
            'gx': 0, 'gz': 0, 'resolved': area > 0,
        })

    print(f"  {len(pools)} pools from SignPuddleParam")

    # Step 2: Scan MSB for AEG099_015 to resolve overworld tiles
    print("Scanning MSBs for AEG099_015 positions...")
    MSB_DIR = ERR_MOD_DIR / 'map' / 'MapStudio'
    msb_pools = []  # (area, gx, gz, x, y, z, map_name)
    seen_pos = set()

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
            if str(p.ModelName) != 'AEG099_015':
                continue
            mx = round(float(p.Position.X), 3)
            mz = round(float(p.Position.Z), 3)
            key = (area, round(mx, 0), round(mz, 0))
            if key in seen_pos:
                continue
            seen_pos.add(key)
            msb_pools.append((area, gx, gz, mx, float(p.Position.Y), mz, map_name))

    print(f"  {len(msb_pools)} unique MSB positions")

    # Step 3: Match unresolved pools (overworld) to MSB by position
    unresolved = [p for p in pools if not p['resolved']]
    resolved_count = 0
    for pool in unresolved:
        best_dist = 999
        best_match = None
        for area, gx, gz, mx, my, mz, map_name in msb_pools:
            dx = pool['x'] - mx
            dz = pool['z'] - mz
            dist = math.sqrt(dx*dx + dz*dz)
            if dist < best_dist:
                best_dist = dist
                best_match = (area, gx, gz)
        if best_match and best_dist < 10:
            pool['area'] = best_match[0]
            pool['gx'] = best_match[1]
            pool['gz'] = best_match[2]
            pool['resolved'] = True
            resolved_count += 1

    # For dungeon pools, set gx from block
    for pool in pools:
        if pool['area'] > 0 and pool['area'] not in (60, 61) and pool['block'] > 0:
            pool['gx'] = pool['block']

    still_unresolved = [p for p in pools if not p['resolved']]
    print(f"  Resolved {resolved_count} overworld pools via MSB, {len(still_unresolved)} unresolved")
    for p in still_unresolved:
        print(f"    row={p['rid']} map_ref={p['map_ref']} pos=({p['x']},{p['z']})")

    # Step 4: Deduplicate pools at near-identical positions.
    # SignPuddleParam has separate rows per matchAreaId (e.g. 670105 + 670410
    # for the same physical pool in different game contexts). Keep the first.
    resolved = [p for p in pools if p['resolved']]
    deduped = []
    for pool in resolved:
        is_dupe = any(
            pool['area'] == q['area'] and pool['gx'] == q['gx'] and pool['gz'] == q['gz']
            and abs(pool['x'] - q['x']) < 3 and abs(pool['z'] - q['z']) < 3
            for q in deduped
        )
        if is_dupe: continue
        deduped.append(pool)
    dropped = len(resolved) - len(deduped)
    if dropped:
        print(f"  Deduplicated {dropped} overlapping pool(s)")

    # Step 5: Generate MASSEDIT
    lines = []
    row_id = 8700000
    count = 0
    for pool in deduped:
        area = pool['area']
        gx = pool['gx']
        gz = pool['gz']

        if area in UNDERGROUND_AREAS:
            disp = 'dispMask01'
        elif area in DLC_AREAS:
            disp = 'pad2_0'
        else:
            disp = 'dispMask00'

        lines.append(f'param WorldMapPointParam: id {row_id}: iconId: = 394;')
        lines.append(f'param WorldMapPointParam: id {row_id}: {disp}: = 1;')
        lines.append(f'param WorldMapPointParam: id {row_id}: areaNo: = {area};')
        if area in OVERWORLD_AREAS or area in DLC_AREAS or gx > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: gridXNo: = {gx};')
            lines.append(f'param WorldMapPointParam: id {row_id}: gridZNo: = {gz};')
        lines.append(f'param WorldMapPointParam: id {row_id}: posX: = {pool["x"]:.3f};')
        if pool['y'] != 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: posY: = {pool["y"]:.3f};')
        lines.append(f'param WorldMapPointParam: id {row_id}: posZ: = {pool["z"]:.3f};')
        # Use SignPuddleParam row ID (670XXX) as activation flag — EMEVD sets these
        # textDisableFlagId1 hides text when pool activated → engine hides icon without text
        if pool['rid'] > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: textDisableFlagId1: = {pool["rid"]};')
        lines.append(f'param WorldMapPointParam: id {row_id}: textId1: = 900301690;')
        map_code = f'm{pool["area"]:02d}_{pool["gx"]:02d}_{pool["gz"]:02d}_00'
        loc_id = resolve_location_id(map_code)
        if loc_id > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: textId2: = {loc_id};')
            if pool['rid'] > 0:
                lines.append(f'param WorldMapPointParam: id {row_id}: textDisableFlagId2: = {pool["rid"]};')
        lines.append(f'param WorldMapPointParam: id {row_id}: selectMinZoomStep: = 1;')
        row_id += 1
        count += 1

    out_path = OUT_DIR / 'World - Summoning Pools.MASSEDIT'
    with open(out_path, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')
    print(f"Written {count} entries to {out_path.name}")


if __name__ == '__main__':
    main()
