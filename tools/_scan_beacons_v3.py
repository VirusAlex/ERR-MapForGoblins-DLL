"""
Scan Elden Ring memory for live beacon/stamp marker arrays.

Stricter than v2. Looks for:
  - EMPTY_BEACON pattern (specific 16 bytes) at 16-byte alignment,
    which anchors the beacon array in any slot layout.
  - AND/OR filled slot with type EXACTLY in {0x0100, 0x010A} and plausible coords.

Save file layout (confirmed by extract_markers.py):
  5 beacon slots (type 0x0100/0x010A), then N stamp slots (0x06/0x08/0x09),
  then 0xFFFF terminator. Memory may mirror this or use a different shape.

Run with game PID (pass as arg or edit below).
"""
import sys, io, struct, ctypes
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
from ctypes import wintypes
import pymem

PID = int(sys.argv[1]) if len(sys.argv) > 1 else 18072

EMPTY_BEACON = b"\xff\xff\xff\xff\x00\x00\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00"
EMPTY_BEACON_DLC = b"\xff\xff\xff\xff\x00\x00\x00\x00\x00\x00\x00\x00\x0a\x01\x00\x00"

pm = pymem.Pymem()
pm.open_process_from_id(PID)
print(f'Attached to PID {PID}')

MEM_COMMIT = 0x1000
VALID_PROT = {0x02, 0x04, 0x08, 0x20, 0x40}

class MBI(ctypes.Structure):
    _fields_ = [
        ("BaseAddress", ctypes.c_ulonglong),
        ("AllocationBase", ctypes.c_ulonglong),
        ("AllocationProtect", wintypes.DWORD),
        ("__a1", wintypes.DWORD),
        ("RegionSize", ctypes.c_ulonglong),
        ("State", wintypes.DWORD),
        ("Protect", wintypes.DWORD),
        ("Type", wintypes.DWORD),
        ("__a2", wintypes.DWORD),
    ]

k32 = ctypes.windll.kernel32
VQ = k32.VirtualQueryEx
VQ.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t]
VQ.restype = ctypes.c_size_t
h = pm.process_handle


def regions(max_size=1024 * 1024 * 1024):
    addr = 0
    mbi = MBI()
    while addr < 0x7FFFFFFFFFFF:
        if VQ(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)) == 0:
            break
        if mbi.State == MEM_COMMIT and mbi.Protect in VALID_PROT and mbi.RegionSize <= max_size:
            yield mbi.BaseAddress, mbi.RegionSize
        addr = mbi.BaseAddress + mbi.RegionSize


def parse_slot(data, off):
    if off + 16 > len(data):
        return None
    idx, x, z, typ, pad = struct.unpack_from('<iffHH', data, off)
    return idx, x, z, typ, pad


def is_valid_beacon_slot(slot):
    """Strict beacon slot check: type must be 0x0100 or 0x010A, pad=0."""
    if slot is None:
        return False
    idx, x, z, typ, pad = slot
    if pad != 0: return False
    if typ not in (0x0100, 0x010A): return False
    # Empty
    if idx == -1:
        return x == 0.0 and z == 0.0
    # Filled: need plausible coords
    if idx < 0 or idx > 65535: return False
    if not (100 < abs(x) < 25000): return False
    if not (100 < abs(z) < 25000): return False
    return True


def is_valid_stamp_slot(slot):
    """Stamp slot check."""
    if slot is None:
        return False
    idx, x, z, typ, pad = slot
    if pad != 0: return False
    hi = (typ >> 8) & 0xFF
    if idx == -1:
        return x == 0.0 and z == 0.0 and (typ == 0x0100 or hi == 0)
    if idx < 0 or idx > 65535: return False
    if hi not in (0x06, 0x08, 0x09): return False
    if not (100 < abs(x) < 25000): return False
    if not (100 < abs(z) < 25000): return False
    return True


# --- Strategy 1: find empty_beacon pattern and examine surroundings ---
print('\n=== Strategy 1: Find EMPTY_BEACON patterns ===')
empty_hits_by_region = 0
total_regions = 0
total_mb = 0
pattern_hits = []
for base, size in regions():
    total_regions += 1
    total_mb += size / (1024 * 1024)
    try:
        data = pm.read_bytes(base, size)
    except Exception:
        continue
    for pat in (EMPTY_BEACON, EMPTY_BEACON_DLC):
        idx = 0
        while True:
            found = data.find(pat, idx)
            if found < 0: break
            if found % 16 == 0:
                pattern_hits.append((base + found, pat, data[max(0, found - 80):found + 160]))
            idx = found + 1
    if len(pattern_hits) > 500: break

print(f'Regions scanned: {total_regions} (~{total_mb:.0f} MB)')
print(f'Aligned EMPTY_BEACON patterns: {len(pattern_hits)}')

# Look for regions where we have multiple adjacent beacon-shaped slots around an empty beacon
array_candidates = []
for addr, pat, ctx in pattern_hits:
    # Try to decode slots at -5..+10 positions relative to the empty beacon
    # The empty beacon is at offset 80 in ctx
    base_in_ctx = 80
    # Count consecutive valid beacon slots extending backward
    back = 0
    for k in range(1, 6):
        off = base_in_ctx - k * 16
        if off < 0: break
        s = parse_slot(ctx, off)
        if is_valid_beacon_slot(s): back += 1
        else: break
    # Count forward (including the empty itself)
    fwd = 0
    for k in range(0, 10):
        off = base_in_ctx + k * 16
        if off + 16 > len(ctx): break
        s = parse_slot(ctx, off)
        if is_valid_beacon_slot(s): fwd += 1
        else: break
    # Also look at slot right after beacons - should be stamp or empty
    after_beacons = parse_slot(ctx, base_in_ctx + fwd * 16)
    after_is_stamp = is_valid_stamp_slot(after_beacons) if after_beacons else False
    total = back + fwd
    if total >= 2:
        array_candidates.append((addr - back * 16, total, back, fwd, after_is_stamp, ctx))

# Dedup by array start address
seen = set()
unique = []
for start, total, back, fwd, after, ctx in sorted(array_candidates, key=lambda x: -x[1]):
    if start in seen: continue
    seen.add(start)
    unique.append((start, total, back, fwd, after, ctx))

print(f'\nBeacon-array candidates (valid slot chains >=2 around EMPTY_BEACON): {len(unique)}')
for start, total, back, fwd, after, ctx in unique[:20]:
    print(f'\n=== 0x{start:016X}  chain={total} (back={back} fwd={fwd}) after_stamp={after} ===')
    for k in range(min(total + 3, 10)):
        off = 80 - back * 16 + k * 16
        if off < 0 or off + 16 > len(ctx): continue
        s = parse_slot(ctx, off)
        if s is None:
            continue
        idx, x, z, typ, pad = s
        label = 'BEACON' if is_valid_beacon_slot(s) else ('STAMP' if is_valid_stamp_slot(s) else 'OTHER')
        if idx == -1:
            print(f'  [{k}] EMPTY  type=0x{typ:04X} pad={pad} ({label})')
        else:
            print(f'  [{k}] idx={idx:5d} x={x:10.2f} z={z:10.2f} type=0x{typ:04X} pad={pad} ({label})')
