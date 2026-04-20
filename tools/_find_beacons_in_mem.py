"""Find beacons in live game memory.

Beacons (map coords) from slot 2:
  B1: (3101.7, 6728.5)
  B2: (3068.0, 6455.1)
  B3: (1877.0, 5810.8)
  B4: (4061.3, 8599.9)
  S1: (3644.9, 8161.8)  [stamp]

Map coord formula:
  worldX = mapX + 7042
  worldZ = -mapZ + 16511

Strategy: for each beacon, pack (x, z) as two floats = 8 bytes, search memory.
Also try swapped order (z, x) and world-coord variants.
Report all hits with 128-byte surrounding context.
"""
import sys, io, ctypes, struct
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
from ctypes import wintypes
import pymem

PID = int(sys.argv[1]) if len(sys.argv) > 1 else 18072

BEACONS = [
    ("B1_map",  3101.7, 6728.5),
    ("B2_map",  3068.0, 6455.1),
    ("B3_map",  1877.0, 5810.8),
    ("B4_map",  4061.3, 8599.9),
    ("S1_map",  3644.9, 8161.8),
]

def world(m): return (m[0] + 7042, -m[1] + 16511)

def make_patterns(name, x, z):
    patterns = []
    for tag, fx, fz in [("xz", x, z), ("zx", z, x)]:
        p = struct.pack('<ff', fx, fz)
        patterns.append((f"{name}_{tag}", p, fx, fz))
    wx, wz = x + 7042, -z + 16511
    for tag, fx, fz in [("Wxz", wx, wz), ("Wzx", wz, wx)]:
        p = struct.pack('<ff', fx, fz)
        patterns.append((f"{name}_{tag}", p, fx, fz))
    return patterns

ALL = []
for name, x, z in BEACONS:
    ALL.extend(make_patterns(name, x, z))

pm = pymem.Pymem(); pm.open_process_from_id(PID)
print(f'Attached to PID {PID}')

MEM_COMMIT = 0x1000
VALID_PROT = {0x02, 0x04, 0x08, 0x20, 0x40}

class MBI(ctypes.Structure):
    _fields_ = [
        ("BaseAddress", ctypes.c_ulonglong), ("AllocationBase", ctypes.c_ulonglong),
        ("AllocationProtect", wintypes.DWORD), ("__a1", wintypes.DWORD),
        ("RegionSize", ctypes.c_ulonglong), ("State", wintypes.DWORD),
        ("Protect", wintypes.DWORD), ("Type", wintypes.DWORD), ("__a2", wintypes.DWORD),
    ]
k32 = ctypes.windll.kernel32
VQ = k32.VirtualQueryEx
VQ.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t]
VQ.restype = ctypes.c_size_t
h = pm.process_handle

def regions():
    addr = 0; mbi = MBI()
    while addr < 0x7FFFFFFFFFFF:
        if VQ(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)) == 0: break
        if mbi.State == MEM_COMMIT and mbi.Protect in VALID_PROT and mbi.RegionSize <= 1024*1024*1024:
            yield mbi.BaseAddress, mbi.RegionSize
        addr = mbi.BaseAddress + mbi.RegionSize

# Scan once, collect all hits for all patterns
hits_by_tag = {tag: [] for tag, _, _, _ in ALL}
total_regions = 0; total_mb = 0
for base, size in regions():
    total_regions += 1
    total_mb += size / (1024*1024)
    try: data = pm.read_bytes(base, size)
    except Exception: continue
    for tag, pat, fx, fz in ALL:
        off = 0
        while True:
            i = data.find(pat, off)
            if i < 0: break
            # Only aligned finds (4-byte aligned for float arrays)
            if i % 4 == 0:
                ctx_pre = data[max(0,i-64):i]
                ctx_post = data[i:i+96]
                hits_by_tag[tag].append((base+i, ctx_pre, ctx_post))
            off = i + 1

print(f'\nScanned {total_regions} regions ({total_mb:.0f} MB)\n')
for tag, _, fx, fz in ALL:
    hits = hits_by_tag[tag]
    print(f'{tag}  ({fx:.1f}, {fz:.1f}): {len(hits)} hits')

# Detailed output for any tag with <=10 hits (to keep output manageable)
def fmt(b, a):
    return f'  {a:016x}  ' + ' '.join(f'{x:02x}' for x in b)

print('\n=== Detail for non-empty results ===\n')
for tag, _, fx, fz in ALL:
    hits = hits_by_tag[tag]
    if not hits: continue
    print(f'\n--- {tag}  ({fx:.1f}, {fz:.1f})  [{len(hits)} hit(s)] ---')
    for addr, pre, post in hits[:8]:
        print(f'  @ 0x{addr:016x}')
        # pre in 16-byte chunks
        for k in range(0, len(pre), 16):
            chunk = pre[k:k+16]
            print(fmt(chunk, addr - len(pre) + k))
        print('  >>>>>')
        for k in range(0, len(post), 16):
            chunk = post[k:k+16]
            print(fmt(chunk, addr + k))
