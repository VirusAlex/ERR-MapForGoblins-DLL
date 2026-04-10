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
    data_off = struct.unpack_from("<i", bnd, eoff + 0x18)[0]
    name_off = struct.unpack_from("<i", bnd, eoff + 0x20)[0]
    uncomp_size = struct.unpack_from("<q", bnd, eoff + 0x10)[0]
    j = name_off
    name = ""
    while j + 1 < len(bnd):
        ch = struct.unpack_from("<H", bnd, j)[0]
        if ch == 0:
            break
        name += chr(ch)
        j += 2
    if "PlaceName.fmg" not in name or "_dlc" in name:
        continue

    fmg = bnd[data_off : data_off + uncomp_size]
    group_count = struct.unpack_from("<i", fmg, 0x0C)[0]
    entry_count = struct.unpack_from("<i", fmg, 0x10)[0]
    str_off_table = struct.unpack_from("<I", fmg, 0x18)[0]
    print(f"PlaceName.fmg: {entry_count} entries, {group_count} groups")

    targets = [10600001, 10600002, 10500002]
    for target in targets:
        found = False
        for g in range(group_count):
            off = 0x28 + g * 16
            idx_start = struct.unpack_from("<i", fmg, off)[0]
            id_start = struct.unpack_from("<i", fmg, off + 4)[0]
            id_end = struct.unpack_from("<i", fmg, off + 8)[0]
            if id_start <= target <= id_end:
                entry_idx = idx_start + (target - id_start)
                soff = str_off_table + entry_idx * 8
                if soff + 8 <= len(fmg):
                    s_off = struct.unpack_from("<q", fmg, soff)[0]
                    if s_off > 0 and s_off < len(fmg):
                        s = b""
                        pos = s_off
                        while pos < len(fmg) - 1:
                            c = fmg[pos : pos + 2]
                            if c == b"\x00\x00":
                                break
                            s += c
                            pos += 2
                        text = s.decode("utf-16-le")
                        print(f"  ID {target}: \"{text}\"")
                        found = True
                    else:
                        print(f"  ID {target}: <null offset>")
                        found = True
                break
        if not found:
            print(f"  ID {target}: NOT FOUND")
    break
