#!/usr/bin/env python3
"""Quick diagnostic: find Rune/Ember Pieces in ItemLotParam_map and check event flags."""
import sys, io
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

# Load regulation
print("Loading regulation.bin...")
bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(ERR_MOD_DIR / 'regulation.bin'))
ilp = None
for f in bnd.Files:
    if 'ItemLotParam_map' in str(f.Name):
        ilp = _param_read.Invoke(None, Array[Object]([f.Bytes]))
        pt = str(ilp.ParamType) if ilp.ParamType else ''
        if pt in defs:
            ilp.ApplyParamdef(defs[pt])
        break

print(f"ItemLotParam_map: {ilp.Rows.Count} rows")

# Search for goods 800010 and 850010
rune_lots = []
ember_lots = []
for row in ilp.Rows:
    row_id = int(row.ID)
    cells = {}
    if row.Cells:
        for c in row.Cells:
            cells[str(c.Def.InternalName)] = c.Value

    for slot in range(1, 9):
        item_id = int(str(cells.get(f'lotItemId0{slot}', 0)))
        cat = int(str(cells.get(f'lotItemCategory0{slot}', 0)))
        if cat == 1 and item_id == 800010:
            flag = int(str(cells.get('getItemFlagId', 0)))
            rune_lots.append({'lotId': row_id, 'flag': flag})
            break
        if cat == 1 and item_id == 850010:
            flag = int(str(cells.get('getItemFlagId', 0)))
            ember_lots.append({'lotId': row_id, 'flag': flag})
            break

print(f"\nRune Piece lots (goods 800010): {len(rune_lots)}")
print(f"Ember Piece lots (goods 850010): {len(ember_lots)}")
print(f"Rune with eventFlag>0: {sum(1 for r in rune_lots if r['flag'] > 0)}")
print(f"Ember with eventFlag>0: {sum(1 for r in ember_lots if r['flag'] > 0)}")

if rune_lots:
    print("\nFirst 10 rune lots:")
    for r in rune_lots[:10]:
        print(f"  lotId={r['lotId']}, flag={r['flag']}")

if ember_lots:
    print("\nFirst 10 ember lots:")
    for r in ember_lots[:10]:
        print(f"  lotId={r['lotId']}, flag={r['flag']}")

# Now check: are any of these lot IDs referenced by MSB Treasures?
print("\n=== Checking MSB Treasure references ===")
MSB_DIR = ERR_MOD_DIR / 'map' / 'MapStudio'
_msbe_cls = asm.GetType('SoulsFormats.MSBE')
_msbe_read = _msbe_cls.BaseType.GetMethod('Read', Array[SysType]([_byte_arr_type]))

rune_lot_ids = {r['lotId'] for r in rune_lots}
ember_lot_ids = {r['lotId'] for r in ember_lots}
all_target_ids = rune_lot_ids | ember_lot_ids

msb_files = sorted(MSB_DIR.glob('*.msb.dcx'))
found_rune = 0
found_ember = 0
msb_errors = 0

for msb_path in msb_files:
    try:
        raw = SoulsFormats.DCX.Decompress(str(msb_path))
        msb = _msbe_read.Invoke(None, Array[Object]([raw]))
    except:
        msb_errors += 1
        continue

    if msb.Events.Treasures.Count == 0:
        continue

    for t in msb.Events.Treasures:
        lot_id = int(t.ItemLotID)
        if lot_id in rune_lot_ids:
            part = str(t.TreasurePartName) if t.TreasurePartName else '?'
            print(f"  RUNE in {msb_path.name}: lotId={lot_id}, part={part}")
            found_rune += 1
        elif lot_id in ember_lot_ids:
            part = str(t.TreasurePartName) if t.TreasurePartName else '?'
            print(f"  EMBER in {msb_path.name}: lotId={lot_id}, part={part}")
            found_ember += 1

print(f"\nMSB scan: {found_rune} rune treasures, {found_ember} ember treasures ({msb_errors} MSB errors)")
print(f"Unmatched rune lots: {len(rune_lot_ids) - found_rune}")
print(f"Unmatched ember lots: {len(ember_lot_ids) - found_ember}")
