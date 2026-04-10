#!/usr/bin/env python3
"""
Search ALL MSB files for AEG099_821 (Rune Piece) and AEG099_822 (Ember Piece) assets.
Compare positions with AEG099_510 assets to understand the relationship.
"""
import sys, io, json
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
from pathlib import Path
import config
from pythonnet import load
load('coreclr')
import clr
from System.Reflection import Assembly
from System import Array, Type as SysType, Object

asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
clr.AddReference(str(config.SOULSFORMATS_DLL))
import SoulsFormats

_byte_arr_type = SysType.GetType('System.Byte[]')
_msbe_cls = asm.GetType('SoulsFormats.MSBE')
_msbe_read = _msbe_cls.BaseType.GetMethod('Read', Array[SysType]([_byte_arr_type]))

MAP_DIR = config.require_err_mod_dir() / 'map' / 'MapStudio'
DATA_DIR = config.DATA_DIR

msb_files = sorted(MAP_DIR.glob('*.msb.dcx'))

models_to_find = {'AEG099_821', 'AEG099_822', 'AEG099_510'}
results = {m: [] for m in models_to_find}

print(f"Scanning {len(msb_files)} MSB files for {models_to_find}")

for msb_path in msb_files:
    tile = msb_path.name.split('.')[0]
    # Skip LOD/alternate copies
    parts = tile.split('_')
    if len(parts) == 4 and parts[3] != '00':
        continue
    
    try:
        raw = SoulsFormats.DCX.Decompress(str(msb_path))
        msb = _msbe_read.Invoke(None, Array[Object]([raw]))
    except:
        continue
    
    for parts_type in ['Assets', 'DummyAssets']:
        part_list = getattr(msb.Parts, parts_type, None)
        if part_list is None:
            continue
        for p in part_list:
            model = str(p.ModelName) if hasattr(p, 'ModelName') else ''
            if model not in models_to_find:
                continue
            
            eid = int(p.EntityID) if hasattr(p, 'EntityID') else -1
            pos = (float(p.Position.X), float(p.Position.Y), float(p.Position.Z))
            name = str(p.Name)
            
            egroups = []
            if hasattr(p, 'EntityGroups'):
                egroups = [int(eg) for eg in p.EntityGroups if int(eg) > 0]
            
            results[model].append({
                'map': tile,
                'name': name,
                'entityId': eid,
                'entityGroups': egroups,
                'x': pos[0], 'y': pos[1], 'z': pos[2],
                'partsType': parts_type,
            })

for model in sorted(results.keys()):
    entries = results[model]
    print(f"\n{'='*70}")
    print(f"Model {model}: {len(entries)} instances")
    print(f"{'='*70}")
    for e in entries:
        groups_str = f" groups={e['entityGroups']}" if e['entityGroups'] else ""
        print(f"  {e['map']:>15s} {e['partsType']:>12s} {e['name']:>25s} "
              f"eid={e['entityId']:>12d} pos=({e['x']:>8.1f}, {e['y']:>8.1f}, {e['z']:>8.1f})"
              f"{groups_str}")

# Check proximity: for each AEG099_821/822, find nearest AEG099_510
print(f"\n{'='*70}")
print("Proximity check: AEG099_821/822 vs AEG099_510")
print(f"{'='*70}")

import math
for model in ['AEG099_821', 'AEG099_822']:
    print(f"\n{model}:")
    for piece in results[model]:
        best_dist = float('inf')
        best_510 = None
        for s510 in results['AEG099_510']:
            if s510['map'] != piece['map']:
                continue
            d = math.sqrt((piece['x']-s510['x'])**2 + (piece['y']-s510['y'])**2 + (piece['z']-s510['z'])**2)
            if d < best_dist:
                best_dist = d
                best_510 = s510
        
        if best_510 and best_dist < 50:
            print(f"  {piece['map']} {piece['name']} eid={piece['entityId']} "
                  f"<-> {best_510['name']} eid={best_510['entityId']} "
                  f"dist={best_dist:.1f}")
        else:
            print(f"  {piece['map']} {piece['name']} eid={piece['entityId']} "
                  f"pos=({piece['x']:.1f}, {piece['y']:.1f}, {piece['z']:.1f}) "
                  f"NO NEARBY AEG099_510 (closest={best_dist:.1f})")

# Save
out = {
    'AEG099_821': results['AEG099_821'],
    'AEG099_822': results['AEG099_822'],
    'AEG099_510': results['AEG099_510'],
}
with open(DATA_DIR / '_piece_models.json', 'w', encoding='utf-8') as f:
    json.dump(out, f, indent=2, ensure_ascii=False)
print(f"\nSaved to {DATA_DIR / '_piece_models.json'}")
