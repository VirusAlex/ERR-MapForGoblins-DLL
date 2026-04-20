#!/usr/bin/env python3
"""
Generate World - Hostile NPC.MASSEDIT — fully auto-discovered.

Strategy:
  1. From NpcParam (regulation.bin), collect NPC IDs with teamType=24
     (Invader team) — these are hostile NPC invaders.
  2. Scan all MSBs for Enemies whose NPCParamID is in that set AND
     whose EntityID > 0 (placed, not script-spawned dummies).
  3. Each matched enemy becomes a map marker at its MSB position.

No hardcoded lists, no orig-MASSEDIT dependency.
"""
import sys, io, os, tempfile, json
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
_param_read = asm.GetType('SoulsFormats.PARAM').GetMethod('Read',
    BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str_type]), None)
_msbe_read = asm.GetType('SoulsFormats.MSBE').GetMethod('Read',
    BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str_type]), None)


def load_paramdefs():
    defs = {}
    for x in config.PARAMDEF_DIR.glob('*.xml'):
        try:
            pd = SoulsFormats.PARAMDEF.XmlDeserialize(str(x), False)
            if pd and pd.ParamType:
                defs[str(pd.ParamType)] = pd
        except Exception:
            pass
    return defs


def read_param(bnd, name, paramdefs):
    for f in bnd.Files:
        if name in str(f.Name):
            tmp = os.path.join(tempfile.gettempdir(), '_hnp_p.tmp')
            SysFile.WriteAllBytes(tmp, f.Bytes.ToArray())
            p = _param_read.Invoke(None, Array[Object]([tmp]))
            os.unlink(tmp)
            pt = str(p.ParamType) if p.ParamType else ''
            if pt in paramdefs:
                p.ApplyParamdef(paramdefs[pt])
            return p
    return None


def read_msb(path):
    tmp = os.path.join(tempfile.gettempdir(), '_hnp_m.tmp')
    SysFile.WriteAllBytes(tmp, SoulsFormats.DCX.Decompress(str(path)).ToArray())
    m = _msbe_read.Invoke(None, Array[Object]([tmp]))
    os.unlink(tmp)
    return m


def get_disp_mask(area):
    if area in UNDERGROUND_AREAS: return 'dispMask01'
    if area in DLC_AREAS: return 'pad2_0'
    return 'dispMask00'


def map_to_area(map_name):
    parts = map_name.replace('m', '').split('_')
    try: return int(parts[0]), int(parts[1]), int(parts[2])
    except: return 0, 0, 0


INVADER_TEAM_TYPE = 24


def main():
    print('Loading NpcParam (invader team filter)...')
    pds = load_paramdefs()
    bnd = SoulsFormats.SFUtil.DecryptERRegulation(
        str(config.require_err_mod_dir() / 'regulation.bin'))
    np = read_param(bnd, 'NpcParam', pds)

    invader_ids = set()
    for row in np.Rows:
        if not row.Cells: continue
        for cell in row.Cells:
            if str(cell.Def.InternalName) == 'teamType':
                try:
                    if int(str(cell.Value)) == INVADER_TEAM_TYPE:
                        invader_ids.add(int(row.ID))
                except Exception: pass
                break
    print(f'  {len(invader_ids)} NpcParam IDs with teamType={INVADER_TEAM_TYPE}')

    msb_dir = config.require_err_mod_dir() / 'map' / 'MapStudio'
    records = []
    print(f'Scanning MSBs for invader placements...')
    for msb_path in sorted(msb_dir.glob('*.msb.dcx')):
        try: msb = read_msb(msb_path)
        except Exception: continue
        map_name = msb_path.name.replace('.msb.dcx', '')
        for e in msb.Parts.Enemies:
            npc = getattr(e, 'NPCParamID', 0)
            if npc not in invader_ids: continue
            entity = getattr(e, 'EntityID', 0)
            if not entity or entity <= 0:
                continue  # runtime-only dummy
            pos = e.Position
            area, gx, gz = map_to_area(map_name)
            records.append({
                'entity': entity, 'npc': npc,
                'map': map_name, 'area': area, 'gx': gx, 'gz': gz,
                'x': float(pos.X), 'y': float(pos.Y), 'z': float(pos.Z),
                'model': e.ModelName or '',
            })

    # Deduplicate per (map, rounded_coords) — prevents cluster-spam when
    # multiple invader variants are stacked in same spot for EMEVD triggers
    seen = set()
    uniq = []
    for r in records:
        key = (r['map'], round(r['x'], 1), round(r['z'], 1))
        if key in seen: continue
        seen.add(key)
        uniq.append(r)
    records = uniq
    records.sort(key=lambda r: (r['area'], r['gx'], r['gz'], r['x'], r['z']))

    lines = []
    row_id = 9200000
    for r in records:
        disp = get_disp_mask(r['area'])
        lines.append(f'param WorldMapPointParam: id {row_id}: iconId: = 373;')
        lines.append(f'param WorldMapPointParam: id {row_id}: {disp}: = 1;')
        lines.append(f'param WorldMapPointParam: id {row_id}: areaNo: = {r["area"]};')
        if r['gx'] > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: gridXNo: = {r["gx"]};')
        if r['gz'] > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: gridZNo: = {r["gz"]};')
        lines.append(f'param WorldMapPointParam: id {row_id}: posX: = {r["x"]:.3f};')
        lines.append(f'param WorldMapPointParam: id {row_id}: posY: = {r["y"]:.3f};')
        lines.append(f'param WorldMapPointParam: id {row_id}: posZ: = {r["z"]:.3f};')
        lines.append(f'param WorldMapPointParam: id {row_id}: selectMinZoomStep: = 1;')
        row_id += 1

    out = OUT_DIR / 'World - Hostile NPC.MASSEDIT'
    with open(out, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')
    print(f'Written {len(records)} invader entries to {out.name}')


if __name__ == '__main__':
    main()
