#!/usr/bin/env python3
"""Research: find summoning pool / Martyr Effigy data in regulation.bin params."""

import sys
import io
import os
import tempfile
import re

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

# Load regulation
print("=== Loading regulation.bin ===")
bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(ERR_MOD_DIR / 'regulation.bin'))
paramdefs = load_paramdefs()
print(f"  {bnd.Files.Count} files, {len(paramdefs)} paramdefs")

# Helper to read a param
_str_type = SysType.GetType('System.String')
_param_read = asm.GetType('SoulsFormats.PARAM').GetMethod(
    'Read', BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str_type]), None)

def read_param_raw(file_entry):
    """Read a PARAM from a BND file entry, returns (param, applied_ok)."""
    tmp = os.path.join(tempfile.gettempdir(), '_mfg_research.param')
    SysFile.WriteAllBytes(tmp, file_entry.Bytes.ToArray() if hasattr(file_entry.Bytes, 'ToArray') else file_entry.Bytes)
    param = _param_read.Invoke(None, Array[Object]([tmp]))
    os.unlink(tmp)
    pt = str(param.ParamType) if param.ParamType else ''
    applied = False
    if pt in paramdefs:
        try:
            param.ApplyParamdef(paramdefs[pt])
            applied = True
        except:
            pass
    return param, applied

# ============================================================
# STEP 1: List all param names
# ============================================================
print("\n" + "="*60)
print("STEP 1: All param names in regulation.bin")
print("="*60)

all_params = []
for f in bnd.Files:
    name = str(f.Name)
    # Extract just the param name from the path
    basename = name.split('\\')[-1].split('/')[-1]
    if basename.endswith('.param'):
        basename = basename[:-6]
    all_params.append((basename, f))

all_params.sort(key=lambda x: x[0])
print(f"\nTotal: {len(all_params)} params\n")

# Print params with potentially relevant names
KEYWORDS = ['summon', 'pool', 'effigy', 'martyr', 'bonfire', 'warp', 'coop',
            'multiplayer', 'multi', 'sign', 'phantom', 'invasion', 'buddy',
            'ceremony', 'grace', 'mappoint', 'worldmap']
relevant_params = []
for name, f in all_params:
    low = name.lower()
    for kw in KEYWORDS:
        if kw in low:
            relevant_params.append(name)
            break

print("Potentially relevant params (name matches keywords):")
for name in relevant_params:
    print(f"  {name}")

# Also print ALL param names for reference
print("\nFull param list:")
for name, f in all_params:
    print(f"  {name}")

# ============================================================
# STEP 2: Check BonfireWarpParam
# ============================================================
print("\n" + "="*60)
print("STEP 2: BonfireWarpParam - full field list and data")
print("="*60)

bwp = read_param(bnd, 'BonfireWarpParam', paramdefs)
if bwp:
    print(f"\nBonfireWarpParam: {bwp.Rows.Count} rows")
    # Print all field names from first row
    if bwp.Rows.Count > 0:
        first_row = bwp.Rows[0]
        print("\nAll fields:")
        for cell in first_row.Cells:
            fn = str(cell.Def.InternalName)
            ft = str(cell.Def.DisplayType)
            print(f"  {fn} ({ft})")

        # Check if any field name contains our keywords
        print("\nField name keyword search:")
        for cell in first_row.Cells:
            fn = str(cell.Def.InternalName).lower()
            for kw in ['summon', 'pool', 'effigy', 'martyr', 'flag', 'event']:
                if kw in fn:
                    print(f"  MATCH: {str(cell.Def.InternalName)} contains '{kw}'")

    # Print first 20 rows with all their data
    print("\nFirst 20 rows of BonfireWarpParam:")
    count = 0
    for row in bwp.Rows:
        if count >= 20:
            break
        row_id = int(row.ID)
        vals = {}
        for cell in row.Cells:
            fn = str(cell.Def.InternalName)
            val = cell.Value
            if hasattr(val, 'ToString'):
                val = str(val)
            vals[fn] = val
        # Only print non-zero/non-empty values
        non_zero = {k: v for k, v in vals.items() if v and str(v) != '0' and str(v) != '0.0' and str(v) != 'False' and str(v) != ''}
        print(f"  Row {row_id}: {non_zero}")
        count += 1

    # Search for any rows that might have 670XXX values
    print("\nSearching BonfireWarpParam for 670XXX values:")
    for row in bwp.Rows:
        for cell in row.Cells:
            val = cell.Value
            try:
                ival = int(str(val))
                if 670000 <= ival <= 670999:
                    print(f"  Row {int(row.ID)}, {str(cell.Def.InternalName)} = {ival}")
            except:
                pass
else:
    print("  BonfireWarpParam not found!")

# ============================================================
# STEP 3: Search ALL params for keyword fields & 670XXX values
# ============================================================
print("\n" + "="*60)
print("STEP 3: Search ALL params for summoning/effigy keywords and 670XXX values")
print("="*60)

FIELD_KEYWORDS = ['summon', 'pool', 'effigy', 'martyr', 'ceremony']

for pname, pfile in all_params:
    try:
        param, applied = read_param_raw(pfile)
        if not applied:
            continue
        if param.Rows.Count == 0:
            continue

        first_row = param.Rows[0]

        # Check field names for keywords
        matching_fields = []
        for cell in first_row.Cells:
            fn = str(cell.Def.InternalName).lower()
            for kw in FIELD_KEYWORDS:
                if kw in fn:
                    matching_fields.append(str(cell.Def.InternalName))
                    break

        if matching_fields:
            print(f"\n  {pname} has keyword-matching fields: {matching_fields}")
            # Print a sample of values
            count = 0
            for row in param.Rows:
                if count >= 5:
                    break
                vals = {}
                for cell in row.Cells:
                    fn = str(cell.Def.InternalName)
                    if fn in matching_fields:
                        vals[fn] = str(cell.Value)
                non_zero = {k: v for k, v in vals.items() if v != '0' and v != '0.0' and v != 'False'}
                if non_zero:
                    print(f"    Row {int(row.ID)}: {non_zero}")
                    count += 1

        # Search for 670XXX values in integer fields
        found_670 = False
        for row in param.Rows:
            for cell in row.Cells:
                try:
                    ival = int(str(cell.Value))
                    if 670000 <= ival <= 670999:
                        if not found_670:
                            print(f"\n  {pname} contains 670XXX values:")
                            found_670 = True
                        print(f"    Row {int(row.ID)}, {str(cell.Def.InternalName)} = {ival}")
                except:
                    pass

            # Also check row ID itself
            rid = int(row.ID)
            if 670000 <= rid <= 670999:
                if not found_670:
                    print(f"\n  {pname} contains 670XXX row IDs:")
                    found_670 = True
                print(f"    Row ID = {rid}")
    except Exception as e:
        pass

# ============================================================
# STEP 4: WorldMapPointParam - look for icon 375 / summoning pools
# ============================================================
print("\n" + "="*60)
print("STEP 4: WorldMapPointParam - entries with iconId 375 (summoning pool)")
print("="*60)

wmpp = read_param(bnd, 'WorldMapPointParam', paramdefs)
if wmpp:
    print(f"\nWorldMapPointParam: {wmpp.Rows.Count} rows")

    # Print all fields
    if wmpp.Rows.Count > 0:
        first_row = wmpp.Rows[0]
        print("\nAll fields:")
        for cell in first_row.Cells:
            fn = str(cell.Def.InternalName)
            ft = str(cell.Def.DisplayType)
            print(f"  {fn} ({ft})")

    # Find entries with iconId == 375
    icon375_entries = []
    icon375_count = 0
    for row in wmpp.Rows:
        icon_id = 0
        for cell in row.Cells:
            if str(cell.Def.InternalName) == 'iconId':
                icon_id = int(str(cell.Value))
                break
        if icon_id == 375:
            icon375_count += 1
            if icon375_count <= 30:  # Print first 30
                vals = {}
                for cell in row.Cells:
                    fn = str(cell.Def.InternalName)
                    val = str(cell.Value)
                    if val != '0' and val != '0.0' and val != 'False' and val != '':
                        vals[fn] = val
                icon375_entries.append((int(row.ID), vals))

    print(f"\nEntries with iconId=375: {icon375_count}")
    for rid, vals in icon375_entries:
        print(f"  Row {rid}: {vals}")

    # Also check for 670XXX in textDisableFlagId fields
    print(f"\nWorldMapPointParam entries with 670XXX flags:")
    found_count = 0
    for row in wmpp.Rows:
        for cell in row.Cells:
            fn = str(cell.Def.InternalName)
            if 'flag' in fn.lower() or 'Flag' in fn:
                try:
                    ival = int(str(cell.Value))
                    if 670000 <= ival <= 670999:
                        found_count += 1
                        if found_count <= 30:
                            # Get icon ID for this row
                            icon_val = 0
                            for c2 in row.Cells:
                                if str(c2.Def.InternalName) == 'iconId':
                                    icon_val = int(str(c2.Value))
                                    break
                            print(f"  Row {int(row.ID)}: {fn}={ival}, iconId={icon_val}")
                except:
                    pass
    print(f"  Total: {found_count}")
else:
    print("  WorldMapPointParam not found!")

# ============================================================
# STEP 5: Check for MultiSummonPointParam or coop-related params
# ============================================================
print("\n" + "="*60)
print("STEP 5: Check for coop/multiplayer-related params")
print("="*60)

coop_names = ['MultiSummonPointParam', 'CeremonyParam', 'SignPuddleParam',
              'MultiPlayCorrectionParam', 'BuddyParam', 'PhantomParam']

for cname in coop_names:
    p = read_param(bnd, cname, paramdefs)
    if p:
        print(f"\n{cname}: {p.Rows.Count} rows")
        if p.Rows.Count > 0:
            first_row = p.Rows[0]
            print("  Fields:")
            for cell in first_row.Cells:
                fn = str(cell.Def.InternalName)
                ft = str(cell.Def.DisplayType)
                print(f"    {fn} ({ft})")

            # Print first 10 rows
            print("  First 10 rows:")
            count = 0
            for row in p.Rows:
                if count >= 10:
                    break
                vals = {}
                for cell in row.Cells:
                    fn = str(cell.Def.InternalName)
                    val = str(cell.Value)
                    if val != '0' and val != '0.0' and val != 'False' and val != '':
                        vals[fn] = val
                print(f"    Row {int(row.ID)}: {vals}")
                count += 1
    else:
        print(f"\n{cname}: NOT FOUND")

# ============================================================
# STEP 6: Check BonfireWarpParam for ALL fields, check event flag range
# ============================================================
print("\n" + "="*60)
print("STEP 6: BonfireWarpParam detailed - check for eventFlag field range")
print("="*60)

if bwp:
    # Collect all unique eventFlagId values
    flag_field = None
    for cell in bwp.Rows[0].Cells:
        fn = str(cell.Def.InternalName)
        if 'flag' in fn.lower() or 'event' in fn.lower():
            flag_field = fn
            print(f"  Flag field found: {fn}")

    if flag_field:
        flag_vals = set()
        for row in bwp.Rows:
            for cell in row.Cells:
                if str(cell.Def.InternalName) == flag_field:
                    try:
                        v = int(str(cell.Value))
                        if v > 0:
                            flag_vals.add(v)
                    except:
                        pass
        print(f"  Unique nonzero flag values: {len(flag_vals)}")
        sorted_flags = sorted(flag_vals)
        print(f"  Range: {sorted_flags[0]} to {sorted_flags[-1]}" if sorted_flags else "  No flags")
        # Print flags in 670xxx range
        flags_670 = [f for f in sorted_flags if 670000 <= f <= 670999]
        if flags_670:
            print(f"  Flags in 670000-670999: {flags_670}")
        else:
            print(f"  No flags in 670000-670999 range")
        # Print all flags for reference
        print(f"  All flag values: {sorted_flags[:50]}...")

# ============================================================
# STEP 7: Deeper search - look for "summon" in ALL paramdef XML field names
# ============================================================
print("\n" + "="*60)
print("STEP 7: Search paramdef XMLs for summoning pool related fields")
print("="*60)

import pathlib
paramdef_dir = config.PARAMDEF_DIR
for xml_path in sorted(paramdef_dir.glob('*.xml')):
    with open(xml_path, 'r', encoding='utf-8') as f:
        content = f.read().lower()
    for kw in ['summon', 'pool', 'effigy', 'martyr', 'ceremony']:
        if kw in content:
            print(f"  {xml_path.name} contains '{kw}'")
            # Find the field name
            lines = content.split('\n')
            for line in lines:
                if kw in line and 'internalname' in line:
                    print(f"    {line.strip()}")

print("\n=== Research complete ===")
