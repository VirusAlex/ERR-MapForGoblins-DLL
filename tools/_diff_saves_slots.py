"""Comprehensive Elden Ring save file diff.

Parses two ER0000.sl2/.err files (BND4 container, NOT encrypted for ER),
extracts each character slot, and does a precise byte/bit-level diff.

Key insight from ER-Save-Editor (ClayAmore):
- Each slot is 0x280000 bytes, preceded by 0x10 bytes MD5 checksum
- EventFlags section is 0x1BF99F bytes (~1.75 MB, ~14.7M flags)
- Slot layout: ver, map_id, ga_items, player_data, equip, inventory,
  ... ga_item_data, tutorial_data, _0x1d, EVENT_FLAGS, unk_lists,
  player_coords, net_data, weather, time, steam_id, dlc, rest

Usage:
  python _diff_saves_v2.py <before.sl2> <after.sl2>
  python _diff_saves_v2.py <before.sl2> <after.sl2> --slot 0
"""

import sys
import hashlib
import struct
from pathlib import Path

SLOT_DATA_SIZE = 0x280000    # 2,621,440 bytes per slot
CHECKSUM_SIZE  = 0x10
SLOT_STRIDE    = CHECKSUM_SIZE + SLOT_DATA_SIZE
HEADER_SIZE    = 0x300       # BND4 header + file headers for standard ER saves

EVENT_FLAGS_SIZE = 0x1BF99F  # 1,833,375 bytes


def find_event_flags_offset(slot_data: bytes) -> int:
    """Heuristic: search for the likely start of the event flags section.

    Event flags is a massive block (~1.75 MB) of mostly-zero bytes with
    scattered set bits.  We look for a region that matches this pattern
    by scanning for the 0x1D-byte unknown block that precedes it.

    As a fallback, return a rough estimate (~0x38000).
    """
    # The event flags section follows:
    #   _tutorial_data (0x408 bytes)
    #   _0x1d (0x1D bytes)
    #   event_flags (0x1BF99F bytes)
    # _tutorial_data is 0x408 bytes of mostly zeros.
    # _0x1d is 0x1D bytes.
    # So event_flags starts at tutorial_end + 0x1D.

    # We can search for a pattern: a long run of zeros (tutorial_data)
    # followed by 0x1D bytes, then 0x1BF99F bytes of flag data.
    # The end of event_flags is followed by a single byte, then 5 variable
    # lists, etc.

    # Simple heuristic: the event flags region should be the largest
    # continuous-ish block in the slot.  Since we know it's ~1.75 MB,
    # it must start between 0x30000 and 0x50000.

    # Try to find the offset by looking for the MD5 checksum pattern
    # or known structures.  For now, use the estimate.
    return 0x38000  # ~229,376 bytes into the slot


def diff_slots(data_a: bytes, data_b: bytes, slot_idx: int, ef_offset: int):
    """Compare two slot data buffers and report differences."""
    assert len(data_a) == SLOT_DATA_SIZE
    assert len(data_b) == SLOT_DATA_SIZE

    md5_a = hashlib.md5(data_a).hexdigest()
    md5_b = hashlib.md5(data_b).hexdigest()

    if data_a == data_b:
        print(f"\n  Slot {slot_idx}: IDENTICAL (MD5: {md5_a[:16]})")
        return

    changes = []
    for i in range(SLOT_DATA_SIZE):
        if data_a[i] != data_b[i]:
            xor_val = data_a[i] ^ data_b[i]
            single_bit = (xor_val & (xor_val - 1)) == 0
            changes.append((i, data_a[i], data_b[i], xor_val, single_bit))

    ef_end = ef_offset + EVENT_FLAGS_SIZE

    pre_ef  = [c for c in changes if c[0] < ef_offset]
    in_ef   = [c for c in changes if ef_offset <= c[0] < ef_end]
    post_ef = [c for c in changes if c[0] >= ef_end]

    single_bits = [c for c in changes if c[4]]
    multi_bits  = [c for c in changes if not c[4]]

    print(f"\n{'='*72}")
    print(f"  Slot {slot_idx}: {len(changes)} bytes changed")
    print(f"  MD5 A: {md5_a}")
    print(f"  MD5 B: {md5_b}")
    print(f"  EF offset estimate: 0x{ef_offset:06X} .. 0x{ef_end:06X}")
    print(f"  Pre-EventFlags:  {len(pre_ef):>6d} changes  (0x000000..0x{ef_offset:06X})")
    print(f"  EventFlags:      {len(in_ef):>6d} changes  (0x{ef_offset:06X}..0x{ef_end:06X})")
    print(f"  Post-EventFlags: {len(post_ef):>6d} changes  (0x{ef_end:06X}..0x{SLOT_DATA_SIZE:06X})")
    print(f"  Single-bit:      {len(single_bits):>6d}")
    print(f"  Multi-bit:       {len(multi_bits):>6d}")

    # --- Single-bit changes (most interesting for flag detection) ---
    if single_bits:
        print(f"\n  === Single-bit changes ({len(single_bits)}) ===")
        for idx, (off, old, new, xor_val, _) in enumerate(single_bits):
            if idx >= 500:
                print(f"    ... {len(single_bits) - 500} more")
                break
            bit = 0
            x = xor_val
            while not (x & 1):
                x >>= 1
                bit += 1
            direction = "SET" if (new >> bit) & 1 else "CLR"
            region = "EF" if ef_offset <= off < ef_end else \
                     "PRE" if off < ef_offset else "POST"
            # For EF region, show relative offset
            if region == "EF":
                rel = off - ef_offset
                abs_bit = rel * 8 + bit
                print(f"    +0x{off:06X} (EF+0x{rel:06X}) : "
                      f"{old:02X}->{new:02X} bit {bit} {direction} "
                      f"(abs_bit #{abs_bit})")
            else:
                print(f"    +0x{off:06X} [{region:>4s}] : "
                      f"{old:02X}->{new:02X} bit {bit} {direction}")

    # --- Multi-bit changes ---
    if multi_bits:
        print(f"\n  === Multi-bit/byte changes ({len(multi_bits)}) ===")
        for idx, (off, old, new, xor_val, _) in enumerate(multi_bits):
            if idx >= 200:
                print(f"    ... {len(multi_bits) - 200} more")
                break
            region = "EF" if ef_offset <= off < ef_end else \
                     "PRE" if off < ef_offset else "POST"
            print(f"    +0x{off:06X} [{region:>4s}] : {old:02X} -> {new:02X}")

    # --- Change clusters ---
    if changes:
        print(f"\n  === Change clusters ===")
        clusters = []
        cl_start = changes[0][0]
        cl_end = cl_start
        cl_count = 1
        for c in changes[1:]:
            if c[0] - cl_end <= 32:
                cl_end = c[0]
                cl_count += 1
            else:
                clusters.append((cl_start, cl_end, cl_count))
                cl_start = c[0]
                cl_end = c[0]
                cl_count = 1
        clusters.append((cl_start, cl_end, cl_count))

        for idx, (start, end, count) in enumerate(clusters):
            if idx >= 100:
                print(f"    ... {len(clusters) - 100} more clusters")
                break
            region = "EF" if ef_offset <= start < ef_end else \
                     "PRE" if start < ef_offset else "POST"
            span = end - start + 1
            print(f"    0x{start:06X}..0x{end:06X}  "
                  f"{count:>5d} bytes changed, span {span:>6d} [{region}]")

    # --- Try to find event flag IDs for single-bit changes in EF region ---
    ef_singles = [c for c in single_bits if ef_offset <= c[0] < ef_end]
    if ef_singles:
        print(f"\n  === Potential event flag IDs (heuristic) ===")
        print(f"  NOTE: Flag ID calculation is approximate!")
        print(f"  The mapping from bit offset to flag ID depends on the")
        print(f"  internal sectored layout of EventFlagMan.")
        print(f"  These are RAW bit offsets within the EF section:")
        for idx, (off, old, new, xor_val, _) in enumerate(ef_singles):
            if idx >= 100:
                break
            bit = 0
            x = xor_val
            while not (x & 1):
                x >>= 1
                bit += 1
            rel = off - ef_offset
            abs_bit = rel * 8 + bit
            direction = "SET" if (new >> bit) & 1 else "CLR"
            print(f"    EF byte 0x{rel:06X} bit {bit} = abs_bit #{abs_bit} {direction}")

    print(f"\n{'='*72}")


def main():
    args = sys.argv[1:]
    target_slot = None

    paths = []
    for arg in args:
        if arg.startswith('--slot'):
            continue
        if arg.isdigit() and len(arg) <= 2 and paths:
            target_slot = int(arg)
            continue
        paths.append(arg)

    # Handle --slot N
    for i, arg in enumerate(args):
        if arg == '--slot' and i + 1 < len(args):
            target_slot = int(args[i + 1])

    if len(paths) != 2:
        print(f"Usage: {sys.argv[0]} <before.sl2> <after.sl2> [--slot N]")
        sys.exit(1)

    path_a, path_b = Path(paths[0]), Path(paths[1])
    data_a = path_a.read_bytes()
    data_b = path_b.read_bytes()

    print(f"File A: {path_a.name} ({len(data_a):,} bytes)")
    print(f"File B: {path_b.name} ({len(data_b):,} bytes)")

    if data_a[:4] != b'BND4':
        print("WARNING: File A missing BND4 magic!")
    if data_b[:4] != b'BND4':
        print("WARNING: File B missing BND4 magic!")

    if len(data_a) != len(data_b):
        print(f"WARNING: size mismatch ({len(data_a)} vs {len(data_b)})")

    # Header-level diff
    hdr_diff = sum(1 for i in range(min(HEADER_SIZE, len(data_a), len(data_b)))
                   if data_a[i] != data_b[i])
    print(f"Header (0x000..0x{HEADER_SIZE:03X}): {hdr_diff} bytes changed")

    ef_offset = find_event_flags_offset(b'')

    for slot_idx in range(10):
        if target_slot is not None and slot_idx != target_slot:
            continue

        cksum_start = HEADER_SIZE + slot_idx * SLOT_STRIDE
        data_start  = cksum_start + CHECKSUM_SIZE
        data_end    = data_start + SLOT_DATA_SIZE

        if data_end > min(len(data_a), len(data_b)):
            break

        slot_a = data_a[data_start:data_end]
        slot_b = data_b[data_start:data_end]

        active_a = any(b != 0 for b in slot_a[:0x100])
        active_b = any(b != 0 for b in slot_b[:0x100])

        if not active_a and not active_b:
            continue

        diff_slots(slot_a, slot_b, slot_idx, ef_offset)


if __name__ == '__main__':
    main()
