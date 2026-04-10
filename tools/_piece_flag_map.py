#!/usr/bin/env python3
"""
Final comprehensive Rune/Ember Piece mapping.
Key insight: subEvent2 (1045630100+n) = per-position "collected" flag.
Evidence: instruction [55] 2003:66(0, subEvent2, 1) sets the flag after collection.

Also search for ALL similar RunEvent patterns across ALL EMEVD files.
And search for ALL AEG099_510 assets to find positions not covered by events.
"""
import sys, io, struct, json
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
_emevd_cls = asm.GetType('SoulsFormats.EMEVD')
_emevd_read = _emevd_cls.BaseType.GetMethod('Read', Array[SysType]([_byte_arr_type]))

_err_mod = config.require_err_mod_dir()
EVENT_DIR = _err_mod / 'event'
MAP_DIR = _err_mod / 'map' / 'MapStudio'
DATA_DIR = config.DATA_DIR

diag = json.load(open(DATA_DIR / '_pieces_diagnostic.json', 'r', encoding='utf-8'))
RUNE_LOTS = {int(k): v for k, v in diag['rune_lots'].items()}
EMBER_LOTS = {int(k): v for k, v in diag['ember_lots'].items()}
ALL_LOTS = {**RUNE_LOTS, **EMBER_LOTS}

def i32(data, off): return struct.unpack_from('<i', data, off)[0] if off+4 <= len(data) else None

# ========================
# STEP 1: Build AEG099_510 entity position index (deduplicated)
# ========================
print("=" * 70)
print("STEP 1: Index ALL AEG099_510 assets")
print("=" * 70)

msb_files = sorted(MAP_DIR.glob('*.msb.dcx'))
aeg510_by_eid = {}

for msb_path in msb_files:
    tile = msb_path.name.split('.')[0]
    # Skip "alternate" maps (m31_90 = copy of m11_10, m60_xx_xx_10 = LOD copies)
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
            if 'AEG099_510' not in model:
                continue
            eid = int(p.EntityID)
            if eid <= 0:
                continue
            pos = (float(p.Position.X), float(p.Position.Y), float(p.Position.Z))
            if eid not in aeg510_by_eid:
                aeg510_by_eid[eid] = {
                    'map': tile, 'name': str(p.Name),
                    'x': pos[0], 'y': pos[1], 'z': pos[2],
                }

print(f"Unique AEG099_510 entities (deduplicated): {len(aeg510_by_eid)}")


# ========================
# STEP 2: Parse ALL EMEVD files for RunEvent calls to handler ~1045630910
# ========================
print("\n" + "=" * 70)
print("STEP 2: Scan ALL EMEVD files for piece handler RunEvent calls")
print("=" * 70)

piece_entries = []  # {lotId, flag, entityId, collectedFlag, map, x, y, z, source}
emevd_files = sorted(EVENT_DIR.glob('*.emevd.dcx'))

for emevd_path in emevd_files:
    try:
        raw = SoulsFormats.DCX.Decompress(str(emevd_path))
        emevd = _emevd_read.Invoke(None, Array[Object]([raw]))
    except:
        continue
    
    fname = emevd_path.name.replace('.emevd.dcx', '')
    
    for event in emevd.Events:
        eid = int(event.ID)
        for instr in event.Instructions:
            if int(instr.Bank) != 2000 or int(instr.ID) != 0:
                continue
            args = bytes(instr.ArgData) if instr.ArgData else b''
            if len(args) < 40:
                continue
            
            vals = [i32(args, i*4) for i in range(len(args)//4)]
            called = vals[1]
            
            # Pattern: [slot, handlerId, subEvent1, subEvent2, lotBase1, lotBase2, 
            #           subEvent3, subEvent4, lotId, mapTile]
            # handlerId is likely 1045630910 or 1049630910 (DLC)
            # subEvent4 should be an AEG099_510 EntityID
            
            if len(vals) >= 10:
                sub4 = vals[7]
                lot_id = vals[8]
                sub2 = vals[3]  # collected flag
                
                # Verify sub4 is an AEG099_510 EntityID
                if sub4 in aeg510_by_eid:
                    pos = aeg510_by_eid[sub4]
                    flag = ALL_LOTS.get(lot_id, 0)
                    piece_type = 'rune' if lot_id in RUNE_LOTS else ('ember' if lot_id in EMBER_LOTS else 'unknown')
                    
                    piece_entries.append({
                        'lotId': lot_id,
                        'flag': flag,
                        'type': piece_type,
                        'entityId': sub4,
                        'collectedFlag': sub2,
                        'map': pos['map'],
                        'x': pos['x'], 'y': pos['y'], 'z': pos['z'],
                        'source': f'{fname}:event_{eid}',
                        'handlerId': called,
                    })

# Deduplicate by entityId (same physical position)
seen_entities = set()
unique_entries = []
for p in piece_entries:
    if p['entityId'] not in seen_entities:
        seen_entities.add(p['entityId'])
        unique_entries.append(p)

print(f"Total RunEvent matches: {len(piece_entries)}")
print(f"Unique positions (by entityId): {len(unique_entries)}")


# ========================
# STEP 3: Add entity_match positions from diagnostic
# ========================
print("\n" + "=" * 70)
print("STEP 3: Add entity_match positions")
print("=" * 70)

for match in diag.get('entity_matches', []):
    lot_id = match['lotId']
    eid_val = 0
    # These are Enemies, not Assets, so they won't be in aeg510_by_eid
    flag = ALL_LOTS.get(lot_id, match.get('flag', 0))
    piece_type = match.get('type', 'unknown')
    
    unique_entries.append({
        'lotId': lot_id,
        'flag': flag,
        'type': piece_type,
        'entityId': eid_val,
        'collectedFlag': flag,  # Use the lot flag as collected flag
        'map': match['map'],
        'x': match['x'], 'y': match['y'], 'z': match['z'],
        'source': 'entity_match',
        'handlerId': 0,
    })

print(f"After entity matches: {len(unique_entries)} total unique positions")


# ========================
# STEP 4: Find AEG099_510 assets NOT covered by any event
# ========================
print("\n" + "=" * 70)
print("STEP 4: AEG099_510 assets not covered by events")
print("=" * 70)

covered_eids = set(p['entityId'] for p in unique_entries)
uncovered = {eid: pos for eid, pos in aeg510_by_eid.items() if eid not in covered_eids}

print(f"Covered by events: {len(covered_eids)}")
print(f"Uncovered AEG099_510: {len(uncovered)}")

for eid, pos in sorted(uncovered.items()):
    print(f"  eid={eid} map={pos['map']} pos=({pos['x']:.1f}, {pos['y']:.1f}, {pos['z']:.1f}) "
          f"name={pos['name']}")


# ========================
# STEP 5: Summary
# ========================
print("\n" + "=" * 70)
print("FINAL SUMMARY")
print("=" * 70)

by_type = {}
for p in unique_entries:
    by_type[p['type']] = by_type.get(p['type'], 0) + 1

print(f"Total unique piece positions: {len(unique_entries)}")
for t, c in sorted(by_type.items()):
    print(f"  {t}: {c}")

by_source = {}
for p in unique_entries:
    src = p['source'].split(':')[0]
    by_source[src] = by_source.get(src, 0) + 1
print(f"\nBy source:")
for src, c in sorted(by_source.items()):
    print(f"  {src}: {c}")

# Show collected flags
flags = [p['collectedFlag'] for p in unique_entries if p['collectedFlag'] > 0]
if flags:
    print(f"\nCollected flags range: {min(flags)} - {max(flags)}")
    print(f"Unique collected flags: {len(set(flags))}")

print(f"\nDetailed entries:")
for i, p in enumerate(unique_entries):
    print(f"  [{i:3d}] lot={p['lotId']:>10d} flag={p['flag']:>10d} "
          f"collectedFlag={p['collectedFlag']:>10d} type={p['type']:7s} "
          f"map={p['map']:>15s} pos=({p['x']:>8.1f}, {p['y']:>8.1f}, {p['z']:>8.1f}) "
          f"src={p['source']}")

# Save
out_path = DATA_DIR / '_piece_final_map.json'
with open(out_path, 'w', encoding='utf-8') as f:
    json.dump(unique_entries, f, indent=2, ensure_ascii=False)
print(f"\nSaved to {out_path}")
