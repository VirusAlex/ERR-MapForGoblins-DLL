#!/usr/bin/env python3
"""Build data/grace_position_index.json from BonfireWarpParam.

Each grace gives an authoritative `(areaNo, gridX, gridZ, x, y, z, subCategoryId,
subRegion, majorRegion)` anchor for location-name resolution. Used by
`massedit_common.resolve_location_id_at()` to give per-marker correct subtitles
in dungeon tiles that physically contain multiple regions (e.g. m12_02 / m12_07
where Nokron sits stacked above Siofra River - both regions appear in the same
MSB tile).
"""
import sys
import io
import os
import re
import json
import tempfile
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')

import config
from pythonnet import load as _pyload
_pyload('coreclr')
from System.Reflection import Assembly
from System import Array, Type as SysType, Object
from System.IO import File as SysFile

asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
import SoulsFormats

_str = SysType.GetType('System.String')
_pcls = asm.GetType('SoulsFormats.PARAM')
_pr = _pcls.BaseType.GetMethod('Read', Array[SysType]([_str]))


def load_param(bnd, name):
    for f in bnd.Files:
        if name in str(f.Name):
            tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_p.param')
            SysFile.WriteAllBytes(tmp, f.Bytes.ToArray())
            p = _pr.Invoke(None, Array[Object]([tmp]))
            pdef = defs.get(str(p.ParamType))
            if pdef:
                p.ApplyParamdef(pdef)
            return p
    return None


defs = {}
for xml in config.PARAMDEF_DIR.glob('*.xml'):
    try:
        d = SoulsFormats.PARAMDEF.XmlDeserialize(str(xml), False)
        if d and d.ParamType:
            defs[str(d.ParamType)] = d
    except Exception:
        pass

bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(config.ERR_MOD_DIR / 'regulation.bin'))

# PlaceName text lookup - load from FMG (item_dlc02.msgbnd: slots 19/329/429)
DATA_DIR = config.DATA_DIR  # data/ or data/vanilla/ per profile
_fcls = asm.GetType('SoulsFormats.FMG')
_fr = _fcls.BaseType.GetMethod('Read', Array[SysType]([_str]))
_bcls = asm.GetType('SoulsFormats.BND4')
_br = _bcls.BaseType.GetMethod('Read', Array[SysType]([_str]))

place_names = {}
msg_bnd = _br.Invoke(None, Array[Object]([str(config.ERR_MOD_DIR / 'msg/engus/item_dlc02.msgbnd.dcx')]))
for f in msg_bnd.Files:
    nm = str(f.Name)
    if 'PlaceName' not in nm or 'Tutorial' in nm:
        continue
    tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_pn.fmg')
    SysFile.WriteAllBytes(tmp, f.Bytes.ToArray())
    fmg = _fr.Invoke(None, Array[Object]([tmp]))
    for e in fmg.Entries:
        t = str(e.Text) if e.Text else ''
        if t and t != '[ERROR]':
            place_names.setdefault(int(e.ID), re.sub(r'<[^>]+>', '', t))

# Build BonfireWarpSubCategoryParam: row_id -> (subRegion text, tabId)
subcat = load_param(bnd, 'BonfireWarpSubCategoryParam')
sub_info = {}
for r in subcat.Rows:
    rid = int(r.ID)
    tab_id = None
    for c in r.Cells:
        if str(c.Def.InternalName) == 'tabId':
            try:
                tab_id = int(c.Value)
            except Exception:
                pass
            break
    sub_info[rid] = {
        'subCategoryId': rid,
        'tabId': tab_id,
        'subRegion': place_names.get(rid),
        'majorRegion': place_names.get(tab_id) if tab_id is not None else None,
    }

# Walk BonfireWarpParam → per-grace position + region metadata
bonfire = load_param(bnd, 'BonfireWarpParam')
graces = []
for r in bonfire.Rows:
    area = gx = gz = sc = None
    px = py = pz = 0.0
    for c in r.Cells:
        nm = str(c.Def.InternalName)
        try:
            v = int(c.Value)
        except Exception:
            try:
                v = float(c.Value)
            except Exception:
                continue
        if nm == 'areaNo':
            area = v
        elif nm == 'gridXNo':
            gx = v
        elif nm == 'gridZNo':
            gz = v
        elif nm == 'bonfireSubCategoryId':
            sc = v
        elif nm == 'posX':
            px = float(v)
        elif nm == 'posY':
            py = float(v)
        elif nm == 'posZ':
            pz = float(v)
    if area is None or area < 0 or sc is None:
        continue
    info = sub_info.get(sc, {})
    graces.append({
        'areaNo': area, 'gridX': gx, 'gridZ': gz,
        'x': px, 'y': py, 'z': pz,
        'subCategoryId': sc,
        'subRegion': info.get('subRegion'),
        'tabId': info.get('tabId'),
        'majorRegion': info.get('majorRegion'),
    })

out_path = DATA_DIR / 'grace_position_index.json'
with open(out_path, 'w', encoding='utf-8') as f:
    json.dump(graces, f, ensure_ascii=False, indent=1)
print(f"Saved {len(graces)} grace positions to {out_path}")
