#!/usr/bin/env python3
"""
Find the actual coordinate -> lot ID -> event flag mapping for Rune/Ember Pieces.
Strategy: find ALL events in ALL EMEVD files that use instruction 2003:04 (AwardItemLot)
or 2003:36 (AwardItemsIncludingClients) with our lot IDs, then look for coordinates
in the same event or in InitializeEvent (2000:00/2000:06) calls that set up those events.
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
_emevd_cls = asm.GetType('SoulsFormats.EMEVD')
_emevd_read = _emevd_cls.BaseType.GetMethod('Read', Array[SysType]([_byte_arr_type]))

EVENT_DIR = config.require_err_mod_dir() / 'event'
DATA_DIR = config.DATA_DIR

# Load lot IDs
diag = json.load(open(DATA_DIR / '_pieces_diagnostic.json', 'r', encoding='utf-8'))
RUNE_LOTS = {int(k): v for k, v in diag['rune_lots'].items()}
EMBER_LOTS = {int(k): v for k, v in diag['ember_lots'].items()}
ALL_LOTS = {**RUNE_LOTS, **EMBER_LOTS}
print(f"Target: {len(RUNE_LOTS)} rune lots, {len(EMBER_LOTS)} ember lots")

# The key event IDs from our analysis
# Event 1045632900 orchestrates placement - let's look at its sub-events
# Events 1045631100-1045631149 and 1045633100-1045633175 are sub-events

# Also scan for events that use 2000:06 (InitializeEvent with coordinates?)
# Instruction 2000:00 = RunEvent(eventId, args...)
# Instruction 2000:06 = InitializeEvent(slot, eventId, args...)

def parse_emevd_file(path):
    """Parse an EMEVD file, return list of events with parsed instructions."""
    try:
        raw = SoulsFormats.DCX.Decompress(str(path))
        emevd = _emevd_read.Invoke(None, Array[Object]([raw]))
    except:
        return None
    
    events = []
    for event in emevd.Events:
        e = {
            'id': int(event.ID),
            'restBehavior': str(event.RestBehavior),
            'params': [],
            'instructions': [],
        }
        for p in event.Parameters:
            e['params'].append({
                'instrIdx': int(p.InstructionIndex),
                'targetStart': int(p.TargetStartByte),
                'sourceStart': int(p.SourceStartByte),
                'byteCount': int(p.ByteCount),
            })
        for instr in event.Instructions:
            e['instructions'].append({
                'bank': int(instr.Bank),
                'id': int(instr.ID),
                'args': bytes(instr.ArgData) if instr.ArgData else b'',
            })
        events.append(e)
    return events


def i32_at(data, offset):
    if offset + 4 <= len(data):
        return struct.unpack_from('<i', data, offset)[0]
    return None

def u32_at(data, offset):
    if offset + 4 <= len(data):
        return struct.unpack_from('<I', data, offset)[0]
    return None

def f32_at(data, offset):
    if offset + 4 <= len(data):
        return struct.unpack_from('<f', data, offset)[0]
    return None


# Strategy: Look at ALL InitializeEvent (2000:00 / 2000:06) calls in common.emevd
# that call sub-events in the 1045631xxx-1045633xxx range.
# These calls pass parameters that likely include lot IDs and/or coordinates.

print("\n=== Parsing common.emevd ===")
common_events = parse_emevd_file(EVENT_DIR / 'common.emevd.dcx')
if not common_events:
    print("Failed to parse common.emevd!")
    sys.exit(1)

# Index events by ID
event_by_id = {e['id']: e for e in common_events}
print(f"  {len(common_events)} events in common.emevd")

# Find event 1045632900 and dump its RunEvent calls
target_event = event_by_id.get(1045632900)
if target_event:
    print(f"\n=== Event 1045632900: {len(target_event['instructions'])} instructions ===")
    
    # Look for 2000:00 (RunEvent) and 2000:06 (InitializeEvent) calls
    run_event_calls = []
    for idx, instr in enumerate(target_event['instructions']):
        if instr['bank'] == 2000 and instr['id'] in (0, 6):
            args = instr['args']
            # 2000:00 RunEvent: first few bytes are params, includes sub-event ID
            # 2000:06 InitializeEvent: slot(i32), eventId(i32), args...
            if len(args) >= 8:
                if instr['id'] == 6:
                    slot = i32_at(args, 0)
                    sub_event = i32_at(args, 4)
                    rest = args[8:]
                else:
                    # First arg layout varies; let's try to identify sub-event ID
                    # Often: some_param(i32), eventId(i32), ...
                    v0 = i32_at(args, 0)
                    v1 = i32_at(args, 4)
                    sub_event = v1 if 1045630000 <= v1 <= 1045640000 else v0
                    rest = args[8:]
                
                # Extract all i32 values from rest
                rest_vals = []
                for i in range(0, len(rest) - 3, 4):
                    rest_vals.append(i32_at(rest, i))
                
                # Check if any lot IDs are in the args
                all_vals = []
                for i in range(0, len(args) - 3, 4):
                    all_vals.append(i32_at(args, i))
                
                has_lot = any(v in ALL_LOTS for v in all_vals if v is not None)
                
                run_event_calls.append({
                    'idx': idx,
                    'instrId': instr['id'],
                    'subEvent': sub_event,
                    'allVals': all_vals,
                    'hasLot': has_lot,
                    'argLen': len(args),
                    'rawHex': args.hex(),
                })
    
    print(f"  Found {len(run_event_calls)} RunEvent/InitializeEvent calls")
    
    # Show calls that reference our lots
    lot_calls = [c for c in run_event_calls if c['hasLot']]
    print(f"  Of those, {len(lot_calls)} reference our lot IDs")
    
    for c in lot_calls[:20]:
        lot_ids = [v for v in c['allVals'] if v in ALL_LOTS]
        flags = [ALL_LOTS.get(v, '?') for v in lot_ids]
        print(f"    [{c['idx']}] 2000:{c['instrId']:02d} -> event {c['subEvent']}, "
              f"lots={lot_ids}, flags={flags}, allVals={c['allVals']}")

# Now look for the sub-events themselves - they might be in common.emevd too
print("\n=== Looking for sub-events 1045631xxx / 1045633xxx ===")
sub_event_ids = set()
for c in run_event_calls:
    if 1045630000 <= c['subEvent'] <= 1045640000:
        sub_event_ids.add(c['subEvent'])

found_in_common = 0
for eid in sorted(sub_event_ids):
    if eid in event_by_id:
        found_in_common += 1
        e = event_by_id[eid]
        if len(e['instructions']) > 0:
            # Check for coordinates (floats in reasonable world-coordinate range)
            for instr in e['instructions']:
                args = instr['args']
                for i in range(0, len(args) - 3, 4):
                    fval = f32_at(args, i)
                    ival = i32_at(args, i)
                    if fval and 10.0 < abs(fval) < 2000.0 and ival not in ALL_LOTS:
                        pass  # Could be coordinate

print(f"  {found_in_common} / {len(sub_event_ids)} sub-events found in common.emevd")

# Now scan ALL per-map EMEVD files for these sub-events
print("\n=== Scanning per-map EMEVD files for sub-events ===")
emevd_files = sorted(EVENT_DIR.glob('*.emevd.dcx'))
sub_events_found = {}  # eventId -> {map, instructions}

for emevd_path in emevd_files:
    if emevd_path.name.startswith('common'):
        continue
    
    events = parse_emevd_file(emevd_path)
    if not events:
        continue
    
    map_name = emevd_path.name.replace('.emevd.dcx', '')
    
    for e in events:
        eid = e['id']
        
        # Check if this event contains our lot IDs in any instruction args
        has_lot = False
        lot_ids_found = set()
        for instr in e['instructions']:
            args = instr['args']
            for i in range(0, len(args) - 3, 4):
                val = i32_at(args, i)
                if val in ALL_LOTS:
                    has_lot = True
                    lot_ids_found.add(val)
        
        if not has_lot:
            continue
        
        # Check for InitializeEvent calls (2000:00 / 2000:06) that pass lot IDs
        for instr in e['instructions']:
            if instr['bank'] == 2000 and instr['id'] in (0, 6):
                args = instr['args']
                vals = []
                for i in range(0, len(args) - 3, 4):
                    vals.append(i32_at(args, i))
                
                found_lots = [v for v in vals if v in ALL_LOTS]
                if found_lots:
                    # Also extract floats to find coordinates
                    floats = []
                    for i in range(0, len(args) - 3, 4):
                        fv = f32_at(args, i)
                        if fv and 1.0 < abs(fv) < 5000.0:
                            iv = i32_at(args, i)
                            if iv not in ALL_LOTS and not (1000000 <= iv <= 2100000000):
                                floats.append((i, fv))
                    
                    if eid not in sub_events_found:
                        sub_events_found[eid] = []
                    
                    sub_events_found[eid].append({
                        'map': map_name,
                        'lots': found_lots,
                        'vals': vals,
                        'floats': floats,
                        'argLen': len(args),
                    })

print(f"Found {len(sub_events_found)} events with lot IDs across per-map files")

# Detailed output
all_mappings = []
for eid, occurrences in sorted(sub_events_found.items()):
    for occ in occurrences:
        for lot_id in occ['lots']:
            flag = ALL_LOTS.get(lot_id, 0)
            piece_type = 'rune' if lot_id in RUNE_LOTS else 'ember'
            
            # Try to extract coordinates from floats
            coords = [f[1] for f in occ['floats']]
            
            mapping = {
                'eventId': eid,
                'map': occ['map'],
                'lotId': lot_id,
                'flag': flag,
                'type': piece_type,
                'vals': occ['vals'],
                'coords': coords,
                'argLen': occ['argLen'],
            }
            all_mappings.append(mapping)
            
            if len(all_mappings) <= 30:
                coord_str = ', '.join(f'{c:.1f}' for c in coords[:4]) if coords else 'none'
                print(f"  [{piece_type}] {occ['map']} event {eid}: "
                      f"lot={lot_id}, flag={flag}, coords=[{coord_str}]")

print(f"\nTotal mappings found: {len(all_mappings)}")
rune_mapped = len(set(m['lotId'] for m in all_mappings if m['type'] == 'rune'))
ember_mapped = len(set(m['lotId'] for m in all_mappings if m['type'] == 'ember'))
print(f"Unique rune lots mapped: {rune_mapped} / {len(RUNE_LOTS)}")
print(f"Unique ember lots mapped: {ember_mapped} / {len(EMBER_LOTS)}")

# Save
out_path = DATA_DIR / '_piece_mappings.json'
with open(out_path, 'w', encoding='utf-8') as f:
    json.dump(all_mappings, f, indent=2, ensure_ascii=False)
print(f"Saved to {out_path}")
