#!/usr/bin/env python3
"""
Decode Rune/Ember Piece placements from EMEVD.
Key insight: arg[9] in RunEvent(1045630910) encodes map tile as 4 LE bytes: [area, tileX, tileY, tileZ]
e.g. 2370108 → [60, 42, 36, 0] → m60_42_36_00

Strategy:
1. Decode all 50 RunEvent calls to get map tile per lot ID
2. Dump the FULL instruction set of event 1045630910 (all 58 instructions)
3. Search MSB files in matching tiles for Assets with model IDs related to Rune Pieces
4. Cross-reference with existing MASSEDIT coordinates
"""
import sys, io, struct, json, re
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
MASSEDIT_DIR = Path(__file__).parent.parent / 'addons' / 'massedit'

diag = json.load(open(DATA_DIR / '_pieces_diagnostic.json', 'r', encoding='utf-8'))
RUNE_LOTS = {int(k): v for k, v in diag['rune_lots'].items()}
EMBER_LOTS = {int(k): v for k, v in diag['ember_lots'].items()}
ALL_LOTS = {**RUNE_LOTS, **EMBER_LOTS}

def i32(data, off): return struct.unpack_from('<i', data, off)[0] if off+4 <= len(data) else None
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

def decode_map_tile(val):
    """Decode arg[9] into map tile string."""
    b = struct.pack('<I', val & 0xFFFFFFFF)
    area, tx, ty, tz = b[0], b[1], b[2], b[3]
    return f"m{area:02d}_{tx:02d}_{ty:02d}_{tz:02d}"


# ========================
# STEP 1: Decode all RunEvent(1045630910) calls
# ========================
print("=" * 70)
print("STEP 1: Decode all RunEvent(1045630910) calls from event 1045632900")
print("=" * 70)

common_events = parse_emevd(EVENT_DIR / 'common.emevd.dcx')
event_main = common_events.get(1045632900)

piece_placements = []
if event_main:
    for idx, instr in enumerate(event_main['instructions']):
        if instr['bank'] == 2000 and instr['id'] == 0:
            args = instr['args']
            if len(args) >= 40:
                vals = [i32(args, i*4) for i in range(len(args)//4)]
                if vals[1] == 1045630910:
                    map_tile = decode_map_tile(vals[9])
                    lot_id = vals[8]
                    flag = ALL_LOTS.get(lot_id, 0)
                    piece_type = 'rune' if lot_id in RUNE_LOTS else ('ember' if lot_id in EMBER_LOTS else 'unknown')
                    
                    p = {
                        'slot': vals[0],
                        'lotId': lot_id,
                        'flag': flag,
                        'type': piece_type,
                        'mapTile': map_tile,
                        'subEvent1': vals[2],
                        'subEvent2': vals[3],
                        'lotBase1': vals[4],
                        'lotBase2': vals[5],
                        'subEvent3': vals[6],
                        'subEvent4': vals[7],
                        'arg9raw': vals[9],
                    }
                    piece_placements.append(p)

print(f"Decoded {len(piece_placements)} placements:")
for p in piece_placements:
    print(f"  slot={p['slot']:2d} lot={p['lotId']:5d} flag={p['flag']} "
          f"tile={p['mapTile']} type={p['type']}")

# Count per map tile
tile_counts = {}
for p in piece_placements:
    tile_counts[p['mapTile']] = tile_counts.get(p['mapTile'], 0) + 1
print(f"\nPlacements per tile:")
for tile, count in sorted(tile_counts.items()):
    print(f"  {tile}: {count}")


# ========================
# STEP 2: Find ALL events that handle piece placement (not just 1045632900)
# ========================
print("\n" + "=" * 70)
print("STEP 2: Find ALL RunEvent calls targeting piece handler events")
print("=" * 70)

# Search for other orchestrator events that call 1045630910 or similar handlers
all_piece_events = []
for eid, e in common_events.items():
    for instr in e['instructions']:
        if instr['bank'] == 2000 and instr['id'] == 0:
            args = instr['args']
            if len(args) >= 8:
                called = i32(args, 4)
                if called == 1045630910 and eid != 1045632900:
                    all_piece_events.append(eid)

print(f"Other events calling 1045630910: {all_piece_events}")

# Also look at events 1042612000, 1042612010-12 which are the tier orchestrators
for target_eid in [1042612000, 1042612010, 1042612011, 1042612012]:
    e = common_events.get(target_eid)
    if e:
        print(f"\n  Event {target_eid}: {len(e['instructions'])} instructions, {len(e['params'])} params")
        for idx, instr in enumerate(e['instructions'][:20]):
            args = instr['args']
            vals = [i32(args, i*4) for i in range(len(args)//4)]
            print(f"    [{idx:3d}] {instr['bank']:04d}:{instr['id']:02d} vals={vals}")


# ========================
# STEP 3: Look for ALL RunEvent calls in common.emevd that pass our lot IDs
# ========================
print("\n" + "=" * 70)
print("STEP 3: ALL RunEvent calls with lot IDs in common.emevd")
print("=" * 70)

all_lot_calls = []
for eid, e in common_events.items():
    for idx, instr in enumerate(e['instructions']):
        if instr['bank'] == 2000 and instr['id'] in (0, 6):
            args = instr['args']
            for off in range(0, len(args)-3, 4):
                v = i32(args, off)
                if v in ALL_LOTS:
                    all_lot_calls.append({
                        'eventId': eid,
                        'instrIdx': idx,
                        'instrType': f"2000:{instr['id']:02d}",
                        'lotId': v,
                        'flag': ALL_LOTS[v],
                    })
                    break

# Deduplicate by eventId+lotId
seen = set()
unique_calls = []
for c in all_lot_calls:
    key = (c['eventId'], c['lotId'])
    if key not in seen:
        seen.add(key)
        unique_calls.append(c)

print(f"Found {len(unique_calls)} unique event+lot combinations:")
by_event = {}
for c in unique_calls:
    by_event.setdefault(c['eventId'], []).append(c)

for eid, calls in sorted(by_event.items()):
    lots = [c['lotId'] for c in calls]
    if len(lots) > 5:
        print(f"  Event {eid}: {len(lots)} lots: {lots[:5]}...")
    else:
        print(f"  Event {eid}: {len(lots)} lots: {lots}")


# ========================  
# STEP 4: Full instruction dump of event 1045630910
# ========================
print("\n" + "=" * 70)
print("STEP 4: Full event 1045630910 instruction dump")
print("=" * 70)

handler = common_events.get(1045630910)
if handler:
    print(f"Parameters ({len(handler['params'])}):")
    for p in handler['params']:
        # Map source offset to parameter name
        param_names = {
            0: 'subEvent1', 4: 'subEvent2', 8: 'lotBase1', 12: 'lotBase2',
            16: 'subEvent3', 20: 'subEvent4', 24: 'lotId', 28: 'mapTile(b0)',
            29: 'mapTile(b1)', 30: 'mapTile(b2)', 31: 'mapTile(b3)'
        }
        name = param_names.get(p['sourceStart'], f"param@{p['sourceStart']}")
        print(f"  instr[{p['instrIdx']}].byte[{p['targetStart']}:{p['targetStart']+p['byteCount']}] "
              f"<= {name}")
    
    print(f"\nAll {len(handler['instructions'])} instructions:")
    for idx, instr in enumerate(handler['instructions']):
        args = instr['args']
        vals = [i32(args, i*4) for i in range(len(args)//4)]
        fvals = []
        for v in vals:
            if v and abs(v) > 100:
                fv = f32(args, vals.index(v)*4)
                if fv and 0.01 < abs(fv) < 100000:
                    fvals.append(f'{fv:.2f}')
        
        # Check if any params substitute into this instruction
        param_subs = [p for p in handler['params'] if p['instrIdx'] == idx]
        sub_str = ''
        if param_subs:
            subs = [f"{param_names.get(p['sourceStart'], '?')}->byte[{p['targetStart']}]" 
                    for p in param_subs]
            sub_str = f"  PARAMS: {', '.join(subs)}"
        
        print(f"  [{idx:3d}] {instr['bank']:04d}:{instr['id']:02d} "
              f"argLen={len(args):2d} vals={vals}{sub_str}")


# ========================
# STEP 5: Search MSB files for Rune Piece model IDs
# ========================
print("\n" + "=" * 70)
print("STEP 5: Search for Rune Piece asset models in MSB files")
print("=" * 70)

# AEG099 assets are commonly used for item pickups
# Let's look at assets in our target map tiles
target_tiles = set(p['mapTile'] for p in piece_placements)
print(f"Target tiles: {sorted(target_tiles)}")

# Check a few tiles to understand asset naming patterns
rune_model_candidates = set()
for tile in sorted(target_tiles)[:5]:
    msb_path = MAP_DIR / f'{tile}.msb.dcx'
    if not msb_path.exists():
        msb_path = MAP_DIR / f'{tile}.msb'
    if not msb_path.exists():
        continue
    
    try:
        raw = SoulsFormats.DCX.Decompress(str(msb_path))
        msb = SoulsFormats.MSBE.Read(raw)
    except:
        continue
    
    print(f"\n  {tile}:")
    # List all assets and their models
    for parts_type in ['Assets', 'DummyAssets']:
        parts = getattr(msb.Parts, parts_type, None)
        if parts is None:
            continue
        for p in parts:
            name = str(p.Name)
            model = str(p.ModelName) if hasattr(p, 'ModelName') else '?'
            eid = int(p.EntityID) if hasattr(p, 'EntityID') else -1
            pos = (float(p.Position.X), float(p.Position.Y), float(p.Position.Z))
            
            # Look for item-pickup related assets (AEG099, AEG027, etc.)
            if any(k in model.lower() for k in ['099', '027', 'item', 'rune', 'piece']):
                print(f"    {parts_type}: {name} model={model} eid={eid} "
                      f"pos=({pos[0]:.1f}, {pos[1]:.1f}, {pos[2]:.1f})")
                rune_model_candidates.add(model)

    # Also look at Enemies with model c1000 (invisible helper entities)
    for p in msb.Parts.Enemies:
        name = str(p.Name)
        model = str(p.ModelName) if hasattr(p, 'ModelName') else '?'
        eid = int(p.EntityID) if hasattr(p, 'EntityID') else -1
        if 'c1000' in model or 'c0000' in model:
            pos = (float(p.Position.X), float(p.Position.Y), float(p.Position.Z))
            egroups = [int(eg) for eg in p.EntityGroups if int(eg) > 0] if hasattr(p, 'EntityGroups') else []
            if egroups:
                print(f"    Enemy: {name} model={model} eid={eid} groups={egroups[:5]} "
                      f"pos=({pos[0]:.1f}, {pos[1]:.1f}, {pos[2]:.1f})")

if rune_model_candidates:
    print(f"\nRune piece model candidates: {sorted(rune_model_candidates)}")


# ========================
# STEP 6: Parse existing MASSEDIT to get current coordinates
# ========================
print("\n" + "=" * 70)
print("STEP 6: Parse existing MASSEDIT Rune Piece entries")  
print("=" * 70)

massedit_files = list(MASSEDIT_DIR.glob('*Rune*')) + list(MASSEDIT_DIR.glob('*rune*'))
if not massedit_files:
    massedit_files = list(MASSEDIT_DIR.glob('*Piece*')) + list(MASSEDIT_DIR.glob('*piece*'))

print(f"Found MASSEDIT files: {[f.name for f in massedit_files]}")

massedit_entries = []
for mf in massedit_files:
    text = mf.read_text(encoding='utf-8')
    # Parse MASSEDIT format - each entry is a set of field: value lines
    current_entry = {}
    for line in text.split('\n'):
        line = line.strip()
        if not line or line.startswith('//') or line.startswith('#'):
            continue
        # MASSEDIT format: "param WorldMapPointParam: id XXXX: field value;"
        m = re.match(r'param WorldMapPointParam:\s*id\s+(\d+):\s*(\w+)\s+(.+?);', line)
        if m:
            row_id = int(m.group(1))
            field = m.group(2)
            value = m.group(3).strip()
            
            if current_entry and current_entry.get('id') != row_id:
                if current_entry.get('id'):
                    massedit_entries.append(current_entry)
                current_entry = {'id': row_id}
            elif not current_entry.get('id'):
                current_entry = {'id': row_id}
            
            current_entry[field] = value
    
    if current_entry.get('id'):
        massedit_entries.append(current_entry)

print(f"Parsed {len(massedit_entries)} MASSEDIT entries")
if massedit_entries:
    print(f"  Sample entry: {massedit_entries[0]}")
    
    # Extract unique field names
    fields = set()
    for e in massedit_entries:
        fields.update(e.keys())
    print(f"  Fields: {sorted(fields)}")
    
    # Check for existing textDisableFlagId
    has_flags = sum(1 for e in massedit_entries if e.get('textDisableFlagId', '0') != '0')
    print(f"  Entries with textDisableFlagId != 0: {has_flags}")
    
    # Show a few entries with their positions
    print(f"\n  Sample coordinates:")
    for e in massedit_entries[:10]:
        x = e.get('posX', '?')
        z = e.get('posZ', '?')
        area = e.get('areaNo', '?')
        print(f"    id={e['id']}, posX={x}, posZ={z}, areaNo={area}")

print("\nDone!")
