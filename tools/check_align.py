import ctypes, struct
import config

oodle = ctypes.cdll.LoadLibrary(str(config.require_oo2core()))
decompress = oodle.OodleLZ_Decompress
decompress.restype = ctypes.c_int
decompress.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_int,
                        ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_void_p,
                        ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
                        ctypes.c_int, ctypes.c_int]

def dcx_decompress(data):
    uncomp_size = struct.unpack('>I', data[0x1C:0x20])[0]
    comp_size = struct.unpack('>I', data[0x20:0x24])[0]
    out = ctypes.create_string_buffer(uncomp_size)
    result = decompress(data[0x4C:0x4C+comp_size], comp_size, out, uncomp_size, 0, 0, 0, None, 0, None, None, None, 0, 3)
    return bytes(out)[:result]

import sys

if len(sys.argv) < 2:
    print("Usage: check_align.py <file1.dcx> [file2.dcx ...]")
    print("Checks BND4 data alignment in DCX-compressed files.")
    sys.exit(1)

for path in sys.argv[1:]:
    label = os.path.basename(path)
    data = dcx_decompress(open(path, 'rb').read())
    
    file_count = struct.unpack_from('<i', data, 0x0C)[0]
    fhs = struct.unpack_from('<q', data, 0x20)[0]
    
    print(f'\n=== {label}: Alignment check ===')
    misaligned = 0
    for i in range(file_count):
        off = 0x40 + i * fhs
        data_off = struct.unpack_from('<i', data, off + 0x18)[0]
        comp_size = struct.unpack_from('<q', data, off + 0x08)[0]
        aligned = data_off % 16 == 0
        if not aligned:
            name_off = struct.unpack_from('<i', data, off + 0x20)[0]
            name = ''
            j = name_off
            while j + 1 < len(data):
                ch = struct.unpack_from('<H', data, j)[0]
                if ch == 0: break
                name += chr(ch) if ch < 128 else '?'
                j += 2
            short = name.split('\\')[-1]
            print(f'  [{i:2d}] MISALIGNED: off=0x{data_off:08X} (mod16={data_off%16}) {short}')
            misaligned += 1
    if misaligned == 0:
        print('  All files 16-byte aligned!')
    else:
        print(f'  {misaligned} files misaligned!')
