#!/usr/bin/env python3
"""
Try to match Rune/Ember Piece ItemLotParam entries with MSB Assets by EntityID.
Also try matching by proximity to known MASSEDIT coordinates.
"""
import sys, io, json, math
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
from pathlib import Path
import config
from pythonnet import load
load('coreclr')
import clr
from System.Reflection import Assembly
from System import Array, Type as SysType, Object

ERR_MOD_DIR = config.require_err_mod_dir()
DATA_DIR = config.DATA_DIR

asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
clr.AddReference(str(config.SOULSFORMATS_DLL))
import SoulsFormats

_byte_arr_type = SysType.GetType('System.Byte[]')
_param_cls = asm.GetType('SoulsFormats.PARAM')
_param_read = _param_cls.BaseType.GetMethod('Read', Array[SysType]([_byte_arr_type]))
_msbe_cls = asm.GetType('SoulsFormats.MSBE')
_msbe_read = _msbe_cls.BaseType.GetMethod('Read', Array[SysType]([_byte_arr_type]))

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

# 1. Get all Rune/Ember lot IDs + event flags from ItemLotParam_map
print("=== Loading regulation.bin ===")
bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(ERR_MOD_DIR / 'regulation.bin'))
ilp = None
for f in bnd.Files:
    if 'ItemLotParam_map' in str(f.Name):
        ilp = _param_read.Invoke(None, Array[Object]([f.Bytes]))
        pt = str(ilp.ParamType) if ilp.ParamType else ''
        if pt in defs:
            ilp.ApplyParamdef(defs[pt])
        break

rune_lots = {}  # lotId -> flag
ember_lots = {}
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
            rune_lots[row_id] = int(str(cells.get('getItemFlagId', 0)))
            break
        if cat == 1 and item_id == 850010:
            ember_lots[row_id] = int(str(cells.get('getItemFlagId', 0)))
            break

all_lot_ids = set(rune_lots.keys()) | set(ember_lots.keys())
print(f"Rune lots: {len(rune_lots)}, Ember lots: {len(ember_lots)}")

# 2. Scan ALL MSB files for Assets/Parts with EntityID matching lot IDs
#    Also collect ALL asset EntityIDs to understand the ID ranges
print("\n=== Scanning MSB files for EntityID matches ===")
MSB_DIR = ERR_MOD_DIR / 'map' / 'MapStudio'
msb_files = sorted(MSB_DIR.glob('*.msb.dcx'))

entity_matches = []  # {lotId, flag, type, x, y, z, map, entityId, partName}
all_entity_ids = set()
msb_ok = 0
msb_fail = 0

for msb_path in msb_files:
    try:
        raw = SoulsFormats.DCX.Decompress(str(msb_path))
        msb = _msbe_read.Invoke(None, Array[Object]([raw]))
        msb_ok += 1
    except:
        msb_fail += 1
        continue

    map_name = msb_path.name.replace('.msb.dcx', '')

    # Check ALL part types for matching EntityIDs
    for part_list_name in ['Assets', 'DummyAssets', 'Enemies', 'Players', 'MapPieces', 'ConnectCollisions']:
        try:
            parts = getattr(msb.Parts, part_list_name)
        except:
            continue
        for p in parts:
            eid = int(p.EntityID) if hasattr(p, 'EntityID') else 0
            if eid > 0:
                all_entity_ids.add(eid)
            if eid in all_lot_ids:
                x = float(p.Position.X)
                y = float(p.Position.Y)
                z = float(p.Position.Z)
                is_rune = eid in rune_lots
                flag = rune_lots.get(eid, ember_lots.get(eid, 0))
                entity_matches.append({
                    'lotId': eid,
                    'flag': flag,
                    'type': 'rune' if is_rune else 'ember',
                    'partType': part_list_name,
                    'partName': str(p.Name),
                    'map': map_name,
                    'x': round(x, 3), 'y': round(y, 3), 'z': round(z, 3),
                })

            # Also check EntityGroups if available
            if hasattr(p, 'EntityGroups'):
                for eg in p.EntityGroups:
                    eg_val = int(eg)
                    if eg_val in all_lot_ids:
                        x = float(p.Position.X)
                        y = float(p.Position.Y)
                        z = float(p.Position.Z)
                        is_rune = eg_val in rune_lots
                        flag = rune_lots.get(eg_val, ember_lots.get(eg_val, 0))
                        entity_matches.append({
                            'lotId': eg_val,
                            'flag': flag,
                            'type': 'rune' if is_rune else 'ember',
                            'partType': f'{part_list_name}(EntityGroup)',
                            'partName': str(p.Name),
                            'map': map_name,
                            'x': round(x, 3), 'y': round(y, 3), 'z': round(z, 3),
                        })

print(f"MSB parsed: {msb_ok} ok, {msb_fail} failed")
print(f"Total entity IDs found: {len(all_entity_ids)}")
print(f"Direct EntityID matches: {len(entity_matches)}")

rune_matched_ids = {m['lotId'] for m in entity_matches if m['type'] == 'rune'}
ember_matched_ids = {m['lotId'] for m in entity_matches if m['type'] == 'ember'}
print(f"Unique rune lots matched: {len(rune_matched_ids)} / {len(rune_lots)}")
print(f"Unique ember lots matched: {len(ember_matched_ids)} / {len(ember_lots)}")

if entity_matches:
    print("\nFirst 20 matches:")
    for m in entity_matches[:20]:
        print(f"  [{m['type']}] lotId={m['lotId']}, flag={m['flag']}, "
              f"{m['partType']}:{m['partName']} @ {m['map']} ({m['x']}, {m['y']}, {m['z']})")

# 3. Check if lot IDs fall in any EntityID range
print("\n=== EntityID range analysis ===")
lot_ids_sorted = sorted(all_lot_ids)
print(f"Lot ID range: {lot_ids_sorted[0]} - {lot_ids_sorted[-1]}")
# Check nearby entity IDs
nearby = [eid for eid in all_entity_ids if 10000 <= eid <= 11000]
print(f"Entity IDs in range 10000-11000: {len(nearby)}")
if nearby:
    print(f"  {sorted(nearby)[:20]}...")

# Save matches
out = {
    'entity_matches': entity_matches,
    'rune_lots': {str(k): v for k, v in rune_lots.items()},
    'ember_lots': {str(k): v for k, v in ember_lots.items()},
    'stats': {
        'msb_ok': msb_ok, 'msb_fail': msb_fail,
        'rune_matched': len(rune_matched_ids),
        'ember_matched': len(ember_matched_ids),
    }
}
out_path = DATA_DIR / '_pieces_diagnostic.json'
with open(out_path, 'w', encoding='utf-8') as f:
    json.dump(out, f, indent=2, ensure_ascii=False)
print(f"\nSaved diagnostic to {out_path}")
