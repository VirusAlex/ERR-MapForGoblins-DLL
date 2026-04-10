"""List FMG file IDs in item_dlc02.msgbnd.dcx."""
import ctypes, struct
import config

dll_path = str(config.require_oo2core())
oodle = ctypes.cdll.LoadLibrary(dll_path)
decompress = oodle.OodleLZ_Decompress
decompress.restype = ctypes.c_int
decompress.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_int,
                        ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_void_p,
                        ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
                        ctypes.c_int, ctypes.c_int]

def dcx_decompress(data):
    uncomp_size = struct.unpack(">I", data[0x1C:0x20])[0]
    comp_size = struct.unpack(">I", data[0x20:0x24])[0]
    out = ctypes.create_string_buffer(uncomp_size)
    result = decompress(data[0x4C:0x4C+comp_size], comp_size, out, uncomp_size, 0, 0, 0, None, 0, None, None, None, 0, 3)
    return bytes(out)[:result]

dcx = str(config.require_err_mod_dir() / "msg" / "engus" / "item_dlc02.msgbnd.dcx")
bnd = dcx_decompress(open(dcx, "rb").read())

file_count = struct.unpack_from("<i", bnd, 0x0C)[0]
fhs = struct.unpack_from("<q", bnd, 0x20)[0]

for i in range(file_count):
    eoff = 0x40 + i * fhs
    file_id = struct.unpack_from("<i", bnd, eoff + 0x1C)[0]
    name_off = struct.unpack_from("<i", bnd, eoff + 0x20)[0]
    j = name_off
    name = ""
    while j + 1 < len(bnd):
        ch = struct.unpack_from("<H", bnd, j)[0]
        if ch == 0: break
        name += chr(ch)
        j += 2
    basename = name.split("\\")[-1]
    print(f"  file_id={file_id:>4}  {basename}")
