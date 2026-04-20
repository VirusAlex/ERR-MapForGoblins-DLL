"""Get decrypted slot bytes, find beacon entry bytes, then search game memory."""
import sys, io, os, struct, re
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
import config
os.environ.setdefault("PYTHONNET_RUNTIME", "coreclr")
from pythonnet import load
load("coreclr")
import clr
from System import Array, Object
from System import Type as SysType
from System.Reflection import Assembly, BindingFlags

dll = str(config.SOULSFORMATS_DLL)
asm = Assembly.LoadFrom(dll); clr.AddReference(dll)
_str_type = SysType.GetType("System.String")
_bnd4_read = asm.GetType("SoulsFormats.BND4").GetMethod("Read",
    BindingFlags.Public|BindingFlags.Static|BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str_type]), None)

SAVE = str(config.require_active_save())
SLOT = 2
bnd = _bnd4_read.Invoke(None, Array[Object]([SAVE]))
slot_data = bytes(bnd.Files[SLOT].Bytes.ToArray())
print(f'Slot {SLOT} decrypted size: {len(slot_data)}')

# Search for beacon 1: idx=?, x=3101.7, z=6728.5, type=0x0100 (as raw bytes in slot)
xbytes = struct.pack('<f', 3101.7)
zbytes = struct.pack('<f', 6728.5)
pat = xbytes + zbytes
positions = [m.start() for m in re.finditer(re.escape(pat), slot_data)]
print(f'Beacon 1 xz pattern in slot: {positions[:5]}')

beacon_entries = []
if positions:
    for pos in positions[:3]:
        entry_start = pos - 4
        entry = slot_data[entry_start:entry_start+16]
        idx, fx, fz, typ, pad = struct.unpack('<iffHH', entry)
        print(f'  @ slot offset 0x{entry_start:X}: idx={idx} x={fx:.1f} z={fz:.1f} type=0x{typ:04X} pad=0x{pad:04X}')
        print(f'    bytes: {entry.hex()}')
        beacon_entries.append((entry, entry_start))

# Now search GAME MEMORY for these exact 16-byte entries
import ctypes
from ctypes import wintypes
import pymem
PID = config.require_eldenring_pid()
pm = pymem.Pymem(); pm.open_process_from_id(PID)

MEM_COMMIT = 0x1000
VALID_PROT = {0x02, 0x04, 0x08, 0x20, 0x40}
class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_ulonglong),("AllocationBase", ctypes.c_ulonglong),
        ("AllocationProtect", wintypes.DWORD),("__a1", wintypes.DWORD),
        ("RegionSize", ctypes.c_ulonglong),("State", wintypes.DWORD),
        ("Protect", wintypes.DWORD),("Type", wintypes.DWORD),("__a2", wintypes.DWORD)]
k32 = ctypes.windll.kernel32
VQ = k32.VirtualQueryEx
VQ.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t]
VQ.restype = ctypes.c_size_t
h = pm.process_handle

print('\nScanning game memory for beacon 1 entry bytes...')
total_hits = 0
for entry, slot_off in beacon_entries:
    print(f'\nSearching for: {entry.hex()}')
    hits = []
    addr = 0; mbi = MBI()
    while addr < 0x7FFFFFFFFFFF:
        if VQ(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)) == 0: break
        if mbi.State == MEM_COMMIT and mbi.Protect in VALID_PROT and mbi.RegionSize <= 512*1024*1024:
            try:
                data = pm.read_bytes(mbi.BaseAddress, mbi.RegionSize)
                off = 0
                while True:
                    i = data.find(entry, off)
                    if i < 0: break
                    hits.append(mbi.BaseAddress + i)
                    off = i + 1
            except: pass
        addr = mbi.BaseAddress + mbi.RegionSize
    print(f'  Hits: {len(hits)}')
    for a in hits[:10]:
        print(f'    0x{a:016X}')
    total_hits += len(hits)

print(f'\nTotal hits: {total_hits}')
