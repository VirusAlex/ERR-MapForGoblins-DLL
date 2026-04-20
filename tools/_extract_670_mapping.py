#!/usr/bin/env python3
"""
Extract the complete mapping from Event 6909 (common.emevd):
  multiplayer_state_flag -> 670XXX summoning pool flag

Also extract RunEvent(90009000) calls from per-map EMEVDs:
  670XXX flag -> map file -> (area, block)
"""
import sys, io, struct, json
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
from pathlib import Path
import config
from pythonnet import load
load('coreclr')
import clr
from System.Reflection import Assembly
from System import Array, Type as SysType, Object, String

asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
clr.AddReference(str(config.SOULSFORMATS_DLL))
import SoulsFormats

_emevd_cls = asm.GetType('SoulsFormats.EMEVD')
_str_type = SysType.GetType('System.String')
_read_method = _emevd_cls.BaseType.GetMethod('Read', Array[SysType]([_str_type]))

def i32(data, off):
    return struct.unpack_from('<i', data, off)[0] if off+4 <= len(data) else None

def parse_emevd(path):
    try:
        emevd = _read_method.Invoke(None, Array[Object]([String(str(path))]))
    except Exception as e:
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
        events[eid] = instrs
    return events


mod_event_dir = Path(config.require_err_mod_dir()) / 'event'
game_event_dir = Path(config.require_game_dir()) / 'event'

# ============================================================
# 1. Extract Event 6909 mapping: mp_flag -> 670XXX
# ============================================================
print("=" * 60)
print("Event 6909: MultiplayerState flag -> 670XXX mapping")
print("=" * 60)

# Parse from MOD first (mod overrides game)
events = parse_emevd(mod_event_dir / 'common.emevd.dcx')
if 6909 not in events:
    events = parse_emevd(game_event_dir / 'common.emevd.dcx')

event_6909 = events.get(6909, [])
print(f"Event 6909: {len(event_6909)} instructions")

# Pattern: IfMultiplayerState(1, mp_flag) -> SetEventFlag(0, 670XXX, 1)
mp_to_670 = {}
for idx in range(0, len(event_6909) - 1):
    instr = event_6909[idx]
    next_instr = event_6909[idx + 1]
    # IfMultiplayerState: bank=1003, id=1
    if instr['bank'] == 1003 and instr['id'] == 1:
        args = instr['args']
        if len(args) >= 8:
            mp_state = i32(args, 0)
            mp_flag = i32(args, 4)
            # SetEventFlag: bank=2003, id=66
            if next_instr['bank'] == 2003 and next_instr['id'] == 66:
                nargs = next_instr['args']
                if len(nargs) >= 8:
                    flag_670 = i32(nargs, 4)
                    state = i32(nargs, 8)
                    if 670000 <= flag_670 <= 670999:
                        mp_to_670[mp_flag] = flag_670

print(f"\nExtracted {len(mp_to_670)} mappings: mp_flag -> 670XXX")
print(f"670XXX range: {min(mp_to_670.values())} - {max(mp_to_670.values())}")
print(f"Unique 670XXX flags: {len(set(mp_to_670.values()))}")

# Decode mp_flag format: MMBB0040 where MM=map area, BB=map block
# Legacy dungeons: 1X00004Y where X=mapArea, Y=index
# Open world: 10AABBCC40 where AA=row, BB=col
print("\nMapping table (mp_flag -> 670XXX):")
for mp_flag in sorted(mp_to_670.keys()):
    flag_670 = mp_to_670[mp_flag]
    # Decode map from mp_flag
    if mp_flag >= 1000000000:
        # Open world: 1RRCC0040 where RR=row, CC=col
        # e.g. 1060410040 -> row 60, col 41
        s = str(mp_flag)
        area_part = s[:3]  # e.g. "106"
        tile_part = s[3:7]  # e.g. "0410"
        suffix = s[7:]      # e.g. "040" or "041"
        print(f"  {mp_flag} -> {flag_670}  (overworld tile {area_part}_{tile_part}_{suffix})")
    else:
        # Legacy dungeon: MMBB0040
        s = str(mp_flag)
        if len(s) == 8:
            area = int(s[:2])
            block = int(s[2:4])
            idx = int(s[-2:]) - 40
            print(f"  {mp_flag} -> {flag_670}  (m{area:02d}_{block:02d}, pool #{idx})")
        else:
            print(f"  {mp_flag} -> {flag_670}  (unknown format)")


# ============================================================
# 2. Extract RunEvent(90009000, 670XXX, area, block, ...) calls
# ============================================================
print("\n" + "=" * 60)
print("RunCommonEvent(90009000) calls: 670XXX -> map location")
print("=" * 60)

flag_to_map = {}  # 670XXX -> {'map': ..., 'area': ..., 'block': ...}

for emevd_path in sorted(mod_event_dir.glob('*.emevd.dcx')):
    events = parse_emevd(emevd_path)
    if not events:
        continue
    fname = emevd_path.name.replace('.emevd.dcx', '')

    for eid, instrs in events.items():
        for instr in instrs:
            if instr['bank'] == 2000 and instr['id'] in (0, 6):
                args = instr['args']
                if len(args) >= 8:
                    slot = i32(args, 0)
                    target = i32(args, 4)
                    if target == 90009000 and len(args) >= 28:
                        flag_670 = i32(args, 8)
                        area = i32(args, 12)
                        block = i32(args, 16)
                        unk1 = i32(args, 20)
                        unk2 = i32(args, 24)
                        if 670000 <= flag_670 <= 670999:
                            flag_to_map[flag_670] = {
                                'map': fname,
                                'area': area,
                                'block': block,
                                'unk1': unk1,
                                'unk2': unk2,
                            }

print(f"\nExtracted {len(flag_to_map)} flag-to-map entries")
for flag_670 in sorted(flag_to_map.keys()):
    info = flag_to_map[flag_670]
    print(f"  {flag_670} -> {info['map']} (area={info['area']}, block={info['block']})")


# ============================================================
# 3. Combine: mp_flag -> 670XXX -> map location
# ============================================================
print("\n" + "=" * 60)
print("Combined mapping: 670XXX flag -> mp_flag -> map")
print("=" * 60)

# Invert mp_to_670 to get 670XXX -> mp_flag
flag_670_to_mp = {}
for mp_flag, flag_670 in mp_to_670.items():
    flag_670_to_mp[flag_670] = mp_flag

all_670_flags = sorted(set(list(mp_to_670.values()) + list(flag_to_map.keys())))
print(f"\nTotal unique 670XXX flags: {len(all_670_flags)}")

# Flags in Event 6909 but NOT in any RunEvent(90009000) call
in_6909_only = set(mp_to_670.values()) - set(flag_to_map.keys())
in_run_only = set(flag_to_map.keys()) - set(mp_to_670.values())
in_both = set(mp_to_670.values()) & set(flag_to_map.keys())

print(f"In Event 6909 only: {len(in_6909_only)}")
print(f"In RunEvent(90009000) only: {len(in_run_only)}")
print(f"In both: {len(in_both)}")

if in_6909_only:
    print(f"\n  670XXX flags in Event 6909 but NOT in RunEvent(90009000):")
    for f in sorted(in_6909_only):
        mp = flag_670_to_mp.get(f, "?")
        print(f"    {f} (mp_flag={mp})")

if in_run_only:
    print(f"\n  670XXX flags in RunEvent(90009000) but NOT in Event 6909:")
    for f in sorted(in_run_only):
        info = flag_to_map.get(f, {})
        print(f"    {f} -> {info.get('map', '?')}")


# ============================================================
# 4. Summary: flag encoding pattern
# ============================================================
print("\n" + "=" * 60)
print("Flag encoding pattern analysis")
print("=" * 60)

# Group by hundreds digit to see area mapping
groups = {}
for f in all_670_flags:
    group = (f - 670000) // 100
    if group not in groups:
        groups[group] = []
    groups[group].append(f)

for group in sorted(groups.keys()):
    flags = groups[group]
    # Find maps for these flags
    maps = set()
    for f in flags:
        if f in flag_to_map:
            maps.add(flag_to_map[f]['map'])
    maps_str = ', '.join(sorted(maps)) if maps else 'no map'
    print(f"  670{group}XX: {len(flags)} flags [{min(flags)}-{max(flags)}] -> {maps_str}")


# Save results
out = {
    'mp_to_670': {str(k): v for k, v in mp_to_670.items()},
    'flag_to_map': {str(k): v for k, v in flag_to_map.items()},
    'all_flags': all_670_flags,
}
out_path = Path(__file__).parent.parent / 'data' / '_670_summoning_pools.json'
with open(out_path, 'w', encoding='utf-8') as f:
    json.dump(out, f, indent=2, ensure_ascii=False)
print(f"\nSaved to {out_path}")
