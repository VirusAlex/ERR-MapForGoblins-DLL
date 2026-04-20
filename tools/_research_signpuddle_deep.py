#!/usr/bin/env python3
"""Deep dive into SignPuddleParam - the summoning pool param."""

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

from extract_all_items import load_paramdefs, read_param, param_to_dict

ERR_MOD_DIR = config.require_err_mod_dir()

print("=== Loading regulation.bin ===")
bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(ERR_MOD_DIR / 'regulation.bin'))
paramdefs = load_paramdefs()

# ============================================================
# SignPuddleParam - full dump
# ============================================================
print("\n" + "="*60)
print("SignPuddleParam - Full Analysis")
print("="*60)

sp = read_param(bnd, 'SignPuddleParam', paramdefs)
print(f"\nTotal rows: {sp.Rows.Count}")

# Print all field names and types
print("\nField definitions:")
first_row = sp.Rows[0]
field_names = []
for cell in first_row.Cells:
    fn = str(cell.Def.InternalName)
    ft = str(cell.Def.DisplayType)
    field_names.append(fn)
    print(f"  {fn} ({ft})")

# Decode field meanings based on known values
print("\nField interpretation based on data analysis:")
print("  matchAreaId => Multiplayer matching area (e.g. 1000, 1200, ...)")
print("  unknown_0x20 => Constant 5898329 - possibly a hash/magic number")
print("  unknown_0x24 => Always 1 - possibly enabled flag")
print("  unknown_0x28 => Varies widely - could be entityId or MSB reference")
print("  unknown_0x2c => Float - X coordinate")
print("  unknown_0x30 => Float - Y coordinate")
print("  unknown_0x34 => Float - Z coordinate")
print("  unknown_0x38 => Looks like a subcategory ID (61000, 61002, etc.)")
print("  unknown_0x3c => 10670XXX - flag (10000000 + row ID)")
print("  unknown_0x40 => Small int - sort order?")

# Print ALL rows
print("\n" + "="*60)
print("All SignPuddleParam rows:")
print("="*60)

for row in sp.Rows:
    rid = int(row.ID)
    vals = {}
    for cell in row.Cells:
        fn = str(cell.Def.InternalName)
        val = cell.Value
        if hasattr(val, 'ToString'):
            val_s = str(val)
        else:
            val_s = str(val)
        # Skip byte arrays
        if 'System.Byte[]' in val_s:
            continue
        if val_s != '0' and val_s != '0.0':
            vals[fn] = val_s
    print(f"  {rid}: {vals}")

# ============================================================
# SignPuddleSubCategoryParam
# ============================================================
print("\n" + "="*60)
print("SignPuddleSubCategoryParam - Full dump")
print("="*60)

spsc = read_param(bnd, 'SignPuddleSubCategoryParam', paramdefs)
if spsc:
    print(f"\nTotal rows: {spsc.Rows.Count}")
    if spsc.Rows.Count > 0:
        print("\nField definitions:")
        for cell in spsc.Rows[0].Cells:
            fn = str(cell.Def.InternalName)
            ft = str(cell.Def.DisplayType)
            print(f"  {fn} ({ft})")

    print("\nAll rows:")
    for row in spsc.Rows:
        rid = int(row.ID)
        vals = {}
        for cell in row.Cells:
            fn = str(cell.Def.InternalName)
            val_s = str(cell.Value)
            if 'System.Byte[]' in val_s:
                continue
            if val_s != '0' and val_s != '0.0':
                vals[fn] = val_s
        print(f"  {rid}: {vals}")

# ============================================================
# SignPuddleTabParam
# ============================================================
print("\n" + "="*60)
print("SignPuddleTabParam - Full dump")
print("="*60)

spt = read_param(bnd, 'SignPuddleTabParam', paramdefs)
if spt:
    print(f"\nTotal rows: {spt.Rows.Count}")
    if spt.Rows.Count > 0:
        print("\nField definitions:")
        for cell in spt.Rows[0].Cells:
            fn = str(cell.Def.InternalName)
            ft = str(cell.Def.DisplayType)
            print(f"  {fn} ({ft})")

    print("\nAll rows:")
    for row in spt.Rows:
        rid = int(row.ID)
        vals = {}
        for cell in row.Cells:
            fn = str(cell.Def.InternalName)
            val_s = str(cell.Value)
            if 'System.Byte[]' in val_s:
                continue
            if val_s != '0' and val_s != '0.0':
                vals[fn] = val_s
        print(f"  {rid}: {vals}")

# ============================================================
# Analyze the unknown_0x28 field - check if these are entity IDs in MSBs
# ============================================================
print("\n" + "="*60)
print("Analyzing unknown_0x28 values - possible entity IDs")
print("="*60)

entity_ids = set()
for row in sp.Rows:
    for cell in row.Cells:
        fn = str(cell.Def.InternalName)
        if fn == 'unknown_0x28':
            val = int(str(cell.Value))
            if val > 0:
                entity_ids.add(val)

print(f"Unique unknown_0x28 values: {len(entity_ids)}")
sorted_eids = sorted(entity_ids)
for eid in sorted_eids[:30]:
    print(f"  {eid}")
if len(sorted_eids) > 30:
    print(f"  ... ({len(sorted_eids) - 30} more)")

# Check if any of these are AEG099_015 entity IDs by scanning MSBs
print("\nSearching MSBs for AEG099_015 entity IDs...")
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

# Collect all AEG099_015 asset positions & entity IDs, and all asset entity IDs
aeg015_entities = {}
all_asset_entities = {}
matched_count = 0
for msb_path in sorted(MSB_DIR.glob('*.msb.dcx')):
    map_name = msb_path.name.replace('.msb.dcx', '')
    try:
        msb = rfb(_msbe_read, SoulsFormats.DCX.Decompress(str(msb_path)), '.msb')
    except:
        continue

    for p in msb.Parts.Assets:
        eid = int(p.EntityID)
        model = str(p.ModelName)
        if eid > 0:
            info = {
                'map': map_name,
                'model': model,
                'name': str(p.Name),
                'x': float(p.Position.X),
                'y': float(p.Position.Y),
                'z': float(p.Position.Z),
            }
            if model == 'AEG099_015':
                aeg015_entities[eid] = info
            if eid in entity_ids:
                all_asset_entities[eid] = info

print(f"\nFound {len(aeg015_entities)} AEG099_015 assets with entity IDs")
print(f"Of {len(entity_ids)} unknown_0x28 values, {len(all_asset_entities)} match asset entity IDs")

# Show matches
if all_asset_entities:
    print("\nMatched entity IDs:")
    for eid in sorted(all_asset_entities.keys()):
        info = all_asset_entities[eid]
        print(f"  {eid}: {info['model']} in {info['map']} at ({info['x']:.1f}, {info['y']:.1f}, {info['z']:.1f})")

# Now check if unknown_0x28 might be something else - e.g. encoded map coordinates
print("\nChecking if unknown_0x28 could be encoded coordinates...")
for row in sp.Rows:
    rid = int(row.ID)
    if rid == 0:
        continue
    vals = {}
    for cell in row.Cells:
        fn = str(cell.Def.InternalName)
        vals[fn] = str(cell.Value)

    u28 = int(vals.get('unknown_0x28', '0'))
    if u28 <= 0:
        continue
    # Decode as possible entity ID: AABBCCDD format
    s = str(u28)
    if len(s) == 7:
        # Could be AXXYYCC - area XX YY CC
        area_maybe = int(s[:1])
        xx = int(s[1:3])
        yy = int(s[3:5])
        cc = int(s[5:7])
        if rid <= 670110 and rid >= 670099:
            print(f"  Row {rid}: u28={u28} (len={len(s)}) -> {area_maybe}_{xx}_{yy}_{cc}?  coords=({vals.get('unknown_0x2c','?')}, {vals.get('unknown_0x30','?')}, {vals.get('unknown_0x34','?')})")

# ============================================================
# Compare SignPuddleParam positions with AEG099_015 MSB positions
# ============================================================
print("\n" + "="*60)
print("Comparing SignPuddleParam coordinates with AEG099_015 MSB positions")
print("="*60)

# Build list of SignPuddle positions
sp_positions = []
for row in sp.Rows:
    rid = int(row.ID)
    if rid == 0:
        continue
    vals = {}
    for cell in row.Cells:
        fn = str(cell.Def.InternalName)
        vals[fn] = str(cell.Value)

    x = float(vals.get('unknown_0x2c', '0'))
    y = float(vals.get('unknown_0x30', '0'))
    z = float(vals.get('unknown_0x34', '0'))
    u28 = int(vals.get('unknown_0x28', '0'))
    sp_positions.append({'id': rid, 'x': x, 'y': y, 'z': z, 'u28': u28,
                         'matchAreaId': int(vals.get('matchAreaId', '0'))})

# Build list of AEG099_015 positions
aeg_positions = []
for eid, info in aeg015_entities.items():
    aeg_positions.append({'eid': eid, 'x': info['x'], 'y': info['y'], 'z': info['z'],
                          'map': info['map'], 'name': info['name']})

print(f"\nSignPuddleParam entries: {len(sp_positions)}")
print(f"AEG099_015 entities: {len(aeg_positions)}")

# Try matching by proximity
matches = []
for sp_pos in sp_positions:
    best_dist = 999999
    best_aeg = None
    for aeg_pos in aeg_positions:
        dx = sp_pos['x'] - aeg_pos['x']
        dy = sp_pos['y'] - aeg_pos['y']
        dz = sp_pos['z'] - aeg_pos['z']
        dist = (dx*dx + dy*dy + dz*dz) ** 0.5
        if dist < best_dist:
            best_dist = dist
            best_aeg = aeg_pos
    if best_dist < 5.0:
        matches.append((sp_pos, best_aeg, best_dist))

print(f"Matches within 5.0 units: {len(matches)}")
for sp_pos, aeg, dist in matches[:20]:
    print(f"  SP {sp_pos['id']}: ({sp_pos['x']:.1f}, {sp_pos['y']:.1f}, {sp_pos['z']:.1f}) <-> "
          f"AEG {aeg['eid']} ({aeg['x']:.1f}, {aeg['y']:.1f}, {aeg['z']:.1f}) in {aeg['map']} dist={dist:.2f}")

# Check if u28 matches entity IDs directly
print("\nDirect u28 -> AEG099_015 entity ID matches:")
direct_matches = 0
for sp_pos in sp_positions:
    if sp_pos['u28'] in aeg015_entities:
        info = aeg015_entities[sp_pos['u28']]
        print(f"  SP {sp_pos['id']}: u28={sp_pos['u28']} -> {info['name']} in {info['map']}")
        direct_matches += 1
print(f"Total direct matches: {direct_matches}")

# ============================================================
# Analyze the flag pattern: unknown_0x3c = 10670XXX
# ============================================================
print("\n" + "="*60)
print("Flag analysis: unknown_0x3c values")
print("="*60)

flag_values = []
for row in sp.Rows:
    rid = int(row.ID)
    for cell in row.Cells:
        fn = str(cell.Def.InternalName)
        if fn == 'unknown_0x3c':
            val = int(str(cell.Value))
            if val > 0:
                flag_values.append((rid, val))

print(f"Nonzero unknown_0x3c values: {len(flag_values)}")
print("Pattern check (is it 10000000 + row_id?):")
mismatches = 0
for rid, flag in flag_values[:30]:
    expected = 10000000 + rid
    match = "YES" if flag == expected else f"NO (expected {expected})"
    print(f"  Row {rid}: flag={flag} -> 10M + rowId? {match}")
    if flag != expected:
        mismatches += 1
print(f"Mismatches: {mismatches} out of {len(flag_values)}")

# ============================================================
# Check BuddyStoneParam for relation to summoning pools
# ============================================================
print("\n" + "="*60)
print("BuddyStoneParam - checking for summoning pool references")
print("="*60)

bsp = read_param(bnd, 'BuddyStoneParam', paramdefs)
if bsp:
    print(f"\nTotal rows: {bsp.Rows.Count}")
    if bsp.Rows.Count > 0:
        print("\nField definitions:")
        for cell in bsp.Rows[0].Cells:
            fn = str(cell.Def.InternalName)
            ft = str(cell.Def.DisplayType)
            print(f"  {fn} ({ft})")

    print("\nFirst 20 rows:")
    count = 0
    for row in bsp.Rows:
        if count >= 20:
            break
        rid = int(row.ID)
        vals = {}
        for cell in row.Cells:
            fn = str(cell.Def.InternalName)
            val_s = str(cell.Value)
            if 'System.Byte[]' in val_s:
                continue
            if val_s != '0' and val_s != '0.0' and val_s != '-1' and val_s != 'False':
                vals[fn] = val_s
        print(f"  {rid}: {vals}")
        count += 1

# ============================================================
# Ceremony param (the name matched but was not found earlier - double check)
# ============================================================
print("\n" + "="*60)
print("Ceremony param - full check")
print("="*60)

cer = read_param(bnd, 'Ceremony', paramdefs)
if cer:
    print(f"\nTotal rows: {cer.Rows.Count}")
    if cer.Rows.Count > 0:
        print("\nField definitions:")
        for cell in cer.Rows[0].Cells:
            fn = str(cell.Def.InternalName)
            ft = str(cell.Def.DisplayType)
            print(f"  {fn} ({ft})")

    print("\nAll rows:")
    for row in cer.Rows:
        rid = int(row.ID)
        vals = {}
        for cell in row.Cells:
            fn = str(cell.Def.InternalName)
            val_s = str(cell.Value)
            if 'System.Byte[]' in val_s:
                continue
            if val_s != '0' and val_s != '0.0' and val_s != '-1' and val_s != 'False':
                vals[fn] = val_s
        print(f"  {rid}: {vals}")
else:
    print("  Ceremony param not found")

print("\n=== Deep research complete ===")
