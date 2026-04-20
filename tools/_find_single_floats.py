"""Find single-float beacon coordinates in live memory.

If no (x,z) pair found, try each float individually to learn if coords
are stored at all and in what form (float/double/int-scaled).
"""
import sys, io, ctypes, struct
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
from ctypes import wintypes
import pymem

PID = int(sys.argv[1]) if len(sys.argv) > 1 else 18072

# Values to try per beacon (map + world forms, float + double + int*10/100)
VALUES = []
for name, mx, mz in [("B1",3101.7,6728.5),("B2",3068.0,6455.1),
                     ("B3",1877.0,5810.8),("B4",4061.3,8599.9),
                     ("S1",3644.9,8161.8)]:
    wx, wz = mx + 7042, -mz + 16511
    for tag, v in [(f"{name}_mapX",mx),(f"{name}_mapZ",mz),
                    (f"{name}_wX",wx),(f"{name}_wZ",wz)]:
        VALUES.append((tag+"_float",  struct.pack('<f', v),  v, "float"))
        VALUES.append((tag+"_double", struct.pack('<d', v),  v, "double"))

pm = pymem.Pymem(); pm.open_process_from_id(PID)

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

counts = {tag: 0 for tag, _, _, _ in VALUES}
sample_addrs = {tag: [] for tag, _, _, _ in VALUES}
for base, size in regions():
    try: data = pm.read_bytes(base, size)
    except Exception: continue
    for tag, pat, v, kind in VALUES:
        off = 0
        while True:
            i = data.find(pat, off)
            if i < 0: break
            # accept any alignment for initial counting
            counts[tag] += 1
            if len(sample_addrs[tag]) < 4:
                sample_addrs[tag].append(base + i)
            off = i + 1

print('Per-pattern raw hit counts (any alignment):\n')
for tag, pat, v, kind in VALUES:
    if counts[tag]:
        print(f'  {tag:30s}  {v:10.2f}  {counts[tag]:6d}  samples: ' +
              ', '.join(f'0x{a:x}' for a in sample_addrs[tag]))

# Summary: which beacon has both coords (or all beacon coords) findable?
print('\nSummary by kind:')
for kind in ('float', 'double'):
    any_found = [t for t,_,_,k in VALUES if k==kind and counts[t]>0]
    print(f'  {kind}: {len(any_found)} of {sum(1 for _,_,_,k in VALUES if k==kind)} tags have hits')
