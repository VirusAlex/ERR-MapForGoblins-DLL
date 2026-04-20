"""
Scan Elden Ring process memory for marker/beacon arrays.

Beacon format (from extract_markers.py):
  16 bytes = {int32 idx, float x, float z, uint16 type, uint16 pad}

Markers are placed in arrays of ~N slots. Empty slot pattern:
  idx=-1 (0xFFFFFFFF), x=0, z=0, type=0x0100, pad=0
  → bytes: FF FF FF FF 00 00 00 00 00 00 00 00 00 01 00 00

Strategy: find large regions where this 16-byte pattern repeats many times
(empty slots) — that reveals the beacon array base. Non-empty slots with
plausible coordinates are the actual markers.
"""
import sys, io, struct
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import ctypes
from ctypes import wintypes
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
import config
import pymem

PID = config.require_eldenring_pid()

EMPTY_BEACON = b"\xff\xff\xff\xff\x00\x00\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00"

pm = pymem.Pymem()
pm.open_process_from_id(PID)
print(f'Attached to PID {PID}')

# Enumerate writable/readable regions and scan for empty-beacon clusters.
# Using VirtualQueryEx via pymem / ctypes.

PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010

MEM_COMMIT = 0x1000
PAGE_READWRITE = 0x04
PAGE_READONLY = 0x02
PAGE_WRITECOPY = 0x08
PAGE_EXECUTE_READ = 0x20
PAGE_EXECUTE_READWRITE = 0x40

class MEMORY_BASIC_INFORMATION64(ctypes.Structure):
    _fields_ = [
        ("BaseAddress", ctypes.c_ulonglong),
        ("AllocationBase", ctypes.c_ulonglong),
        ("AllocationProtect", wintypes.DWORD),
        ("__alignment1", wintypes.DWORD),
        ("RegionSize", ctypes.c_ulonglong),
        ("State", wintypes.DWORD),
        ("Protect", wintypes.DWORD),
        ("Type", wintypes.DWORD),
        ("__alignment2", wintypes.DWORD),
    ]

kernel32 = ctypes.windll.kernel32
VirtualQueryEx = kernel32.VirtualQueryEx
VirtualQueryEx.argtypes = [wintypes.HANDLE, ctypes.c_void_p,
                           ctypes.POINTER(MEMORY_BASIC_INFORMATION64),
                           ctypes.c_size_t]
VirtualQueryEx.restype = ctypes.c_size_t

h = pm.process_handle

def iter_regions():
    addr = 0
    mbi = MEMORY_BASIC_INFORMATION64()
    while addr < 0x7FFFFFFFFFFF:
        r = VirtualQueryEx(h, ctypes.c_void_p(addr), ctypes.byref(mbi),
                           ctypes.sizeof(mbi))
        if r == 0: break
        if (mbi.State == MEM_COMMIT and
            mbi.Protect in (PAGE_READWRITE, PAGE_READONLY, PAGE_WRITECOPY,
                             PAGE_EXECUTE_READ, PAGE_EXECUTE_READWRITE) and
            mbi.RegionSize < 256*1024*1024):  # skip huge regions
            yield mbi.BaseAddress, mbi.RegionSize
        addr = mbi.BaseAddress + mbi.RegionSize


print('Scanning for empty-beacon patterns...')
hits = []
region_count = 0
scanned_mb = 0
for base, size in iter_regions():
    region_count += 1
    scanned_mb += size / (1024*1024)
    try:
        data = pm.read_bytes(base, size)
    except Exception:
        continue
    # Quick test: does it contain the empty-beacon pattern at all?
    idx = 0
    while True:
        found = data.find(EMPTY_BEACON, idx)
        if found < 0: break
        # Check if aligned on 16-byte boundary (marker arrays likely are)
        if found % 16 == 0:
            # Count consecutive empty-beacon patterns starting here
            p = found
            run = 0
            while p + 16 <= len(data) and data[p:p+16] == EMPTY_BEACON:
                run += 1; p += 16
            if run >= 4:  # at least 4 consecutive empties = probable array
                hits.append((base + found, run, data[max(0,found-32):found+run*16+64]))
        idx = found + 1

print(f'\nRegions scanned: {region_count}, total ~{scanned_mb:.0f} MB')
print(f'Found {len(hits)} clusters with >=4 consecutive empty beacons')
# Group hits by nearby address (same array)
for addr, run, context in hits[:10]:
    print(f'\n=== 0x{addr:016X}  run={run} empty slots ===')
    # Try to decode all slots before and after the empty run
    # Look backwards for non-empty slots (actual markers)
    pre = context[:32]
    post = context[32+run*16:]
    print(f'  pre  32 bytes: {pre.hex()}')
    print(f'  post 64 bytes: {post.hex()}')
    # Decode pre as possible markers
    for off in range(0, len(pre), 16):
        if off + 16 > len(pre): break
        entry = pre[off:off+16]
        idx_, x, z, typ, pad = struct.unpack('<iffHH', entry)
        if idx_ >= 0:
            print(f'    PRE  idx={idx_} x={x:.1f} z={z:.1f} type=0x{typ:04X}')
