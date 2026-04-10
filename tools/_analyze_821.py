#!/usr/bin/env python3
"""
Deep analysis of AEG099_821 (Rune Piece) assets.
Check EntityGroups, all available properties, and look for any hidden tracking mechanism.
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

all_821 = []
eid_set = set()
group_set = set()

print("Scanning for AEG099_821...")
for msb_path in msb_files:
    tile = msb_path.name.split('.')[0]
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
            if model != 'AEG099_821':
                continue
            
            eid = int(p.EntityID) if hasattr(p, 'EntityID') else -1
            pos = (float(p.Position.X), float(p.Position.Y), float(p.Position.Z))
            name = str(p.Name)
            
            egroups = []
            if hasattr(p, 'EntityGroups'):
                egroups = [int(eg) for eg in p.EntityGroups if int(eg) > 0]
                for eg in egroups:
                    group_set.add(eg)
            
            if eid > 0:
                eid_set.add(eid)
            
            # Collect ALL available properties
            props = {}
            for attr_name in ['UnkT00', 'UnkT04', 'UnkT08', 'UnkT0C', 'UnkT10',
                              'UnkT14', 'UnkT18', 'UnkT1C', 'UnkT20', 'UnkT24',
                              'EventFlagID', 'ActivateConditionParamID',
                              'UnkPartField01', 'UnkPartField02', 'ObjActEntityID',
                              'MapStudioLayer', 'UnkE04', 'UnkE09',
                              'LanternID', 'LodParamID', 'UnkE0E',
                              'BreakTerm', 'NetSyncType', 'UnkT02',
                              'CollisionIndex', 'IsShadowSrc', 'IsStaticShadowSrc',
                              'IsCascade3ShadowSrc', 'UnkE04B', 'UnkE04C',
                              'UnkE05', 'UnkE06', 'UnkE07', 'UnkE08']:
                v = getattr(p, attr_name, None)
                if v is not None:
                    try:
                        val = int(v) if not isinstance(v, float) else v
                        if val != 0 and val != -1:
                            props[attr_name] = val
                    except:
                        props[attr_name] = str(v)
            
            entry = {
                'map': tile,
                'name': name,
                'entityId': eid,
                'entityGroups': egroups,
                'x': pos[0], 'y': pos[1], 'z': pos[2],
                'partsType': parts_type,
                'props': props,
            }
            all_821.append(entry)

print(f"\nTotal AEG099_821: {len(all_821)}")
print(f"With EntityID > 0: {len([e for e in all_821 if e['entityId'] > 0])}")
print(f"Unique EntityIDs: {sorted(eid_set)}")
print(f"With EntityGroups: {len([e for e in all_821 if e['entityGroups']])}")
print(f"Unique EntityGroups: {sorted(group_set)[:50]}")

# Show all unique non-zero properties
all_prop_keys = set()
for e in all_821:
    all_prop_keys.update(e['props'].keys())
print(f"\nNon-zero properties found: {sorted(all_prop_keys)}")

# Show property distribution
for prop in sorted(all_prop_keys):
    vals = set()
    for e in all_821:
        v = e['props'].get(prop)
        if v is not None:
            vals.add(v)
    print(f"  {prop}: {len(vals)} unique values: {sorted(vals)[:20]}")

# Show entries with EntityID > 0
print(f"\nEntries with EntityID > 0:")
for e in all_821:
    if e['entityId'] > 0:
        print(f"  {e['map']:>15s} {e['name']:>25s} eid={e['entityId']:>12d} "
              f"groups={e['entityGroups']} props={e['props']} "
              f"pos=({e['x']:.1f}, {e['y']:.1f}, {e['z']:.1f})")

# Show entries with EntityGroups
print(f"\nEntries with EntityGroups:")
for e in all_821:
    if e['entityGroups']:
        print(f"  {e['map']:>15s} {e['name']:>25s} eid={e['entityId']:>12d} "
              f"groups={e['entityGroups']} pos=({e['x']:.1f}, {e['y']:.1f}, {e['z']:.1f})")

# Per-map counts
map_counts = {}
for e in all_821:
    map_counts[e['map']] = map_counts.get(e['map'], 0) + 1
print(f"\nPer-map distribution (top 30):")
for tile, count in sorted(map_counts.items(), key=lambda x: -x[1])[:30]:
    print(f"  {tile}: {count}")

# Save all 821 data for further use
out_path = DATA_DIR / '_rune_pieces_821.json'
with open(out_path, 'w', encoding='utf-8') as f:
    json.dump(all_821, f, indent=2, ensure_ascii=False)
print(f"\nSaved {len(all_821)} entries to {out_path}")
