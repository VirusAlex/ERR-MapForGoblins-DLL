#!/usr/bin/env python3
"""Bootstrap two committed data files from the active profile's regulation/FMG.

Produces, into config.DATA_DIR (data/ for err, data/vanilla/ for vanilla):
  - WorldMapLegacyConvParam.json : dungeon->overworld coord conversion table
        (consumed by massedit_common.convert_legacy_coords and
         generate_data.generate_legacy_conv_cpp). Without it, legacy-dungeon
         markers get raw local coords instead of overworld positions.
  - valid_location_ids.json      : the set of PlaceName FMG ids that actually
        resolve to a string (consumed by massedit_common.resolve_location_id*
        as the dungeon-subtitle fallback). Picking an id absent from the FMG
        is exactly the null-PlaceName crash risk, so this must be per-game.

For the err profile these files already ship committed (this script reproduces
them); the vanilla profile has no committed copies, so this is its source.
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
_fcls = asm.GetType('SoulsFormats.FMG')
_fr = _fcls.BaseType.GetMethod('Read', Array[SysType]([_str]))
_bcls = asm.GetType('SoulsFormats.BND4')
_br = _bcls.BaseType.GetMethod('Read', Array[SysType]([_str]))

SRC = config.require_err_mod_dir()  # profile-aware: ERR mod or vanilla game
OUT = config.DATA_DIR
OUT.mkdir(parents=True, exist_ok=True)

# ── paramdefs ──
defs = {}
for xml in config.PARAMDEF_DIR.glob('*.xml'):
    try:
        d = SoulsFormats.PARAMDEF.XmlDeserialize(str(xml), False)
        if d and d.ParamType:
            defs[str(d.ParamType)] = d
    except Exception:
        pass


def load_param(bnd, name):
    for f in bnd.Files:
        if name in str(f.Name):
            tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_pbp.param')
            SysFile.WriteAllBytes(tmp, f.Bytes.ToArray())
            p = _pr.Invoke(None, Array[Object]([tmp]))
            pdef = defs.get(str(p.ParamType))
            if pdef:
                p.ApplyParamdef(pdef)
            return p
    return None


def _cell_val(v):
    """Convert a .NET cell value to a JSON-friendly python scalar."""
    if isinstance(v, bool):
        return v
    try:
        fv = float(v)
        return fv
    except Exception:
        return str(v) if v is not None else None


# ── WorldMapLegacyConvParam ──
bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(SRC / 'regulation.bin'))
conv = load_param(bnd, 'WorldMapLegacyConvParam')
conv_rows = []
if conv is not None:
    for r in conv.Rows:
        entry = {}
        if r.Cells:
            for c in r.Cells:
                entry[str(c.Def.InternalName)] = _cell_val(c.Value)
        entry['rowId'] = int(r.ID)
        conv_rows.append(entry)
    with open(OUT / 'WorldMapLegacyConvParam.json', 'w', encoding='utf-8') as f:
        json.dump(conv_rows, f, indent=2)
    print(f"wrote {len(conv_rows)} rows -> {OUT / 'WorldMapLegacyConvParam.json'}")
else:
    print("WARNING: WorldMapLegacyConvParam not found in regulation")

# ── valid_location_ids from PlaceName FMG ──
place_ids = set()
msg_path = SRC / 'msg' / 'engus' / 'item_dlc02.msgbnd.dcx'
if not msg_path.exists():
    # vanilla without DLC layout - fall back to the base item msgbnd
    alt = SRC / 'msg' / 'engus' / 'item.msgbnd.dcx'
    if alt.exists():
        msg_path = alt
msg_bnd = _br.Invoke(None, Array[Object]([str(msg_path)]))
for f in msg_bnd.Files:
    nm = str(f.Name)
    if 'PlaceName' not in nm or 'Tutorial' in nm:
        continue
    tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_pbp.fmg')
    SysFile.WriteAllBytes(tmp, f.Bytes.ToArray())
    fmg = _fr.Invoke(None, Array[Object]([tmp]))
    for e in fmg.Entries:
        t = str(e.Text) if e.Text else ''
        if t and t != '[ERROR]':
            place_ids.add(int(e.ID))
with open(OUT / 'valid_location_ids.json', 'w', encoding='utf-8') as f:
    json.dump(sorted(place_ids), f)
print(f"wrote {len(place_ids)} ids -> {OUT / 'valid_location_ids.json'}")
