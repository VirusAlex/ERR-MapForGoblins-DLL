"""Binary diff of two Elden Ring save files.
Reports all byte-level differences with context."""

import sys, struct

if len(sys.argv) != 3:
    print("Usage: _diff_saves.py <before.err> <after.err>")
    sys.exit(1)

BEFORE, AFTER = sys.argv[1], sys.argv[2]

with open(BEFORE, 'rb') as f:
    before = f.read()
with open(AFTER, 'rb') as f:
    after = f.read()

print(f"Before: {len(before)} bytes")
print(f"After:  {len(after)} bytes")

diffs = []
for i in range(min(len(before), len(after))):
    if before[i] != after[i]:
        diffs.append(i)

print(f"Total different bytes: {len(diffs)}")
print()

if not diffs:
    print("Files are identical!")
    sys.exit(0)

# Group consecutive diffs into regions
regions = []
region_start = diffs[0]
region_end = diffs[0]
for d in diffs[1:]:
    if d <= region_end + 16:  # merge if within 16 bytes
        region_end = d
    else:
        regions.append((region_start, region_end))
        region_start = d
        region_end = d
regions.append((region_start, region_end))

print(f"Difference regions: {len(regions)}")
print("=" * 80)

for ri, (start, end) in enumerate(regions):
    ctx_start = max(0, start - 16)
    ctx_end = min(len(before), end + 17)
    num_diff = sum(1 for i in range(start, end+1) if before[i] != after[i])
    
    print(f"\nRegion {ri+1}: offset 0x{start:08X} - 0x{end:08X} ({end-start+1} bytes span, {num_diff} changed)")
    print(f"  Context 0x{ctx_start:08X} - 0x{ctx_end:08X}")
    
    # Show before/after hex dump
    print("  BEFORE: ", end="")
    for i in range(ctx_start, ctx_end):
        if start <= i <= end and before[i] != after[i]:
            print(f"[{before[i]:02X}]", end="")
        else:
            print(f" {before[i]:02X} ", end="")
    print()
    
    print("  AFTER:  ", end="")
    for i in range(ctx_start, ctx_end):
        if start <= i <= end and before[i] != after[i]:
            print(f"[{after[i]:02X}]", end="")
        else:
            print(f" {after[i]:02X} ", end="")
    print()
    
    # Try to interpret as various types
    if num_diff <= 4:
        for i in range(start, end+1):
            if before[i] != after[i]:
                # Check if it's a single bit change
                xor = before[i] ^ after[i]
                if xor & (xor - 1) == 0:  # power of 2 = single bit
                    bit = xor.bit_length() - 1
                    print(f"  >> Single bit change at 0x{i:08X}: bit {bit} {'cleared' if after[i] < before[i] else 'set'}")
                else:
                    print(f"  >> Byte 0x{i:08X}: {before[i]} -> {after[i]} (delta {after[i] - before[i]:+d})")
    
    # Try reading as uint32 around the diff
    if end - start < 8:
        for align in [start & ~3, (start & ~3) - 4, (start & ~3) + 4]:
            if 0 <= align and align + 4 <= len(before):
                val_b = struct.unpack_from('<I', before, align)[0]
                val_a = struct.unpack_from('<I', after, align)[0]
                if val_b != val_a:
                    print(f"  >> uint32 @ 0x{align:08X}: {val_b} -> {val_a} (0x{val_b:08X} -> 0x{val_a:08X})")

print("\n" + "=" * 80)
print(f"Summary: {len(diffs)} bytes changed across {len(regions)} regions")
