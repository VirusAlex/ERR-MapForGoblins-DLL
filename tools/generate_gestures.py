#!/usr/bin/env python3
"""
Generate Loot - Gestures.MASSEDIT — auto-discovered via gesture template.

Common event 90005570 is ER's gesture-spawn template: it sets a gesture
flag when player interacts with a specific asset. We scan Event 0 across
all EMEVDs for RunEvent/RunCommonEvent calls targeting 90005570 and
extract (flag, entity_id) from args.

Calls use args pattern [0, 90005570, flag, gesture_param, entity_id, ...].
Entity ID is then resolved to MSB position via entity index.

Map-specific gesture templates (e.g. m16 event 16003762, m18 chest chain)
are not covered by this generator — they'd need per-map heuristics which
conflict with clean auto-generation.
"""
import sys, io, os, tempfile, struct, json
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import config
from pathlib import Path
from pythonnet import load
load('coreclr')
import clr
clr.AddReference(str(config.SOULSFORMATS_DLL))
from System.Reflection import Assembly, BindingFlags
from System import Array, Type as SysType, Object
from System.IO import File as SysFile
import SoulsFormats

from massedit_common import OUT_DIR, UNDERGROUND_AREAS, DLC_AREAS

asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
_str_type = SysType.GetType('System.String')
_emevd_read = asm.GetType('SoulsFormats.EMEVD').GetMethod('Read',
    BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str_type]), None)

GESTURE_TEMPLATE = 90005570


def load_emevd(path):
    tmp = os.path.join(tempfile.gettempdir(), '_gst.tmp')
    SysFile.WriteAllBytes(tmp, SoulsFormats.DCX.Decompress(str(path)).ToArray())
    e = _emevd_read.Invoke(None, Array[Object]([tmp]))
    os.unlink(tmp)
    return e


def get_disp_mask(area):
    if area in UNDERGROUND_AREAS: return 'dispMask01'
    if area in DLC_AREAS: return 'pad2_0'
    return 'dispMask00'


def map_to_area(map_name):
    parts = map_name.replace('m', '').split('_')
    try: return int(parts[0]), int(parts[1]), int(parts[2])
    except: return 0, 0, 0


def main():
    ei = json.load(open(config.DATA_DIR / 'msb_entity_index.json', encoding='utf-8'))
    ent_set = {int(k): v for k, v in ei.items()}

    records = []
    for p in sorted((config.require_err_mod_dir() / 'event').glob('*.emevd.dcx')):
        try: em = load_emevd(p)
        except Exception: continue
        map_name = p.name.replace('.emevd.dcx', '')
        for evt in em.Events:
            if int(evt.ID) != 0: continue
            for inst in evt.Instructions:
                bank, idx = int(inst.Bank), int(inst.ID)
                if bank != 2000 or idx not in (0, 6): continue
                ab = bytes(inst.ArgData) if inst.ArgData else b''
                if len(ab) < 8: continue
                called = struct.unpack_from('<I', ab, 4)[0]
                if called != GESTURE_TEMPLATE: continue
                # args start after 8-byte header (slot + event_id)
                vals = [struct.unpack_from('<I', ab, i)[0]
                        for i in range(8, len(ab) - 3, 4)]
                # Expected layout: [flag, gesture_param, entity_id, ...]
                if len(vals) < 3: continue
                flag, _param, entity = vals[0], vals[1], vals[2]
                info = ent_set.get(entity)
                if not info or info['map'] != map_name:
                    continue
                area, gx, gz = map_to_area(map_name)
                records.append({
                    'flag': flag,
                    'map': map_name, 'area': area, 'gx': gx, 'gz': gz,
                    'x': info['x'], 'y': info['y'], 'z': info['z'],
                    'entity': entity, 'model': info['model'],
                })

    # Dedup by (map, entity)
    seen = set()
    uniq = []
    for r in records:
        k = (r['map'], r['entity'])
        if k in seen: continue
        seen.add(k)
        uniq.append(r)
    uniq.sort(key=lambda r: (r['area'], r['gx'], r['gz'], r['x'], r['z']))

    lines = []
    row_id = 4900000
    for r in uniq:
        disp = get_disp_mask(r['area'])
        lines.append(f'param WorldMapPointParam: id {row_id}: iconId: = 417;')
        lines.append(f'param WorldMapPointParam: id {row_id}: {disp}: = 1;')
        lines.append(f'param WorldMapPointParam: id {row_id}: areaNo: = {r["area"]};')
        if r['gx'] > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: gridXNo: = {r["gx"]};')
        if r['gz'] > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: gridZNo: = {r["gz"]};')
        lines.append(f'param WorldMapPointParam: id {row_id}: posX: = {r["x"]:.3f};')
        lines.append(f'param WorldMapPointParam: id {row_id}: posY: = {r["y"]:.3f};')
        lines.append(f'param WorldMapPointParam: id {row_id}: posZ: = {r["z"]:.3f};')
        lines.append(f'param WorldMapPointParam: id {row_id}: textDisableFlagId1: = {r["flag"]};')
        lines.append(f'param WorldMapPointParam: id {row_id}: selectMinZoomStep: = 1;')
        row_id += 1

    out = OUT_DIR / 'Loot - Gestures.MASSEDIT'
    with open(out, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')
    print(f'Written {len(uniq)} gesture entries to {out.name}')
    for r in uniq:
        print(f'  flag={r["flag"]} map={r["map"]} pos=({r["x"]:.2f},{r["y"]:.2f},{r["z"]:.2f}) model={r["model"]}')


if __name__ == '__main__':
    main()
