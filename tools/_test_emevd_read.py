#!/usr/bin/env python3
"""Test EMEVD reading - using different approaches."""
import sys, io
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
from pathlib import Path
import config
from pythonnet import load
load('coreclr')
import clr
from System.Reflection import Assembly
from System import Array, Type as SysType, Object, String, Byte
import System

asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
clr.AddReference(str(config.SOULSFORMATS_DLL))
import SoulsFormats

test_path = str(config.require_game_dir() / 'event' / 'common.emevd.dcx')
print(f"Reading: {test_path}")

# Try 1: Read with System.String
try:
    s = String(test_path)
    emevd = SoulsFormats.EMEVD.Read(s)
    print(f"  String approach: Events={emevd.Events.Count}")
except Exception as e:
    print(f"  String approach failed: {e}")

# Try 2: Read file into byte[] manually, then use IsRead
try:
    with open(test_path, 'rb') as f:
        data = f.read()
    arr = Array[Byte](data)
    print(f"  Byte array: {len(data)} bytes, type={type(arr)}")

    # Use reflection for IsRead(byte[], out EMEVD)
    emevd_type = asm.GetType('SoulsFormats.EMEVD')
    base_type = emevd_type.BaseType

    # Try calling IsRead via reflection
    is_read_method = None
    for m in base_type.GetMethods():
        if str(m.Name) == 'IsRead':
            params = m.GetParameters()
            if len(params) == 2 and str(params[0].ParameterType) == 'System.Byte[]':
                is_read_method = m
                break

    if is_read_method:
        print(f"  Found IsRead(byte[], out EMEVD)")
        # Call with out parameter
        args_arr = Array[Object]([arr, None])
        result = is_read_method.Invoke(None, args_arr)
        print(f"  IsRead result: {result}")
        if result:
            emevd = args_arr[1]
            print(f"  Events: {emevd.Events.Count}")
    else:
        print("  IsRead(byte[]) not found")

except Exception as e:
    print(f"  Byte array approach failed: {e}")
    import traceback
    traceback.print_exc()

# Try 3: Use reflection to call Read(string) directly
try:
    print("\n  Trying reflection Read(string)...")
    emevd_type = asm.GetType('SoulsFormats.EMEVD')
    base_type = emevd_type.BaseType

    str_type = SysType.GetType('System.String')
    read_method = base_type.GetMethod('Read', Array[SysType]([str_type]))
    print(f"  Read(string) method: {read_method}")
    if read_method:
        result = read_method.Invoke(None, Array[Object]([String(test_path)]))
        print(f"  Result type: {type(result)}")
        if result:
            print(f"  Events: {result.Events.Count}")
except Exception as e:
    print(f"  Reflection Read(string) failed: {e}")
    import traceback
    traceback.print_exc()

# Try 4: Read bytes into Memory<byte> manually
try:
    print("\n  Trying Memory<byte> approach...")
    mem_type = System.Type.GetType('System.Memory`1[System.Byte]')
    print(f"  Memory<byte> type: {mem_type}")

    with open(test_path, 'rb') as f:
        data = f.read()
    arr = Array[Byte](data)

    # Create Memory<byte> from array
    mem = System.Memory[Byte](arr)
    print(f"  Memory created, length={mem.Length}")

    emevd = SoulsFormats.EMEVD.Read(mem)
    print(f"  Events: {emevd.Events.Count}")
except Exception as e:
    print(f"  Memory<byte> approach failed: {e}")
    import traceback
    traceback.print_exc()
