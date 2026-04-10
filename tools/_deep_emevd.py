#!/usr/bin/env python3
"""
Deep dive into EMEVD Rune/Ember Piece events.
1. Parse event 1045630910 (the main handler) to understand its instruction set
2. Find sub-events 1045631100-1045631149, 1045633100-1045633175 in ALL emevd files
3. Try to match arg[9] values as Entity IDs in MSB files for coordinates
4. Look at ALL events that reference any of our lot IDs to find the complete picture
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

_err_mod = config.require_err_mod_dir()
EVENT_DIR = _err_mod / 'event'
MAP_DIR = _err_mod / 'map' / 'MapStudio'
DATA_DIR = config.DATA_DIR

diag = json.load(open(DATA_DIR / '_pieces_diagnostic.json', 'r', encoding='utf-8'))
RUNE_LOTS = {int(k): v for k, v in diag['rune_lots'].items()}
EMBER_LOTS = {int(k): v for k, v in diag['ember_lots'].items()}
ALL_LOTS = {**RUNE_LOTS, **EMBER_LOTS}

def i32(data, off): return struct.unpack_from('<i', data, off)[0] if off+4 <= len(data) else None
def u32(data, off): return struct.unpack_from('<I', data, off)[0] if off+4 <= len(data) else None
def f32(data, off): return struct.unpack_from('<f', data, off)[0] if off+4 <= len(data) else None

def parse_emevd(path):
    try:
        raw = SoulsFormats.DCX.Decompress(str(path))
        emevd = _emevd_read.Invoke(None, Array[Object]([raw]))
    except:
        return {}
    events = {}
    for event in emevd.Events:
        eid = int(event.ID)
        instrs = []
        for instr in event.Instructions:
            instrs.append({
                'bank': int(instr.Bank),
                'id': int(instr.ID),
                'args': bytes(instr.ArgData) if instr.ArgData else b'',
            })
        params = []
        for p in event.Parameters:
            params.append({
                'instrIdx': int(p.InstructionIndex),
                'targetStart': int(p.TargetStartByte),
                'sourceStart': int(p.SourceStartByte),
                'byteCount': int(p.ByteCount),
            })
        events[eid] = {'id': eid, 'instructions': instrs, 'params': params}
    return events


# ========================
# STEP 1: Parse common.emevd and analyze event 1045632900 structure
# ========================
print("=" * 70)
print("STEP 1: Analyzing event 1045632900 RunEvent calls")
print("=" * 70)

common_events = parse_emevd(EVENT_DIR / 'common.emevd.dcx')
event_main = common_events.get(1045632900)

# Extract ALL RunEvent calls to 1045630910
runevent_args = []
if event_main:
    for idx, instr in enumerate(event_main['instructions']):
        if instr['bank'] == 2000 and instr['id'] == 0:
            args = instr['args']
            # RunEvent args: slot(i32), eventId(i32), params...
            if len(args) >= 40:  # 10 * 4 bytes
                vals = [i32(args, i*4) for i in range(len(args)//4)]
                if vals[1] == 1045630910:
                    runevent_args.append({
                        'idx': idx,
                        'slot': vals[0],
                        'subEvent1': vals[2],  # 1045631100+
                        'subEvent2': vals[3],  # 1045630100+
                        'lotBase1': vals[4],   # 10100
                        'lotBase2': vals[5],   # 10101
                        'subEvent3': vals[6],  # 1045632100+
                        'subEvent4': vals[7],  # 1045633100+
                        'lotId': vals[8],      # 10102 - our lot!
                        'arg9': vals[9],       # mystery value
                        'allVals': vals,
                    })

print(f"Found {len(runevent_args)} RunEvent(1045630910) calls")
print(f"Sub-event ranges:")
se1 = sorted(set(r['subEvent1'] for r in runevent_args))
se4 = sorted(set(r['subEvent4'] for r in runevent_args))
print(f"  subEvent1 (arg[2]): {min(se1)}-{max(se1)}")
print(f"  subEvent4 (arg[7]): {min(se4)}-{max(se4)}")

# Analyze arg[9] - what is it?
arg9_vals = [r['arg9'] for r in runevent_args]
print(f"\nArg[9] values ({len(arg9_vals)} total):")
print(f"  Range: {min(arg9_vals)} - {max(arg9_vals)}")
print(f"  Unique: {len(set(arg9_vals))}")

# Try to decode arg[9] as map tile encoding
print("\n  As potential map tile encoding (LE bytes):")
for r in runevent_args[:5]:
    a9 = r['arg9']
    if a9 > 1000:
        b = struct.pack('<I', a9 & 0xFFFFFFFF)
        print(f"    lot={r['lotId']}, arg9={a9}, hex={a9:08X}, "
              f"bytes=[{b[0]}, {b[1]}, {b[2]}, {b[3]}]")

# ========================
# STEP 2: Find sub-events in ALL emevd files
# ========================
print("\n" + "=" * 70)
print("STEP 2: Searching ALL EMEVD files for sub-events")
print("=" * 70)

target_event_ids = set()
for r in runevent_args:
    target_event_ids.add(r['subEvent1'])
    target_event_ids.add(r['subEvent4'])

# Also search for the handler itself and related events
target_event_ids.add(1045630910)
target_event_ids.add(1045632900)

# Add range 1045630100-1045630149 (subEvent2)
for r in runevent_args:
    target_event_ids.add(r['subEvent2'])

emevd_files = sorted(EVENT_DIR.glob('*.emevd.dcx'))
found_events = {}  # eventId -> list of {file, numInstr, params}

for emevd_path in emevd_files:
    events = parse_emevd(emevd_path)
    if not events:
        continue
    fname = emevd_path.name.replace('.emevd.dcx', '')
    for eid, e in events.items():
        if eid in target_event_ids:
            if eid not in found_events:
                found_events[eid] = []
            found_events[eid].append({
                'file': fname,
                'numInstr': len(e['instructions']),
                'numParams': len(e['params']),
                'instructions': e['instructions'],
                'params': e['params'],
            })

print(f"Found {len(found_events)} / {len(target_event_ids)} target events across all files")

# Show where each event is defined
for eid in sorted(found_events.keys())[:50]:
    locs = found_events[eid]
    for loc in locs:
        print(f"  Event {eid}: {loc['file']} ({loc['numInstr']} instr, {loc['numParams']} params)")

# ========================
# STEP 3: Analyze event 1045630910 (the handler)
# ========================
print("\n" + "=" * 70)
print("STEP 3: Event 1045630910 detailed analysis")
print("=" * 70)

handler = common_events.get(1045630910)
if handler:
    print(f"  {len(handler['instructions'])} instructions, {len(handler['params'])} parameters")
    print(f"\n  Parameters (template substitutions):")
    for p in handler['params'][:30]:
        print(f"    instrIdx={p['instrIdx']}, target={p['targetStart']}, "
              f"source={p['sourceStart']}, bytes={p['byteCount']}")
    
    print(f"\n  First 40 instructions:")
    for idx, instr in enumerate(handler['instructions'][:40]):
        args = instr['args']
        vals = [i32(args, i*4) for i in range(len(args)//4)] if len(args) >= 4 else []
        print(f"    [{idx:3d}] {instr['bank']:04d}:{instr['id']:02d} "
              f"argLen={len(args)} vals={vals}")

# ========================
# STEP 4: Analyze sub-events 1045631100+ (actual placement?)
# ========================
print("\n" + "=" * 70)
print("STEP 4: Sub-events 1045631100-1045631149 analysis")
print("=" * 70)

for eid in sorted(found_events.keys()):
    if 1045631100 <= eid <= 1045631200:
        for loc in found_events[eid]:
            print(f"\n  Event {eid} in {loc['file']}:")
            print(f"    {loc['numInstr']} instructions, {loc['numParams']} params")
            for p in loc['params']:
                print(f"    Param: instrIdx={p['instrIdx']}, target={p['targetStart']}, "
                      f"source={p['sourceStart']}, bytes={p['byteCount']}")
            for idx, instr in enumerate(loc['instructions'][:20]):
                args = instr['args']
                vals = [i32(args, i*4) for i in range(len(args)//4)]
                # Also try floats
                fvals = [f32(args, i*4) for i in range(len(args)//4)]
                fvals_str = [f'{f:.1f}' for f in fvals if f and 0.1 < abs(f) < 10000]
                print(f"      [{idx:3d}] {instr['bank']:04d}:{instr['id']:02d} "
                      f"vals={vals} floats={fvals_str}")

# ========================
# STEP 5: Try arg[9] as Entity IDs - search MSBs
# ========================
print("\n" + "=" * 70)
print("STEP 5: Searching MSBs for arg[9] as Entity IDs")
print("=" * 70)

# Collect all arg9 values
arg9_to_lots = {}
for r in runevent_args:
    arg9_to_lots[r['arg9']] = r['lotId']

# Also check if arg9 could be a 4-byte entity ID
# Try to find ANY entity IDs matching our arg9 values
msb_files = sorted(MAP_DIR.glob('*.msb.dcx')) if MAP_DIR.exists() else []
if not msb_files:
    msb_files = sorted(MAP_DIR.glob('*.msb'))
print(f"Scanning {len(msb_files)} MSB files...")

entity_matches = {}
for msb_path in msb_files:
    try:
        raw = SoulsFormats.DCX.Decompress(str(msb_path))
        msb = SoulsFormats.MSBE.Read(raw)
    except:
        continue
    
    map_name = msb_path.name.split('.')[0]
    
    for parts_type in ['MapPieces', 'Enemies', 'Players', 'Collisions', 'ConnectCollisions', 'Assets', 'DummyAssets', 'DummyEnemies']:
        parts = getattr(msb.Parts, parts_type, None)
        if parts is None:
            continue
        for p in parts:
            eid = int(p.EntityID) if hasattr(p, 'EntityID') else -1
            if eid in arg9_to_lots:
                lot = arg9_to_lots[eid]
                flag = ALL_LOTS.get(lot, '?')
                pos = (float(p.Position.X), float(p.Position.Y), float(p.Position.Z))
                entity_matches[eid] = {
                    'entityId': eid, 'lotId': lot, 'flag': flag,
                    'map': map_name, 'partType': parts_type, 'partName': str(p.Name),
                    'x': pos[0], 'y': pos[1], 'z': pos[2],
                }
                print(f"  MATCH: entity={eid} -> lot={lot}, flag={flag}, "
                      f"pos=({pos[0]:.1f}, {pos[1]:.1f}, {pos[2]:.1f}) "
                      f"in {map_name} ({parts_type}: {p.Name})")
            
            # Also check EntityGroups
            if hasattr(p, 'EntityGroups'):
                for eg in p.EntityGroups:
                    eg = int(eg)
                    if eg in arg9_to_lots:
                        lot = arg9_to_lots[eg]
                        flag = ALL_LOTS.get(lot, '?')
                        pos = (float(p.Position.X), float(p.Position.Y), float(p.Position.Z))
                        print(f"  MATCH (group): entityGroup={eg} -> lot={lot}, flag={flag}, "
                              f"pos=({pos[0]:.1f}, {pos[1]:.1f}, {pos[2]:.1f}) "
                              f"in {map_name} ({parts_type}: {p.Name})")

print(f"\nEntity ID matches: {len(entity_matches)}")

# ========================
# STEP 6: Alternative - try to find events with 2003:04 (AwardItemLot) that 
# reference our lots AND have coordinates in the same event
# ========================
print("\n" + "=" * 70)
print("STEP 6: Looking for AwardItemLot + coordinates in same event")
print("=" * 70)

# Common ER EMEVD instructions:
# 2003:04 = AwardItemLot(lotId)
# 2003:36 = AwardItemsIncludingClients(lotId)
# 2004:01 = SpawnObjAct(entityId, ...) 
# 2004:44 = CreateMapPoint(x, y, z, ...)

for emevd_path in emevd_files:
    events = parse_emevd(emevd_path)
    if not events:
        continue
    fname = emevd_path.name.replace('.emevd.dcx', '')
    
    for eid, e in events.items():
        lot_instrs = []
        coord_candidates = []
        entity_refs = []
        
        for idx, instr in enumerate(e['instructions']):
            args = instr['args']
            
            # Check for AwardItemLot instructions
            if instr['bank'] == 2003 and instr['id'] in (4, 36):
                if len(args) >= 4:
                    lot = i32(args, 0)
                    if lot in ALL_LOTS:
                        lot_instrs.append({'idx': idx, 'lot': lot, 'flag': ALL_LOTS[lot]})
            
            # Check for any instruction with float coordinates
            if len(args) >= 12:
                for off in range(0, len(args)-11, 4):
                    x, y, z = f32(args, off), f32(args, off+4), f32(args, off+8)
                    if (x and y is not None and z and 
                        1.0 < abs(x) < 5000 and abs(y) < 1000 and 1.0 < abs(z) < 5000):
                        coord_candidates.append({
                            'idx': idx, 'off': off,
                            'x': x, 'y': y, 'z': z,
                            'bank': instr['bank'], 'iid': instr['id']
                        })
        
        if lot_instrs and coord_candidates:
            print(f"\n  {fname} event {eid}:")
            for li in lot_instrs:
                print(f"    AwardItemLot: lot={li['lot']}, flag={li['flag']}")
            for cc in coord_candidates[:5]:
                print(f"    Coords at [{cc['idx']}] {cc['bank']:04d}:{cc['iid']:02d}: "
                      f"({cc['x']:.1f}, {cc['y']:.1f}, {cc['z']:.1f})")

print("\nDone!")
