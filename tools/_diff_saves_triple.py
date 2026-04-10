"""Triple-test save file comparator for Rune Piece tracking research.

Use case:
  Test 1: pick piece A  ->  saves: base.err, test1.err
  Test 2: pick piece B  ->  saves: base.err, test2.err
  Test 3: pick A + B    ->  saves: base.err, test3.err

We compare:
  base vs test1  -> changes caused by picking A
  base vs test2  -> changes caused by picking B
  base vs test3  -> changes caused by picking A+B
  test1 vs test2 -> structural difference between A and B
  
And find bits that are UNIQUE to each pickup, ignoring noise (checksums, timestamps, etc).

SL2 slot layout (each slot = 0x280000 bytes):
  Slot 0: 0x310      .. 0x28030F  (checksums at 0x300..0x30F)
  Slot 1: 0x280320   .. 0x50031F  
  Slot 2: 0x500330   .. 0x78032F
  ...
  Slot 9: 0x1180390  .. 0x140038F
"""

import sys
import struct
import os

# ─── Configuration ──────────────────────────────────────────────────────────

SLOT_SIZE = 0x280000
SLOT_OFFSETS = [0x310 + i * (SLOT_SIZE + 0x10) for i in range(10)]
CHECKSUM_SIZE = 0x10

def detect_slot(data):
    """Find the active character slot by checking which slots have non-zero data."""
    active = []
    for i, off in enumerate(SLOT_OFFSETS):
        if off + 0x100 > len(data):
            break
        chunk = data[off:off+0x100]
        if any(b != 0 for b in chunk):
            active.append(i)
    return active

def extract_slot(data, slot_idx):
    """Extract a single slot's data."""
    off = SLOT_OFFSETS[slot_idx]
    return data[off:off+SLOT_SIZE]

def diff_bytes(a, b):
    """Return list of (offset, old_byte, new_byte) for differences."""
    diffs = []
    for i in range(min(len(a), len(b))):
        if a[i] != b[i]:
            diffs.append((i, a[i], b[i]))
    return diffs

def analyze_single_bits(diffs):
    """Extract single-bit changes from diffs."""
    bits = []
    for off, old, new in diffs:
        xor = old ^ new
        if xor & (xor - 1) == 0:
            bit = xor.bit_length() - 1
            direction = "SET" if new > old else "CLEARED"
            bits.append((off, bit, direction))
    return bits

def compare_pair(name, base_slot, test_slot, output_file=None):
    """Compare two slot extracts and report differences."""
    diffs = diff_bytes(base_slot, test_slot)
    bits = analyze_single_bits(diffs)
    
    lines = []
    lines.append(f"\n{'='*80}")
    lines.append(f"  {name}")
    lines.append(f"{'='*80}")
    lines.append(f"  Total changed bytes: {len(diffs)}")
    lines.append(f"  Single-bit changes: {len(bits)}")
    
    # Group into regions
    if diffs:
        regions = []
        region_start = diffs[0][0]
        region_end = diffs[0][0]
        for off, _, _ in diffs[1:]:
            if off <= region_end + 32:
                region_end = off
            else:
                regions.append((region_start, region_end))
                region_start = off
                region_end = off
        regions.append((region_start, region_end))
        lines.append(f"  Regions: {len(regions)}")
        
        # Print first 50 regions
        for ri, (start, end) in enumerate(regions[:50]):
            span = end - start + 1
            num_diff = sum(1 for o, _, _ in diffs if start <= o <= end)
            lines.append(f"    Region {ri+1}: slot+0x{start:06X}..0x{end:06X} ({span}B span, {num_diff} changed)")
            
            # Show single-bit changes in this region
            for off, bit, direction in bits:
                if start <= off <= end:
                    lines.append(f"      bit #{off*8+bit}: byte slot+0x{off:06X} bit {bit} {direction}")
    
    lines.append("")
    
    text = "\n".join(lines)
    print(text)
    if output_file:
        output_file.write(text + "\n")
    
    return diffs, bits

def find_unique_bits(bits_a, bits_b, bits_ab):
    """Find bits that are unique to A, unique to B, and shared between A+B."""
    set_a = set((off, bit) for off, bit, _ in bits_a)
    set_b = set((off, bit) for off, bit, _ in bits_b)
    set_ab = set((off, bit) for off, bit, _ in bits_ab)
    
    # Bits only in A
    only_a = set_a - set_b
    # Bits only in B
    only_b = set_b - set_a
    # Bits in both A and B (noise)
    noise = set_a & set_b
    # Bits in AB that match union of A+B
    expected_ab = set_a | set_b
    
    return only_a, only_b, noise, expected_ab, set_ab

def main():
    if len(sys.argv) < 3:
        print("Usage: python _diff_saves_triple.py <base.err> <test1.err> [test2.err] [test3.err]")
        print("  2 files: simple diff of base vs test1")
        print("  3 files: diff base vs test1, base vs test2, test1 vs test2")
        print("  4 files: full triple test (base vs test1/test2/test3, cross-compare)")
        sys.exit(1)
    
    files = sys.argv[1:]
    saves = []
    for f in files:
        if not os.path.exists(f):
            print(f"ERROR: File not found: {f}")
            sys.exit(1)
        with open(f, 'rb') as fp:
            saves.append(fp.read())
        print(f"Loaded: {f} ({len(saves[-1])} bytes)")
    
    # Detect active slot
    base = saves[0]
    active = detect_slot(base)
    print(f"Active slots: {active}")
    
    if not active:
        print("No active slots found! Falling back to raw diff.")
        for i in range(1, len(saves)):
            compare_pair(f"Raw diff: {files[0]} vs {files[i]}", base, saves[i])
        return
    
    slot_idx = active[-1]  # Use last active slot
    print(f"Using slot {slot_idx} (offset 0x{SLOT_OFFSETS[slot_idx]:X})")
    
    # Extract slots
    slots = [extract_slot(s, slot_idx) for s in saves]
    
    output_path = os.path.join(os.path.dirname(files[0]), "diff_analysis.txt")
    
    with open(output_path, 'w', encoding='utf-8') as out:
        out.write(f"Triple-test save analysis\n")
        out.write(f"Slot: {slot_idx}, offset 0x{SLOT_OFFSETS[slot_idx]:X}\n")
        out.write(f"Files: {files}\n\n")
        
        if len(saves) == 2:
            compare_pair(f"base vs test1", slots[0], slots[1], out)
        
        elif len(saves) == 3:
            _, bits_a = compare_pair(f"base vs test1 (piece A)", slots[0], slots[1], out)[:2]
            _, bits_b = compare_pair(f"base vs test2 (piece B)", slots[0], slots[2], out)[:2]
            compare_pair(f"test1 vs test2 (A vs B)", slots[1], slots[2], out)
            
            # Cross-analysis
            only_a, only_b, noise, _, _ = find_unique_bits(bits_a, bits_b, [])
            
            header = f"\n{'='*80}\n  CROSS-ANALYSIS: unique bits\n{'='*80}"
            print(header)
            out.write(header + "\n")
            
            msg = f"  Bits ONLY in test1 (piece A specific): {len(only_a)}"
            print(msg); out.write(msg + "\n")
            for off, bit in sorted(only_a)[:50]:
                line = f"    slot+0x{off:06X} bit {bit}  (absolute bit #{off*8+bit})"
                print(line); out.write(line + "\n")
            
            msg = f"  Bits ONLY in test2 (piece B specific): {len(only_b)}"
            print(msg); out.write(msg + "\n")
            for off, bit in sorted(only_b)[:50]:
                line = f"    slot+0x{off:06X} bit {bit}  (absolute bit #{off*8+bit})"
                print(line); out.write(line + "\n")
            
            msg = f"  Bits in BOTH (noise/shared): {len(noise)}"
            print(msg); out.write(msg + "\n")
        
        elif len(saves) >= 4:
            _, bits_a = compare_pair(f"base vs test1 (piece A)", slots[0], slots[1], out)
            diffs_a = _
            _, bits_b = compare_pair(f"base vs test2 (piece B)", slots[0], slots[2], out)
            diffs_b = _
            _, bits_ab = compare_pair(f"base vs test3 (pieces A+B)", slots[0], slots[3], out)
            diffs_ab = _
            compare_pair(f"test1 vs test2 (A vs B)", slots[1], slots[2], out)
            compare_pair(f"test1 vs test3 (A vs A+B)", slots[1], slots[3], out)
            compare_pair(f"test2 vs test3 (B vs A+B)", slots[2], slots[3], out)
            
            # Cross-analysis
            only_a, only_b, noise, expected_ab, actual_ab = find_unique_bits(bits_a, bits_b, bits_ab)
            
            header = f"\n{'#'*80}\n  FINAL CROSS-ANALYSIS\n{'#'*80}"
            print(header); out.write(header + "\n")
            
            # KEY INSIGHT: bits that are unique to A and also present in AB
            a_confirmed = only_a & actual_ab
            b_confirmed = only_b & actual_ab
            
            msg = f"\n  Bits UNIQUE to piece A (confirmed in A+B test): {len(a_confirmed)}"
            print(msg); out.write(msg + "\n")
            for off, bit in sorted(a_confirmed)[:100]:
                line = f"    slot+0x{off:06X} bit {bit}  (absolute bit #{off*8+bit})"
                print(line); out.write(line + "\n")
            
            msg = f"\n  Bits UNIQUE to piece B (confirmed in A+B test): {len(b_confirmed)}"
            print(msg); out.write(msg + "\n")
            for off, bit in sorted(b_confirmed)[:100]:
                line = f"    slot+0x{off:06X} bit {bit}  (absolute bit #{off*8+bit})"
                print(line); out.write(line + "\n")
            
            msg = f"\n  Noise bits (same in A and B): {len(noise)}"
            print(msg); out.write(msg + "\n")
            
            msg = f"\n  Expected bits in A+B: {len(expected_ab)}, Actual bits in A+B: {len(actual_ab)}"
            print(msg); out.write(msg + "\n")
            
            unexpected = actual_ab - expected_ab
            msg = f"  Unexpected bits in A+B (not in A or B alone): {len(unexpected)}"
            print(msg); out.write(msg + "\n")
            for off, bit in sorted(unexpected)[:50]:
                line = f"    slot+0x{off:06X} bit {bit}"
                print(line); out.write(line + "\n")
            
            missing = expected_ab - actual_ab
            msg = f"  Missing from A+B (expected but not found): {len(missing)}"
            print(msg); out.write(msg + "\n")
    
    print(f"\nFull analysis saved to: {output_path}")

if __name__ == "__main__":
    main()
