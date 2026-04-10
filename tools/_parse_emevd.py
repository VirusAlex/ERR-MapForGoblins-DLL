#!/usr/bin/env python3
"""
Parse EMEVD files to find instructions referencing Rune/Ember Piece ItemLot IDs.
EMEVD instructions are identified by (bank, index) pairs.
Known relevant instructions for item spawning:
  2003[04] = AwardItemLot(item_lot_id)
  2003[36] = AwardItemsIncludingClients(item_lot_id)  
  2003[69] = SpawnObjTreasure(asset_id, item_lot_id)
"""
import sys, io, struct
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

# Target lot IDs (small ones for Rune/Ember pieces)
RUNE_LOTS = set()
EMBER_LOTS = set()

# Load the lot IDs from regulation (reuse previous results)
import json
diag_path = Path(__file__).parent.parent / 'data' / '_pieces_diagnostic.json'
if diag_path.exists():
    diag = json.load(open(diag_path, 'r', encoding='utf-8'))
    RUNE_LOTS = {int(k) for k in diag['rune_lots'].keys()}
    EMBER_LOTS = {int(k) for k in diag['ember_lots'].keys()}
ALL_LOTS = RUNE_LOTS | EMBER_LOTS

print(f"Target lots: {len(RUNE_LOTS)} rune, {len(EMBER_LOTS)} ember")

def unpack_args(arg_bytes, arg_types):
    """Unpack instruction arguments based on ArgType list."""
    result = []
    offset = 0
    raw = bytes(arg_bytes)
    for at in arg_types:
        at_val = int(at)
        if at_val == 0:  # u8
            if offset < len(raw):
                result.append(raw[offset])
                offset += 1
        elif at_val == 1:  # u16
            if offset + 2 <= len(raw):
                result.append(struct.unpack_from('<H', raw, offset)[0])
                offset += 2
        elif at_val == 2:  # u32
            if offset + 4 <= len(raw):
                result.append(struct.unpack_from('<I', raw, offset)[0])
                offset += 4
        elif at_val == 3:  # i8
            if offset < len(raw):
                result.append(struct.unpack_from('<b', raw, offset)[0])
                offset += 1
        elif at_val == 4:  # i16
            if offset + 2 <= len(raw):
                result.append(struct.unpack_from('<h', raw, offset)[0])
                offset += 2
        elif at_val == 5:  # i32
            if offset + 4 <= len(raw):
                result.append(struct.unpack_from('<i', raw, offset)[0])
                offset += 4
        elif at_val == 6:  # f32
            if offset + 4 <= len(raw):
                result.append(struct.unpack_from('<f', raw, offset)[0])
                offset += 4
        elif at_val == 8:  # u64
            if offset + 8 <= len(raw):
                result.append(struct.unpack_from('<Q', raw, offset)[0])
                offset += 8
        elif at_val == 9:  # i64
            if offset + 8 <= len(raw):
                result.append(struct.unpack_from('<q', raw, offset)[0])
                offset += 8
        else:
            result.append(f'?type{at_val}')
            offset += 4
    return result

# First pass: scan all EMEVD files, dump all instruction types that reference our lot IDs
print("\n=== Scanning EMEVD files ===")
emevd_files = sorted(EVENT_DIR.glob('*.emevd.dcx'))
print(f"{len(emevd_files)} EMEVD files")

all_findings = []
instr_types_seen = set()

for emevd_path in emevd_files:
    try:
        raw = SoulsFormats.DCX.Decompress(str(emevd_path))
        emevd = _emevd_read.Invoke(None, Array[Object]([raw]))
    except Exception as e:
        print(f"  ERROR: {emevd_path.name}: {e}")
        continue

    map_name = emevd_path.name.replace('.emevd.dcx', '')

    for event in emevd.Events:
        event_id = int(event.ID)
        
        for instr in event.Instructions:
            bank = int(instr.Bank)
            instr_id = int(instr.ID)
            
            # Get raw argument bytes
            arg_data = bytes(instr.ArgData) if instr.ArgData else b''
            
            # Check if any of our lot IDs appear in raw bytes (brute force 4-byte scan)
            found_lots = set()
            for i in range(0, len(arg_data) - 3):
                val = struct.unpack_from('<i', arg_data, i)[0]
                if val in ALL_LOTS:
                    found_lots.add(val)
            
            if found_lots:
                instr_types_seen.add((bank, instr_id))
                for lot_id in found_lots:
                    is_rune = lot_id in RUNE_LOTS
                    finding = {
                        'map': map_name,
                        'eventId': event_id,
                        'bank': bank,
                        'index': instr_id,
                        'lotId': lot_id,
                        'type': 'rune' if is_rune else 'ember',
                        'argHex': arg_data.hex(),
                        'argLen': len(arg_data),
                    }
                    all_findings.append(finding)

print(f"\nFound {len(all_findings)} instruction references to our lot IDs")
print(f"Unique instruction types: {sorted(instr_types_seen)}")

# Group by event
events_with_lots = {}
for f in all_findings:
    key = (f['map'], f['eventId'])
    if key not in events_with_lots:
        events_with_lots[key] = []
    events_with_lots[key].append(f)

print(f"Across {len(events_with_lots)} unique events")

# Print findings
for (map_name, event_id), findings in sorted(events_with_lots.items()):
    lots_str = ', '.join(f"{f['lotId']}({f['type'][0]})" for f in findings[:5])
    if len(findings) > 5:
        lots_str += f"... +{len(findings)-5} more"
    instr_key = f"{findings[0]['bank']}:{findings[0]['index']:02d}"
    print(f"  {map_name} event {event_id}: [{instr_key}] {lots_str}")
    if len(findings) <= 3:
        for f in findings:
            print(f"    argHex={f['argHex']}")

# Now let's look at common.emevd specifically - it likely has the shared spawn logic
print("\n=== Detailed look at common.emevd ===")
try:
    raw = SoulsFormats.DCX.Decompress(str(EVENT_DIR / 'common.emevd.dcx'))
    emevd = _emevd_read.Invoke(None, Array[Object]([raw]))
    
    for event in emevd.Events:
        event_id = int(event.ID)
        
        # Check if this event references our lots
        event_has_lots = False
        for instr in event.Instructions:
            arg_data = bytes(instr.ArgData) if instr.ArgData else b''
            for i in range(0, len(arg_data) - 3):
                val = struct.unpack_from('<i', arg_data, i)[0]
                if val in ALL_LOTS:
                    event_has_lots = True
                    break
            if event_has_lots:
                break
        
        if event_has_lots:
            print(f"\n  Event {event_id} ({len(event.Instructions)} instructions, "
                  f"restBehavior={event.RestBehavior}):")
            print(f"  Parameters: {event.Parameters.Count}")
            for param in event.Parameters:
                print(f"    Param: instrIdx={param.InstructionIndex}, "
                      f"targetStartByte={param.TargetStartByte}, "
                      f"sourceStartByte={param.SourceStartByte}, "
                      f"byteCount={param.ByteCount}")
            
            for idx, instr in enumerate(event.Instructions):
                bank = int(instr.Bank)
                iid = int(instr.ID)
                arg_data = bytes(instr.ArgData) if instr.ArgData else b''
                print(f"    [{idx}] {bank}:{iid:02d} args({len(arg_data)}b): {arg_data.hex()}")
                if len(arg_data) >= 4:
                    # Try to interpret as i32 values
                    vals = []
                    for i in range(0, len(arg_data) - 3, 4):
                        vals.append(struct.unpack_from('<i', arg_data, i)[0])
                    print(f"         as i32: {vals}")

except Exception as e:
    print(f"ERROR: {e}")

# Save
out_path = Path(__file__).parent.parent / 'data' / '_emevd_findings.json'
with open(out_path, 'w', encoding='utf-8') as f:
    json.dump(all_findings, f, indent=2, ensure_ascii=False)
print(f"\nSaved {len(all_findings)} findings to {out_path}")
