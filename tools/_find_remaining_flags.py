#!/usr/bin/env python3
"""
Find collected flags for the 28 uncovered AEG099_510 positions.
Strategy: search per-map EMEVD files for any instruction referencing
these EntityIDs, then find the flag-setting instructions in those events.
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

def i32(data, off): return struct.unpack_from('<i', data, off)[0] if off+4 <= len(data) else None

# The 28 uncovered entity IDs (excluding m11_10_00_00 cache)
UNCOVERED = {
    11001692, 11051680,
    12021203, 12021204, 12021205, 12021670,
    12031500,
    14001660, 14001670,
    16001570,
    34101600, 34101620,
    1033461610, 1033471610, 1034471610, 1034511620,
    1036451620, 1039421610, 1040521640,
    1046391600, 1047381610, 1048571500,
    1050361610, 1050361612, 1050561510,
    1051361600, 1051361602, 1051371600, 1051431610,
}

print(f"Searching for {len(UNCOVERED)} uncovered EntityIDs")

# Search ALL EMEVD files for references to these entity IDs
emevd_files = sorted(EVENT_DIR.glob('*.emevd.dcx'))

results = {}  # entityId -> [{file, eventId, instrIdx, bank, id, allVals}]

for emevd_path in emevd_files:
    try:
        raw = SoulsFormats.DCX.Decompress(str(emevd_path))
        emevd = _emevd_read.Invoke(None, Array[Object]([raw]))
    except:
        continue
    
    fname = emevd_path.name.replace('.emevd.dcx', '')
    
    for event in emevd.Events:
        eid = int(event.ID)
        
        for idx, instr in enumerate(event.Instructions):
            args = bytes(instr.ArgData) if instr.ArgData else b''
            bank = int(instr.Bank)
            iid = int(instr.ID)
            
            # Scan all i32 values for our entity IDs
            for off in range(0, len(args)-3, 4):
                v = i32(args, off)
                if v in UNCOVERED:
                    vals = [i32(args, i*4) for i in range(len(args)//4)]
                    
                    if v not in results:
                        results[v] = []
                    results[v].append({
                        'file': fname,
                        'eventId': eid,
                        'instrIdx': idx,
                        'bank': bank,
                        'id': iid,
                        'vals': vals,
                    })

print(f"Found references for {len(results)} / {len(UNCOVERED)} entity IDs")

for eid_val in sorted(results.keys()):
    refs = results[eid_val]
    print(f"\n  EntityID {eid_val}:")
    for r in refs[:10]:
        print(f"    {r['file']} event {r['eventId']} [{r['instrIdx']}] "
              f"{r['bank']:04d}:{r['id']:02d} vals={r['vals']}")

# For each entity, look at the FULL event that references it
# to find the flag-setting instruction (2003:66 or similar)
print("\n" + "=" * 70)
print("Analyzing events for flag patterns")
print("=" * 70)

# Group by event
events_to_analyze = set()
for eid_val, refs in results.items():
    for r in refs:
        events_to_analyze.add((r['file'], r['eventId']))

print(f"Events to analyze: {len(events_to_analyze)}")

for emevd_path in emevd_files:
    try:
        raw = SoulsFormats.DCX.Decompress(str(emevd_path))
        emevd = _emevd_read.Invoke(None, Array[Object]([raw]))
    except:
        continue
    
    fname = emevd_path.name.replace('.emevd.dcx', '')
    
    for event in emevd.Events:
        eid = int(event.ID)
        if (fname, eid) not in events_to_analyze:
            continue
        
        # Find which entity IDs this event references
        referenced_eids = set()
        for instr in event.Instructions:
            args = bytes(instr.ArgData) if instr.ArgData else b''
            for off in range(0, len(args)-3, 4):
                v = i32(args, off)
                if v in UNCOVERED:
                    referenced_eids.add(v)
        
        if not referenced_eids:
            continue
        
        # Look for flag-related instructions in this event
        # 2003:66 = SetEventFlag(target, flagId, state)
        # 2003:22 = SetEventFlag (different format)
        # 0003:00 = IfEventFlag(state, flagId)
        flag_instrs = []
        all_vals_in_event = set()
        
        for idx, instr in enumerate(event.Instructions):
            args = bytes(instr.ArgData) if instr.ArgData else b''
            bank = int(instr.Bank)
            iid = int(instr.ID)
            vals = [i32(args, i*4) for i in range(len(args)//4)]
            
            for v in vals:
                if v:
                    all_vals_in_event.add(v)
            
            if bank == 2003 and iid == 66:
                flag_instrs.append({'idx': idx, 'type': '2003:66', 'vals': vals})
            elif bank == 2003 and iid == 22:
                flag_instrs.append({'idx': idx, 'type': '2003:22', 'vals': vals})
        
        # Also look for RunEvent(2000:00) calls that might pass this entity
        run_events = []
        for idx, instr in enumerate(event.Instructions):
            args = bytes(instr.ArgData) if instr.ArgData else b''
            if int(instr.Bank) == 2000 and int(instr.ID) == 0:
                vals = [i32(args, i*4) for i in range(len(args)//4)]
                has_eid = any(v in UNCOVERED for v in vals)
                if has_eid:
                    run_events.append({'idx': idx, 'vals': vals})
        
        print(f"\n  {fname} event {eid} (refs: {sorted(referenced_eids)}):")
        print(f"    Total instructions: {len(list(event.Instructions))}")
        if flag_instrs:
            print(f"    Flag instructions:")
            for fi in flag_instrs[:5]:
                print(f"      [{fi['idx']}] {fi['type']} vals={fi['vals']}")
        if run_events:
            print(f"    RunEvent calls with entity IDs:")
            for re in run_events[:10]:
                # Try to identify the handler pattern
                vals = re['vals']
                if len(vals) >= 10:
                    entity_ids = [v for v in vals if v in UNCOVERED]
                    print(f"      [{re['idx']}] handler={vals[1]} entityIds={entity_ids} "
                          f"vals={vals}")
                else:
                    print(f"      [{re['idx']}] vals={vals}")

        # Check for parameters (template substitution)
        params = list(event.Parameters)
        if params:
            print(f"    Parameters ({len(params)}):")
            for p in params[:5]:
                print(f"      instrIdx={int(p.InstructionIndex)} target={int(p.TargetStartByte)} "
                      f"source={int(p.SourceStartByte)} bytes={int(p.ByteCount)}")

print("\nDone!")
