"""Dump hex context around each EMPTY_BEACON pattern to reveal the real layout."""
import sys, io, ctypes, struct
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
from ctypes import wintypes
import pymem

PID = int(sys.argv[1]) if len(sys.argv) > 1 else 18072

EMPTY_BEACON = b"\xff\xff\xff\xff\x00\x00\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00"
EMPTY_BEACON_DLC = b"\xff\xff\xff\xff\x00\x00\x00\x00\x00\x00\x00\x00\x0a\x01\x00\x00"

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


def regions():
    addr = 0; mbi = MBI()
    while addr < 0x7FFFFFFFFFFF:
        if VQ(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)) == 0: break
        if mbi.State == MEM_COMMIT and mbi.Protect in VALID_PROT and mbi.RegionSize <= 1024*1024*1024:
            yield mbi.BaseAddress, mbi.RegionSize
        addr = mbi.BaseAddress + mbi.RegionSize


hits = []
for base, size in regions():
    try: data = pm.read_bytes(base, size)
    except Exception: continue
    for pat in (EMPTY_BEACON, EMPTY_BEACON_DLC):
        idx = 0
        while True:
            found = data.find(pat, idx)
            if found < 0: break
            if found % 16 == 0:
                pre = data[max(0, found - 128):found]
                post = data[found:found + 256]
                hits.append((base + found, pat, pre, post))
            idx = found + 1

print(f'Found {len(hits)} aligned EMPTY_BEACON patterns\n')

def hex_dump(data, base_addr, cols=16):
    lines = []
    for i in range(0, len(data), cols):
        chunk = data[i:i+cols]
        hexpart = ' '.join(f'{b:02x}' for b in chunk)
        asciipart = ''.join(chr(b) if 32 <= b < 127 else '.' for b in chunk)
        lines.append(f'  {base_addr+i:016x}  {hexpart:<48}  {asciipart}')
    return '\n'.join(lines)

def decode_slots(data, base_addr, slots=8):
    lines = []
    for i in range(slots):
        off = i * 16
        if off + 16 > len(data): break
        idx, x, z, typ, pad = struct.unpack_from('<iffHH', data, off)
        if idx == -1 and x == 0.0 and z == 0.0:
            lines.append(f'  +{off:04x} @0x{base_addr+off:x}: EMPTY idx=-1 type=0x{typ:04X} pad=0x{pad:04X}')
        else:
            lines.append(f'  +{off:04x} @0x{base_addr+off:x}: idx={idx} x={x:.2f} z={z:.2f} type=0x{typ:04X} pad=0x{pad:04X}')
    return '\n'.join(lines)

for addr, pat, pre, post in hits:
    tag = 'DLC' if pat == EMPTY_BEACON_DLC else 'BASE'
    print(f'=== 0x{addr:016X}  ({tag} empty beacon) ===')
    print('-- preceding 128 bytes (hex) --')
    print(hex_dump(pre, addr - len(pre)))
    print('-- this and next 256 bytes (hex) --')
    print(hex_dump(post, addr))
    print('-- slot-decoded view of post --')
    print(decode_slots(post, addr, slots=16))
    print()
