"""Find beacon coordinates in Elden Ring process memory."""
import sys, io, struct
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import ctypes
from ctypes import wintypes
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
import config
import pymem

PID = config.require_eldenring_pid()

# Known beacons from slot 2
TARGETS = [
    (3101.7, 6728.5),
    (3068.0, 6455.1),
    (1877.0, 5810.8),
]
# Also try world-coord conversion (x+7042, -z+16511)
WORLD_TARGETS = [(x + 7042, -z + 16511) for x, z in TARGETS]

# Build search patterns: x float followed by z float (8 bytes)
patterns = []
for x, z in TARGETS:
    patterns.append(('MAP', x, z, struct.pack('<ff', x, z)))
for x, z in WORLD_TARGETS:
    patterns.append(('WORLD', x, z, struct.pack('<ff', x, z)))

# Also individual float searches with looser tolerance — scan for just X
# then verify Z nearby

pm = pymem.Pymem()
pm.open_process_from_id(PID)

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

hits = []
addr = 0; mbi = MBI()
total_regions = 0
while addr < 0x7FFFFFFFFFFF:
    if VQ(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)) == 0: break
    if mbi.State == MEM_COMMIT and mbi.Protect in VALID_PROT and mbi.RegionSize <= 1024*1024*1024:
        total_regions += 1
        try:
            data = pm.read_bytes(mbi.BaseAddress, mbi.RegionSize)
        except:
            addr = mbi.BaseAddress + mbi.RegionSize
            continue
        for label, x, z, pat in patterns:
            off = 0
            while True:
                i = data.find(pat, off)
                if i < 0: break
                # Grab 16-byte window
                base_off = max(0, i - 4)
                window = data[base_off:base_off+32]
                hits.append((mbi.BaseAddress + i, label, x, z, window))
                off = i + 1
    addr = mbi.BaseAddress + mbi.RegionSize

print(f'Regions scanned: {total_regions}')
print(f'Hits: {len(hits)}')
for hit_addr, label, x, z, win in hits[:60]:
    print(f'  0x{hit_addr:016X}  [{label}] target=({x:.1f},{z:.1f})  ctx={win.hex()}')
