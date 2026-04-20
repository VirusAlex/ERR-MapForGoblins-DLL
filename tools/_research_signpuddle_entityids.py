#!/usr/bin/env python3
"""Decode unknown_0x28 in SignPuddleParam - check if these are EntityGroupIDs."""

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

from extract_all_items import load_paramdefs, read_param

ERR_MOD_DIR = config.require_err_mod_dir()

print("=== Loading regulation.bin ===")
bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(ERR_MOD_DIR / 'regulation.bin'))
paramdefs = load_paramdefs()

sp = read_param(bnd, 'SignPuddleParam', paramdefs)

# Collect all unknown_0x28 values
u28_values = {}
for row in sp.Rows:
    rid = int(row.ID)
    if rid == 0:
        continue
    vals = {}
    for cell in row.Cells:
        fn = str(cell.Def.InternalName)
        vals[fn] = str(cell.Value)
    u28 = int(vals.get('unknown_0x28', '0'))
    u28_values[rid] = u28

print(f"SignPuddleParam entries with u28: {len(u28_values)}")

# The u28 values fall into two patterns:
# Small values (10-45): these look like area numbers
# Larger values: could be entityIDs
small_vals = {k: v for k, v in u28_values.items() if v < 100}
large_vals = {k: v for k, v in u28_values.items() if v >= 100}
print(f"  Small (<100): {len(small_vals)} -- values: {sorted(set(small_vals.values()))}")
print(f"  Large (>=100): {len(large_vals)}")

# The large values look like they encode map tile + entity
# Let's check: are they entityIDs in the format used by ERR?
# In ERR, open-world entity IDs for m60 are typically 10XXYYSSSS where XX=gridX, YY=gridZ
# Let's try to decode them
print("\nDecoding large u28 values:")
for rid in sorted(large_vals.keys())[:30]:
    v = large_vals[rid]
    s = str(v)
    print(f"  SP {rid}: u28={v} (len={len(s)})")

# Now scan ALL MSB AEG099_015 assets to get their EntityGroupIDs
print("\n=== Scanning MSBs for AEG099_015 assets (EntityID + EntityGroupIDs) ===")

MSB_DIR = ERR_MOD_DIR / 'map' / 'MapStudio'
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

# Collect u28 value set for matching
u28_set = set(u28_values.values())

pools = []
entity_to_pool = {}
group_to_pool = {}
all_015_data = []

for msb_path in sorted(MSB_DIR.glob('*.msb.dcx')):
    map_name = msb_path.name.replace('.msb.dcx', '')
    try:
        msb = rfb(_msbe_read, SoulsFormats.DCX.Decompress(str(msb_path)), '.msb')
    except:
        continue

    for p in msb.Parts.Assets:
        model = str(p.ModelName)
        if model != 'AEG099_015':
            continue

        eid = int(p.EntityID)
        name = str(p.Name)
        x = float(p.Position.X)
        y = float(p.Position.Y)
        z = float(p.Position.Z)

        # Get entity group IDs
        groups = []
        if hasattr(p, 'EntityGroupIDs'):
            for g in p.EntityGroupIDs:
                gv = int(g)
                if gv > 0:
                    groups.append(gv)

        info = {
            'map': map_name,
            'name': name,
            'eid': eid,
            'groups': groups,
            'x': x, 'y': y, 'z': z,
        }
        all_015_data.append(info)

        if eid > 0:
            entity_to_pool[eid] = info
        for g in groups:
            group_to_pool[g] = info

        # Check if entity ID or any group ID matches u28
        if eid in u28_set:
            pools.append(('entityID', eid, info))
        for g in groups:
            if g in u28_set:
                pools.append(('groupID', g, info))

print(f"\nTotal AEG099_015 assets found: {len(all_015_data)}")
print(f"Matches via entityID: {sum(1 for t in pools if t[0] == 'entityID')}")
print(f"Matches via groupID: {sum(1 for t in pools if t[0] == 'groupID')}")

if pools:
    print("\nMatched pools:")
    for match_type, match_val, info in pools[:30]:
        print(f"  {match_type}={match_val}: {info['name']} in {info['map']} eid={info['eid']} groups={info['groups']}")

# Show some AEG099_015 data
print(f"\nSample AEG099_015 assets (first 30):")
for info in all_015_data[:30]:
    print(f"  {info['name']} in {info['map']}: eid={info['eid']}, groups={info['groups']}, pos=({info['x']:.1f}, {info['y']:.1f}, {info['z']:.1f})")

# Check the small u28 values - are these dungeon area numbers?
# For small u28 values, the row IDs seem to follow a pattern with the area
print("\n=== Analyzing small u28 values (area numbers?) ===")
for rid in sorted(small_vals.keys()):
    v = small_vals[rid]
    # Get the full row data
    for row in sp.Rows:
        if int(row.ID) == rid:
            vals = {}
            for cell in row.Cells:
                fn = str(cell.Def.InternalName)
                vs = str(cell.Value)
                if 'System.Byte[]' not in vs and vs != '0' and vs != '0.0':
                    vals[fn] = vs
            print(f"  SP {rid}: u28={v} -> area {v}? data: {vals}")
            break

# For the overworld pools, u28 looks like encoded values. Let me try:
# The generate_summoning_pools.py gets positions from MSBs. Let's match
# SignPuddleParam coords to MSB AEG099_015 positions
print("\n=== Position matching: SignPuddleParam vs AEG099_015 ===")

sp_data = []
for row in sp.Rows:
    rid = int(row.ID)
    if rid == 0:
        continue
    vals = {}
    for cell in row.Cells:
        fn = str(cell.Def.InternalName)
        vals[fn] = str(cell.Value)
    sp_data.append({
        'id': rid,
        'x': float(vals.get('unknown_0x2c', '0')),
        'y': float(vals.get('unknown_0x30', '0')),
        'z': float(vals.get('unknown_0x34', '0')),
        'u28': int(vals.get('unknown_0x28', '0')),
        'u38': int(vals.get('unknown_0x38', '0')),
        'matchAreaId': int(vals.get('matchAreaId', '0')),
    })

matched = 0
unmatched = 0
for sp_entry in sp_data:
    best_dist = 999999
    best_aeg = None
    for aeg in all_015_data:
        dx = sp_entry['x'] - aeg['x']
        dy = sp_entry['y'] - aeg['y']
        dz = sp_entry['z'] - aeg['z']
        dist = (dx*dx + dy*dy + dz*dz) ** 0.5
        if dist < best_dist:
            best_dist = dist
            best_aeg = aeg
    if best_dist < 5.0:
        matched += 1
        if matched <= 30:
            print(f"  MATCH SP {sp_entry['id']}: ({sp_entry['x']:.1f}, {sp_entry['y']:.1f}, {sp_entry['z']:.1f}) "
                  f"<-> {best_aeg['name']} in {best_aeg['map']} dist={best_dist:.2f} "
                  f"eid={best_aeg['eid']} groups={best_aeg['groups']}")
    else:
        unmatched += 1
        if unmatched <= 10:
            closest = f"{best_aeg['map']} dist={best_dist:.1f}" if best_aeg else "none"
            print(f"  NOMATCH SP {sp_entry['id']}: ({sp_entry['x']:.1f}, {sp_entry['y']:.1f}, {sp_entry['z']:.1f}) "
                  f"closest: {closest}")

print(f"\nTotal: {matched} matched, {unmatched} unmatched out of {len(sp_data)}")

# Summary: what do we know about SignPuddleParam fields?
print("\n" + "="*60)
print("SUMMARY: SignPuddleParam field interpretation")
print("="*60)
print(f"""
Row IDs: 670XXX (the summoning pool IDs, matching the flag convention)
  - matchAreaId: multiplayer match area (e.g. 1000=Limgrave, 1200=Stormveil)
  - unknown_0x20: constant 5898329 (possibly model hash AEG099_015?)
  - unknown_0x24: always 1 (enabled flag)
  - unknown_0x28: for overworld = entity/map reference; for dungeons = area number
  - unknown_0x2c: X position (local map coordinates)
  - unknown_0x30: Y position (local map coordinates)
  - unknown_0x34: Z position (local map coordinates)
  - unknown_0x38: subcategory ID -> SignPuddleSubCategoryParam
  - unknown_0x3c: event flag = 10000000 + row_id (activation flag)
  - unknown_0x40: sort order within subcategory
  - unknown_0x44: always 0

This param IS the summoning pool / Martyr Effigy data.
It has 237 entries (238 rows minus the zero row).
No additional param needed - this is the master list.
""")

print("=== Done ===")
