#!/usr/bin/env python3
"""Search ALL params in regulation.bin for any reference to:
  - goods 800010 (Rune Piece)
  - goods 800011 (Runic Trace)
  - goods 850010 (Ember Piece)
  - model name strings containing "821" or "822"
  - any param that might link assets to item lots or tracking
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

ERR_MOD_DIR = config.require_err_mod_dir()

asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
clr.AddReference(str(config.SOULSFORMATS_DLL))
import SoulsFormats

_byte_arr_type = SysType.GetType('System.Byte[]')
_param_cls = asm.GetType('SoulsFormats.PARAM')
_param_read = _param_cls.BaseType.GetMethod('Read', Array[SysType]([_byte_arr_type]))

# Load paramdefs
paramdex = config.PARAMDEF_DIR / 'Paramdex' / 'ER' / 'Defs'
defs = {}
for xml_path in paramdex.glob('*.xml'):
    try:
        pdef = SoulsFormats.PARAMDEF.XmlDeserialize(str(xml_path))
        if pdef and pdef.ParamType:
            defs[str(pdef.ParamType)] = pdef
    except:
        pass
print(f"Loaded {len(defs)} paramdefs")

# Load regulation
print("Loading regulation.bin...")
bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(ERR_MOD_DIR / 'regulation.bin'))

TARGET_IDS = {800010, 800011, 850010, 850011}
SEARCH_STRINGS = ['821', '822', 'rune', 'piece', 'gather', 'geom']

results = []

for f in bnd.Files:
    param_name = str(f.Name).split('\\')[-1].replace('.param', '')
    try:
        param = _param_read.Invoke(None, Array[Object]([f.Bytes]))
        pt = str(param.ParamType) if param.ParamType else ''
        if pt in defs:
            param.ApplyParamdef(defs[pt])
        else:
            continue
    except:
        continue

    row_count = param.Rows.Count
    if row_count == 0:
        continue

    # Get field names
    first_row = param.Rows[0]
    if not first_row.Cells or first_row.Cells.Count == 0:
        continue

    field_names = [str(c.Def.InternalName) for c in first_row.Cells]

    # Check if any field name contains interesting keywords
    interesting_fields = [fn for fn in field_names
                          if any(s in fn.lower() for s in ['asset', 'model', 'obj', 'gather',
                                                            'geom', 'disable', 'flag', 'lot'])]

    # Search for target IDs in cell values
    for row in param.Rows:
        row_id = int(row.ID)

        # Check if row ID itself is a target
        if row_id in TARGET_IDS:
            cells = {str(c.Def.InternalName): str(c.Value) for c in row.Cells}
            results.append({
                'param': param_name,
                'rowId': row_id,
                'reason': f'Row ID matches target goods',
                'fields': cells
            })
            continue

        # Check cell values for target IDs
        for c in row.Cells:
            try:
                val = int(str(c.Value))
                if val in TARGET_IDS:
                    fname = str(c.Def.InternalName)
                    results.append({
                        'param': param_name,
                        'rowId': row_id,
                        'reason': f'{fname} = {val}',
                        'fields': {str(cc.Def.InternalName): str(cc.Value)
                                   for cc in row.Cells
                                   if str(cc.Value) != '0' and str(cc.Value) != '-1'}
                    })
            except:
                pass

    # Check for string values containing AEG099
    for row in param.Rows:
        for c in row.Cells:
            val_str = str(c.Value)
            if 'AEG099' in val_str:
                results.append({
                    'param': param_name,
                    'rowId': int(row.ID),
                    'reason': f'{str(c.Def.InternalName)} = {val_str}',
                    'fields': {}
                })

print(f"\n{'='*80}")
print(f"Found {len(results)} references in regulation.bin")
print(f"{'='*80}\n")

for r in results:
    print(f"  [{r['param']}] row {r['rowId']}: {r['reason']}")
    if r['fields']:
        for k, v in list(r['fields'].items())[:15]:
            print(f"    {k} = {v}")
    print()

# Also search specific params by name
print(f"\n{'='*80}")
print("Searching for gathering/asset-related params by name...")
print(f"{'='*80}\n")

for f in bnd.Files:
    pn = str(f.Name).lower()
    if any(kw in pn for kw in ['gather', 'asset', 'geom', 'objact', 'actionbutton',
                                 'gaitem', 'treasure', 'rollingobj']):
        try:
            param = _param_read.Invoke(None, Array[Object]([f.Bytes]))
            pt = str(param.ParamType) if param.ParamType else ''
            if pt in defs:
                param.ApplyParamdef(defs[pt])
            row_count = param.Rows.Count

            field_names = []
            if row_count > 0 and param.Rows[0].Cells:
                field_names = [str(c.Def.InternalName) for c in param.Rows[0].Cells]

            print(f"  {str(f.Name)}: {row_count} rows, type={pt}")
            if field_names:
                print(f"    Fields: {', '.join(field_names[:20])}")
            print()
        except:
            pass
