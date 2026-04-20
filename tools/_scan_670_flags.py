#!/usr/bin/env python3
"""
Scan ALL EMEVD files (mod + game) for references to flags in range 670000-670999.
These are summoning pool (Martyr Effigy) activation flags.

Dumps full event structure for every event that references any 670XXX value.
"""
import sys, io, struct, json
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
from pathlib import Path
import config
from pythonnet import load
load('coreclr')
import clr
from System.Reflection import Assembly
from System import Array, Type as SysType, Object, String, Byte

asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
clr.AddReference(str(config.SOULSFORMATS_DLL))
import SoulsFormats

# Use reflection for Read(string) since direct call doesn't work with pythonnet
_emevd_cls = asm.GetType('SoulsFormats.EMEVD')
_str_type = SysType.GetType('System.String')
_read_method = _emevd_cls.BaseType.GetMethod('Read', Array[SysType]([_str_type]))

FLAG_MIN = 670000
FLAG_MAX = 670999

def i32(data, off):
    return struct.unpack_from('<i', data, off)[0] if off+4 <= len(data) else None

def u32(data, off):
    return struct.unpack_from('<I', data, off)[0] if off+4 <= len(data) else None

def f32(data, off):
    return struct.unpack_from('<f', data, off)[0] if off+4 <= len(data) else None

def u8(data, off):
    return data[off] if off < len(data) else None

def u16(data, off):
    return struct.unpack_from('<H', data, off)[0] if off+2 <= len(data) else None

def has_670_value(data):
    """Check if raw bytes contain any 4-byte aligned int in 670000-670999 range."""
    for i in range(0, len(data) - 3, 4):
        val = struct.unpack_from('<i', data, i)[0]
        if FLAG_MIN <= val <= FLAG_MAX:
            return True
    return False

def find_670_values(data):
    """Return all (offset, value) pairs where a 4-byte aligned int is in 670000-670999."""
    results = []
    for i in range(0, len(data) - 3, 4):
        val = struct.unpack_from('<i', data, i)[0]
        if FLAG_MIN <= val <= FLAG_MAX:
            results.append((i, val))
    return results

def parse_emevd(path):
    """Parse an EMEVD file using reflection-based Read(string)."""
    try:
        emevd = _read_method.Invoke(None, Array[Object]([String(str(path))]))
    except Exception as e:
        print(f"  ERROR parsing {path.name}: {e}")
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
        events[eid] = {
            'id': eid,
            'restBehavior': int(event.RestBehavior),
            'instructions': instrs,
            'params': params,
        }
    return events

# Instruction name lookup
INSTR_NAMES = {
    (0, 0): "IfElimConditionGroup",
    (0, 1): "IfConditionGroup",
    (1, 0): "IfElapsedSeconds",
    (1, 3): "IfElapsedFrames",
    (3, 0): "IfEventFlag(state,flagType,flagId)",
    (3, 1): "IfEventFlagRange",
    (3, 2): "IfEventFlagRangeAllState",
    (3, 3): "IfEventFlagRangeAnyState",
    (3, 5): "IfPlayerHasItem",
    (3, 24): "IfMapLoaded",
    (4, 0): "IfPlayerInsideRegion",
    (4, 1): "IfPlayerOutsideRegion",
    (4, 15): "IfEntityDistFromPlayer",
    (1000, 0): "WaitForConditionGroup",
    (1000, 1): "SkipIfConditionGroup",
    (1000, 2): "EndIfConditionGroup",
    (1000, 3): "SkipIfEventFlag",
    (1000, 4): "EndIfEventFlag",
    (1000, 5): "SkipIfRangeState",
    (1000, 7): "SkipUnconditionally",
    (1000, 8): "Goto",
    (1000, 9): "GotoIfConditionGroup",
    (1000, 11): "GotoIfEventFlag",
    (1000, 101): "GotoIfConditionGroup_Log",
    (1000, 107): "SkipIfMultiplayer",
    (1003, 1): "IfMultiplayerState",
    (1003, 2): "IfPlayerInArea",
    (2000, 0): "RunEvent",
    (2000, 6): "RunCommonEvent",
    (2003, 2): "SetEventFlagID",
    (2003, 4): "AwardItemLot",
    (2003, 12): "SetMapLoadToggle",
    (2003, 22): "SetEventFlagRange",
    (2003, 36): "AwardItemsIncludingClients",
    (2003, 41): "EventValueOperation",
    (2003, 66): "SetEventFlag",
    (2003, 69): "SpawnObjTreasure",
    (2004, 1): "SetAssetState",
    (2004, 48): "SetAssetActivation",
    (2005, 1): "DestroyAsset",
    (2005, 14): "SetAssetObjActState",
    (2007, 1): "DisplayDialogBox",
    (2007, 9): "DisplayAreaBanner",
    (2009, 4): "SetNetworkSync",
    (2009, 6): "SetCharacterNetworkSync",
    (2010, 2): "PlaySoundEffect",
    (2010, 4): "SetSoundActive",
    (2011, 1): "SetMapPieceVisibility",
    (2012, 1): "SetObjectBackreadState",
}

def instr_name(bank, iid):
    return INSTR_NAMES.get((bank, iid), f"???_{bank:04d}:{iid:03d}")


def dump_event(eid, event, file_name, indent="  "):
    """Print full event structure."""
    print(f"\n{indent}{'='*60}")
    print(f"{indent}Event {eid} in {file_name}")
    print(f"{indent}  RestBehavior: {event['restBehavior']}")
    print(f"{indent}  Instructions: {len(event['instructions'])}")
    print(f"{indent}  Parameters: {len(event['params'])}")

    if event['params']:
        print(f"{indent}  Parameter templates:")
        for p in event['params']:
            print(f"{indent}    instrIdx={p['instrIdx']}, target={p['targetStart']}, "
                  f"source={p['sourceStart']}, bytes={p['byteCount']}")

    print(f"{indent}  Instructions:")
    for idx, instr in enumerate(event['instructions']):
        bank = instr['bank']
        iid = instr['id']
        args = instr['args']
        name = instr_name(bank, iid)

        # Decode args as i32 values
        i32_vals = []
        for off in range(0, (len(args) // 4) * 4, 4):
            i32_vals.append(i32(args, off))

        # Mark which i32 values are in 670XXX range
        flag_markers = []
        for v in i32_vals:
            if v is not None and FLAG_MIN <= v <= FLAG_MAX:
                flag_markers.append(f">>>{v}<<<")
            else:
                flag_markers.append(str(v))

        # Check if this instruction has parameter substitutions
        param_subs = [p for p in event['params'] if p['instrIdx'] == idx]
        param_str = ""
        if param_subs:
            param_str = " PARAM:" + ",".join(
                f"[t{p['targetStart']}<-s{p['sourceStart']}x{p['byteCount']}]"
                for p in param_subs
            )

        line = f"{indent}    [{idx:3d}] [{bank:04d}:{iid:03d}] {name}"
        if i32_vals:
            line += f"\n{indent}          i32: [{', '.join(flag_markers)}]"
        if len(args) % 4 != 0:
            line += f"\n{indent}          raw({len(args)}b): {args.hex()}"
        if param_str:
            line += f"\n{indent}         {param_str}"

        print(line)


# ================================================================
# MAIN: Scan both mod and game event directories
# ================================================================
mod_event_dir = Path(config.require_err_mod_dir()) / 'event'
game_event_dir = Path(config.require_game_dir()) / 'event'

sources = [
    ("MOD", mod_event_dir),
    ("GAME", game_event_dir),
]

all_670_events = {}  # (source, filename, eventId) -> event data
all_670_flags = set()

for source_name, event_dir in sources:
    print(f"\n{'#'*70}")
    print(f"# Scanning {source_name}: {event_dir}")
    print(f"{'#'*70}")

    emevd_files = sorted(event_dir.glob('*.emevd.dcx'))
    print(f"Found {len(emevd_files)} EMEVD files")

    for emevd_path in emevd_files:
        events = parse_emevd(emevd_path)
        if not events:
            continue
        fname = emevd_path.name.replace('.emevd.dcx', '')

        for eid, event in events.items():
            event_has_670 = False
            for instr in event['instructions']:
                if has_670_value(instr['args']):
                    event_has_670 = True
                    for _, val in find_670_values(instr['args']):
                        all_670_flags.add(val)

            if event_has_670:
                key = (source_name, fname, eid)
                all_670_events[key] = event

print(f"\n{'='*70}")
print(f"SUMMARY: Found {len(all_670_events)} events referencing 670XXX flags")
print(f"Unique 670XXX flags seen: {sorted(all_670_flags)}")
if all_670_flags:
    print(f"Flag range: {min(all_670_flags)} - {max(all_670_flags)}")
print(f"Total unique flags: {len(all_670_flags)}")
print(f"{'='*70}")

# Dump all events
for (source, fname, eid), event in sorted(all_670_events.items()):
    dump_event(eid, event, f"{source}/{fname}")


# ================================================================
# SPECIFIC: Event 1049632530 detailed analysis
# ================================================================
print(f"\n\n{'#'*70}")
print(f"# SPECIFIC ANALYSIS: Event 1049632530")
print(f"{'#'*70}")

found_1049632530 = False
for (source, fname, eid), event in all_670_events.items():
    if eid == 1049632530:
        found_1049632530 = True
        print(f"Found in {source}/{fname}")
        dump_event(eid, event, f"{source}/{fname}", indent="")

if not found_1049632530:
    print("Event 1049632530 not found in 670XXX scan. Searching all files...")
    for source_name, event_dir in sources:
        emevd_files = sorted(event_dir.glob('*.emevd.dcx'))
        for emevd_path in emevd_files:
            events = parse_emevd(emevd_path)
            if 1049632530 in events:
                print(f"Found in {source_name}/{emevd_path.name}")
                dump_event(1049632530, events[1049632530],
                          f"{source_name}/{emevd_path.name}", indent="")
                found_1049632530 = True

if not found_1049632530:
    print("Event 1049632530 NOT FOUND ANYWHERE")


# ================================================================
# SetEventFlagRange [2003:022] in 670XXX events
# ================================================================
print(f"\n\n{'#'*70}")
print(f"# SetEventFlagRange [2003:022] instructions in 670XXX events")
print(f"{'#'*70}")

for (source, fname, eid), event in sorted(all_670_events.items()):
    for idx, instr in enumerate(event['instructions']):
        if instr['bank'] == 2003 and instr['id'] == 22:
            args = instr['args']
            if len(args) >= 12:
                flag_type = i32(args, 0)
                first = i32(args, 4)
                last = i32(args, 8)
                state = u8(args, 12) if len(args) > 12 else "?"
                print(f"  {source}/{fname} event {eid} [{idx}]: "
                      f"SetEventFlagRange(type={flag_type}, first={first}, last={last}, state={state})")


# ================================================================
# SetEventFlag [2003:066] in 670XXX events
# ================================================================
print(f"\n\n{'#'*70}")
print(f"# SetEventFlag [2003:066] instructions in 670XXX events")
print(f"{'#'*70}")

for (source, fname, eid), event in sorted(all_670_events.items()):
    for idx, instr in enumerate(event['instructions']):
        if instr['bank'] == 2003 and instr['id'] == 66:
            args = instr['args']
            if len(args) >= 4:
                flag_id = i32(args, 0)
                state = u8(args, 4) if len(args) > 4 else "?"
                extra = i32(args, 8) if len(args) >= 12 else "?"
                marker = " <<<670XXX>>>" if FLAG_MIN <= flag_id <= FLAG_MAX else ""
                print(f"  {source}/{fname} event {eid} [{idx}]: "
                      f"SetEventFlag(flag={flag_id}, state={state}, extra={extra}){marker}")


# ================================================================
# RunEvent calls that pass 670XXX as parameters
# ================================================================
print(f"\n\n{'#'*70}")
print(f"# RunEvent calls that pass 670XXX flags as parameters")
print(f"{'#'*70}")

for source_name, event_dir in sources:
    emevd_files = sorted(event_dir.glob('*.emevd.dcx'))
    for emevd_path in emevd_files:
        events = parse_emevd(emevd_path)
        if not events:
            continue
        fname = emevd_path.name.replace('.emevd.dcx', '')
        for eid, event in events.items():
            for idx, instr in enumerate(event['instructions']):
                if instr['bank'] == 2000 and instr['id'] in (0, 6):
                    args = instr['args']
                    # Check if any param in the RunEvent args is a 670XXX flag
                    has_flag = False
                    for off in range(0, len(args) - 3, 4):
                        val = i32(args, off)
                        if val is not None and FLAG_MIN <= val <= FLAG_MAX:
                            has_flag = True
                            break
                    if has_flag:
                        all_vals = []
                        for off in range(0, len(args) - 3, 4):
                            v = i32(args, off)
                            if v is not None:
                                if FLAG_MIN <= v <= FLAG_MAX:
                                    all_vals.append(f">>>{v}<<<")
                                else:
                                    all_vals.append(str(v))
                        print(f"  {source_name}/{fname} event {eid} [{idx}]: "
                              f"RunEvent params=[{', '.join(all_vals)}]")


# ================================================================
# Entity IDs and other identifiers alongside 670XXX
# ================================================================
print(f"\n\n{'#'*70}")
print(f"# All unique values in 670XXX events (patterns)")
print(f"{'#'*70}")

for (source, fname, eid), event in sorted(all_670_events.items()):
    all_vals = set()
    flag_vals = set()
    for instr in event['instructions']:
        args = instr['args']
        for off in range(0, (len(args) // 4) * 4, 4):
            val = i32(args, off)
            if val is not None:
                all_vals.add(val)
                if FLAG_MIN <= val <= FLAG_MAX:
                    flag_vals.add(val)

    entity_like = sorted(v for v in all_vals if v > 1000000000 and v not in flag_vals)
    medium_vals = sorted(v for v in all_vals
                         if 10000 < v < 1000000000 and v not in flag_vals
                         and v not in (0, 1, 2, 3, 4, 5))

    if entity_like or medium_vals or flag_vals:
        print(f"\n  {source}/{fname} event {eid}:")
        print(f"    670XXX flags: {sorted(flag_vals)}")
        if entity_like:
            print(f"    Entity-like IDs: {entity_like}")
        if medium_vals:
            print(f"    Medium values: {medium_vals}")


# ================================================================
# Complete flag list
# ================================================================
print(f"\n\n{'#'*70}")
print(f"# Complete flag list (sorted)")
print(f"{'#'*70}")
for f in sorted(all_670_flags):
    print(f"  {f}")
print(f"\nTotal: {len(all_670_flags)} unique flags in range 670000-670999")


# ================================================================
# Also scan for events that use 670XXX as template parameters
# This means the event itself has placeholder values that get replaced
# at runtime with 670XXX values from RunEvent calls
# ================================================================
print(f"\n\n{'#'*70}")
print(f"# Events invoked WITH 670XXX flag parameters (template events)")
print(f"{'#'*70}")

# Collect all target event IDs from RunEvent calls that pass 670XXX
target_events_with_670_params = {}  # eventId -> list of (source, fname, caller_eid, params)

for source_name, event_dir in sources:
    emevd_files = sorted(event_dir.glob('*.emevd.dcx'))
    for emevd_path in emevd_files:
        events = parse_emevd(emevd_path)
        if not events:
            continue
        fname = emevd_path.name.replace('.emevd.dcx', '')
        for eid, event in events.items():
            for idx, instr in enumerate(event['instructions']):
                if instr['bank'] == 2000 and instr['id'] in (0, 6):
                    args = instr['args']
                    if len(args) >= 8:
                        slot = i32(args, 0)
                        target_eid = i32(args, 4)
                        # Check if any param is 670XXX
                        has_670 = False
                        all_params = []
                        for off in range(0, len(args) - 3, 4):
                            v = i32(args, off)
                            if v is not None:
                                all_params.append(v)
                                if FLAG_MIN <= v <= FLAG_MAX:
                                    has_670 = True
                        if has_670 and target_eid:
                            if target_eid not in target_events_with_670_params:
                                target_events_with_670_params[target_eid] = []
                            target_events_with_670_params[target_eid].append({
                                'source': source_name,
                                'file': fname,
                                'caller': eid,
                                'params': all_params,
                            })

print(f"Found {len(target_events_with_670_params)} unique target events called with 670XXX params")
for target_eid in sorted(target_events_with_670_params.keys()):
    calls = target_events_with_670_params[target_eid]
    print(f"\n  Target event {target_eid}: called {len(calls)} times")
    for call in calls[:10]:
        flag_params = [p for p in call['params'] if FLAG_MIN <= p <= FLAG_MAX]
        other_params = [p for p in call['params'][2:] if not (FLAG_MIN <= p <= FLAG_MAX)]
        print(f"    from {call['source']}/{call['file']} event {call['caller']}: "
              f"670flags={flag_params}, other={other_params}")
    if len(calls) > 10:
        print(f"    ... and {len(calls) - 10} more calls")

    # Now dump the target event definition
    for source_name, event_dir in sources:
        emevd_files = sorted(event_dir.glob('*.emevd.dcx'))
        for emevd_path in emevd_files:
            events = parse_emevd(emevd_path)
            if target_eid in events:
                print(f"\n  Definition of event {target_eid}:")
                dump_event(target_eid, events[target_eid],
                          f"{source_name}/{emevd_path.name}", indent="    ")
                break
        else:
            continue
        break

print("\nDone!")
