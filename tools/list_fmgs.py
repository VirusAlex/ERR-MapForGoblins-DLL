import ctypes, struct, os
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
    comp_data = data[0x4C : 0x4C + comp_size]
    result = decompress(comp_data, comp_size, out, uncomp_size, 0, 0, 0, None, 0, None, None, None, 0, 3)
    if result <= 0:
        raise RuntimeError(f"Decompression failed: {result}")
    return bytes(out)[:result]

def parse_bnd4(data):
    file_count = struct.unpack_from("<i", data, 0x0C)[0]
    file_header_size = struct.unpack_from("<q", data, 0x20)[0]
    entries = []
    for i in range(file_count):
        off = 0x40 + i * file_header_size
        comp_size = struct.unpack_from("<q", data, off + 0x08)[0]
        uncomp_size = struct.unpack_from("<q", data, off + 0x10)[0] if file_header_size >= 0x24 else comp_size
        data_off = struct.unpack_from("<i", data, off + 0x18)[0]
        name_off = struct.unpack_from("<i", data, off + 0x20)[0]
        
        name = ""
        if name_off > 0 and name_off < len(data):
            j = name_off
            while j + 1 < len(data):
                ch = struct.unpack_from("<H", data, j)[0]
                if ch == 0:
                    break
                name += chr(ch)
                j += 2
        
        fmg_data = data[data_off : data_off + uncomp_size]
        entries.append({"name": name, "data": fmg_data, "size": uncomp_size})
    return entries

def search_fmg_for_id(fmg_data, target):
    if len(fmg_data) < 0x28:
        return None
    entry_count = struct.unpack_from("<i", fmg_data, 0x10)[0]
    str_off_table = struct.unpack_from("<q", fmg_data, 0x18)[0]
    group_count = struct.unpack_from("<i", fmg_data, 0x0C)[0]
    for g in range(group_count):
        off = 0x28 + g * 0x18
        if off + 12 > len(fmg_data):
            break
        idx_start = struct.unpack_from("<i", fmg_data, off)[0]
        id_start = struct.unpack_from("<i", fmg_data, off + 4)[0]
        id_end = struct.unpack_from("<i", fmg_data, off + 8)[0]
        if id_start <= target <= id_end:
            entry_idx = idx_start + (target - id_start)
            if entry_idx < entry_count:
                soff = str_off_table + entry_idx * 8
                if soff + 8 <= len(fmg_data):
                    str_off = struct.unpack_from("<q", fmg_data, soff)[0]
                    if str_off > 0 and str_off < len(fmg_data):
                        s = b""
                        pos = str_off
                        while pos < len(fmg_data) - 1:
                            c = fmg_data[pos : pos + 2]
                            if c == b"\x00\x00":
                                break
                            s += c
                            pos += 2
                        return s.decode("utf-16-le", errors="replace")
                    return f"<null offset {str_off}>"
    return None

dcx_path = str(config.require_err_mod_dir() / "msg" / "engus" / "item_dlc02.msgbnd.dcx")
print(f"=== {dcx_path} ===")
raw = open(dcx_path, "rb").read()
bnd = dcx_decompress(raw)
entries = parse_bnd4(bnd)

for i, e in enumerate(entries):
    basename = e["name"].split("\\")[-1] if "\\" in e["name"] else e["name"]
    print(f"  [{i}] {basename} ({e['size']} bytes)")

target = 10500002
print(f"\nSearching for ID {target}...")
for e in entries:
    result = search_fmg_for_id(e["data"], target)
    if result is not None:
        basename = e["name"].split("\\")[-1]
        print(f"  FOUND in {basename}: \"{result}\"")

print(f"\nSearching for 'Shadowed Curio' text...")
needle = "Shadowed Curio".encode("utf-16-le")
for e in entries:
    if needle in e["data"]:
        basename = e["name"].split("\\")[-1]
        print(f"  Found in: {basename}")
