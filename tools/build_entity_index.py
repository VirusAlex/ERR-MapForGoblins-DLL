#!/usr/bin/env python3
"""
Build MSB entity index: EntityID -> (map, position, model, kind, name).
Scans all MSB files in ERR mod and emits data/msb_entity_index.json.
Used by EMEVD parser to resolve scripted item drops to positions.
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

asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
_str_type = SysType.GetType('System.String')
_msbe_read = asm.GetType('SoulsFormats.MSBE').GetMethod('Read',
    BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str_type]), None)

def load_msb(path):
    data = SoulsFormats.DCX.Decompress(str(path)).ToArray()
    tmp = os.path.join(tempfile.gettempdir(), '_mfg_msb.msb')
    SysFile.WriteAllBytes(tmp, data)
    m = _msbe_read.Invoke(None, Array[Object]([tmp]))
    os.unlink(tmp)
    return m

MSB_DIR = config.require_err_mod_dir() / 'map' / 'MapStudio'
out = {}
msb_files = sorted(MSB_DIR.glob('*.msb.dcx'))
print(f'Scanning {len(msb_files)} MSBs...')

for i, p in enumerate(msb_files):
    if (i + 1) % 100 == 0:
        print(f'  [{i+1}/{len(msb_files)}]')
    try:
        msb = load_msb(p)
    except Exception as e:
        continue
    map_name = p.name.replace('.msb.dcx', '')
    for kind, col in [('enemy', msb.Parts.Enemies),
                      ('asset', msb.Parts.Assets),
                      ('dummy_asset', msb.Parts.DummyAssets)]:
        for part in col:
            ent = getattr(part, 'EntityID', 0)
            if ent and ent > 0:
                pos = part.Position
                rec = {
                    'map': map_name,
                    'x': float(pos.X), 'y': float(pos.Y), 'z': float(pos.Z),
                    'model': str(part.ModelName or ''),
                    'name': str(part.Name or ''),
                    'kind': kind,
                }
                # First occurrence wins if duplicate
                if ent not in out:
                    out[ent] = rec

DATA = config.DATA_DIR
path_out = DATA / 'msb_entity_index.json'
with open(path_out, 'w', encoding='utf-8') as f:
    json.dump({str(k): v for k, v in out.items()}, f, indent=1)
print(f'Saved {len(out)} entities -> {path_out}')
