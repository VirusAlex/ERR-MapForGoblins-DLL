import struct, ctypes, os
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
    if data[:4] != b"DCX\x00":
        return data
    uncomp_size = struct.unpack(">I", data[0x1C:0x20])[0]
    comp_size = struct.unpack(">I", data[0x20:0x24])[0]
    out = ctypes.create_string_buffer(uncomp_size)
    comp_data = data[0x4C : 0x4C + comp_size]
    result = decompress(comp_data, comp_size, out, uncomp_size, 0, 0, 0, None, 0, None, None, None, 0, 3)
    if result <= 0:
        raise RuntimeError(f"Decompression failed: {result}")
    return bytes(out)[:result]

def parse_bnd4_fmgs(bnd_data):
    assert bnd_data[:4] == b"BND4"
    file_count = struct.unpack_from("<i", bnd_data, 0x0C)[0]
    header_size = 0x40
    entry_size = 0x24
    fmgs = []
    for i in range(file_count):
        eoff = header_size + i * entry_size
        comp_size = struct.unpack_from("<i", bnd_data, eoff + 0x04)[0]
        data_offset = struct.unpack_from("<I", bnd_data, eoff + 0x08)[0]
        name_offset = struct.unpack_from("<I", bnd_data, eoff + 0x14)[0]
        uncomp_size = struct.unpack_from("<i", bnd_data, eoff + 0x1C)[0]
        name_end = bnd_data.index(b"\x00", name_offset)
        name = bnd_data[name_offset:name_end].decode("utf-8", errors="replace")
        fmg_data = bnd_data[data_offset : data_offset + uncomp_size]
        if b"PlaceName" in name.encode():
            fmgs.append((name, fmg_data))
    return fmgs

def search_fmg_for_id(fmg_data, target_id):
    if len(fmg_data) < 0x28 or fmg_data[:1] != b"\x00":
        return None
    entry_count = struct.unpack_from("<i", fmg_data, 0x10)[0]
    str_off_table = struct.unpack_from("<q", fmg_data, 0x18)[0]
    group_count = struct.unpack_from("<i", fmg_data, 0x0C)[0]
    groups_off = 0x28
    for g in range(group_count):
        off = groups_off + g * 0x18
        idx_start = struct.unpack_from("<i", fmg_data, off)[0]
        id_start = struct.unpack_from("<i", fmg_data, off + 4)[0]
        id_end = struct.unpack_from("<i", fmg_data, off + 8)[0]
        if id_start <= target_id <= id_end:
            entry_idx = idx_start + (target_id - id_start)
            if entry_idx < entry_count:
                str_off = struct.unpack_from("<q", fmg_data, str_off_table + entry_idx * 8)[0]
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
                else:
                    return "<null offset>"
    return None

dcx_path = str(config.require_err_mod_dir() / "msg" / "engus" / "item_dlc02.msgbnd.dcx")
print(f"Loading {dcx_path}...")
raw = open(dcx_path, "rb").read()
bnd = dcx_decompress(raw)
fmgs = parse_bnd4_fmgs(bnd)
print(f"Found {len(fmgs)} PlaceName FMGs")

target = 10500002
for name, fmg_data in fmgs:
    result = search_fmg_for_id(fmg_data, target)
    if result is not None:
        print(f"  {name}: ID {target} = \"{result}\"")

print("\nSearching ALL FMGs for 'Shadowed Curio'...")
assert bnd[:4] == b"BND4"
file_count = struct.unpack_from("<i", bnd, 0x0C)[0]
for i in range(file_count):
    eoff = 0x40 + i * 0x24
    data_offset = struct.unpack_from("<I", bnd, eoff + 0x08)[0]
    name_offset = struct.unpack_from("<I", bnd, eoff + 0x14)[0]
    uncomp_size = struct.unpack_from("<i", bnd, eoff + 0x1C)[0]
    name_end = bnd.index(b"\x00", name_offset)
    name = bnd[name_offset:name_end].decode("utf-8", errors="replace")
    fmg_data = bnd[data_offset : data_offset + uncomp_size]
    if b"Shadowed Curio".decode().encode("utf-16-le") in fmg_data:
        print(f"  Found in: {name}")
