#!/usr/bin/env python3
"""Inspect EMEVD events referencing a specific flag or lot ID.

Usage:
  py inspect_emevd_drop.py <map> <flag_or_lot_id> [more ids ...]

Dumps every event whose instructions contain ANY of the given 32-bit
values in their ArgData, plus the full instruction list for context."""
import sys, io, os, tempfile, struct
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')

import config
from pythonnet import load
load('coreclr')
import clr
from System.Reflection import Assembly, BindingFlags
from System import Array, Type as SysType, Object
from System.IO import File as SysFile

clr.AddReference(str(config.SOULSFORMATS_DLL))
import SoulsFormats
asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))

_str = SysType.GetType('System.String')
_emevd = asm.GetType('SoulsFormats.EMEVD').GetMethod(
    'Read', BindingFlags.Public|BindingFlags.Static|BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str]), None)


def main():
    map_name = sys.argv[1]
    targets = set(int(a) for a in sys.argv[2:])
    print(f'Targets: {targets}')

    p = config.ERR_MOD_DIR / 'event' / f'{map_name}.emevd.dcx'
    if not p.exists():
        print(f'No file: {p}')
        return
    data = SoulsFormats.DCX.Decompress(str(p)).ToArray()
    tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_em.bin')
    SysFile.WriteAllBytes(tmp, data)
    emevd = _emevd.Invoke(None, Array[Object]([tmp]))

    print(f'\nEvents in {map_name}: {emevd.Events.Count}')

    for event in emevd.Events:
        # Scan instructions for any 32-bit value matching a target
        match_instrs = []
        all_instrs = []
        for i, instr in enumerate(event.Instructions):
            args = bytes(instr.ArgData)
            line = f'  [{i}] {instr.Bank}:{instr.ID}  args(len={len(args)})='
            vals_4 = []
            for off in range(0, len(args) - 3, 4):
                vals_4.append(struct.unpack_from('<i', args, off)[0])
            line += ' '.join(str(v) for v in vals_4)
            all_instrs.append(line)
            if any(t in vals_4 for t in targets):
                match_instrs.append(i)

        if match_instrs:
            print(f'\n=== Event {event.ID} ({len(event.Instructions)} instructions) - match at idx {match_instrs} ===')
            for line in all_instrs:
                print(line)
            # Also list event parameters (initializer arg bindings)
            try:
                if event.Parameters and event.Parameters.Count > 0:
                    print(f'  Parameters:')
                    for prm in event.Parameters:
                        print(f'    instr={prm.InstructionIndex} target={prm.TargetStartByte} source={prm.SourceStartByte} length={prm.ByteCount}')
            except Exception as e:
                pass


if __name__ == '__main__':
    main()
