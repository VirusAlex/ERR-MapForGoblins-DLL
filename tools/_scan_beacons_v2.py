"""
Scan Elden Ring memory for placed beacon/marker arrays.

Strategy: look for 16-byte aligned entries matching:
  - int32 idx in [0, 65535]  (valid positive index)
  - float x in [-20000, 20000]  (world coordinate range)
  - float z in [-30000, 30000]
  - uint16 type in [0x0100..0x09FF]  (beacon/icon/stamp)
  - uint16 pad = 0

Clusters of >=3 consecutive such entries at 16-byte stride = marker array.
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
pm = pymem.Pymem()
pm.open_process_from_id(PID)

PROCESS_QUERY_INFORMATION = 0x0400
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


def regions(max_size=1024*1024*1024):
    addr = 0; mbi = MBI()
    while addr < 0x7FFFFFFFFFFF:
        if VQ(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)) == 0:
            break
        if mbi.State == MEM_COMMIT and mbi.Protect in VALID_PROT and mbi.RegionSize <= max_size:
            yield mbi.BaseAddress, mbi.RegionSize
        addr = mbi.BaseAddress + mbi.RegionSize


def plausible_marker(entry):
    if len(entry) != 16: return None
    idx, x, z, typ, pad = struct.unpack('<iffHH', entry)
    if pad != 0: return None
    if typ < 0x0100 or typ > 0x09FF: return None
    if typ & 0xFF > 0x50 and typ & 0xFF != 0: return None  # low byte typically small
    if not (0 <= idx <= 65535): return None
    if not (-30000 <= x <= 30000): return None
    if not (-30000 <= z <= 30000): return None
    # exclude too-small floats (0 coord is empty)
    if abs(x) < 1 and abs(z) < 1: return None
    return idx, x, z, typ


clusters = []  # (base_addr, [entries])
total_regions = 0
total_mb = 0
for base, size in regions():
    total_regions += 1
    total_mb += size / (1024*1024)
    try:
        data = pm.read_bytes(base, size)
    except Exception:
        continue
    # Scan aligned positions
    for off in range(0, len(data) - 16, 16):
        entry = data[off:off+16]
        m = plausible_marker(entry)
        if m is None: continue
        # Check next entry: plausible OR empty beacon
        chain = [m]
        p = off + 16
        while p + 16 <= len(data):
            nxt = data[p:p+16]
            if nxt == b"\xff\xff\xff\xff\x00\x00\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00":
                chain.append(('EMPTY', 0, 0, 0x0100))
            else:
                nm = plausible_marker(nxt)
                if nm is None: break
                chain.append(nm)
            p += 16
        if len(chain) >= 2:
            clusters.append((base + off, chain))
            off = p  # skip past cluster
            # but the for-loop will continue from next 16-stride

print(f'Regions scanned: {total_regions} (~{total_mb:.0f} MB)')
print(f'Candidate clusters (>=2 entries, aligned, plausible): {len(clusters)}')

# Dedup clusters that overlap (same starting region)
seen_addrs = set()
unique = []
for addr, chain in sorted(clusters, key=lambda c: -len(c[1])):
    if addr in seen_addrs: continue
    unique.append((addr, chain))
    for i in range(len(chain)):
        seen_addrs.add(addr + i*16)
    if len(unique) >= 30: break

print(f'Top {len(unique)} unique cluster heads:')
for addr, chain in unique:
    print(f'\n=== 0x{addr:016X}  chain_len={len(chain)} ===')
    for i, e in enumerate(chain[:8]):
        if e == 'EMPTY' or e[0] == 'EMPTY':
            print(f'  [{i}] EMPTY')
        else:
            idx, x, z, typ = e
            print(f'  [{i}] idx={idx} x={x:.1f} z={z:.1f} type=0x{typ:04X}')
