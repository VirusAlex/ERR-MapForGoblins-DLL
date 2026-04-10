"""Find free PlaceName IDs in the game FMG."""
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

GROUP_SIZE = 16

def dcx_decompress(data):
    uncomp_size = struct.unpack(">I", data[0x1C:0x20])[0]
    comp_size = struct.unpack(">I", data[0x20:0x24])[0]
    out = ctypes.create_string_buffer(uncomp_size)
    comp_data = data[0x4C : 0x4C + comp_size]
    result = decompress(comp_data, comp_size, out, uncomp_size, 0, 0, 0, None, 0, None, None, None, 0, 3)
    if result <= 0:
        raise RuntimeError(f"Decompression failed: {result}")
    return bytes(out)[:result]

def get_placename_fmg(dcx_path):
    raw = open(dcx_path, "rb").read()
    bnd = dcx_decompress(raw)
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
            if ch == 0: break
            name += chr(ch)
            j += 2
        if "PlaceName.fmg" in name and "_dlc" not in name:
            return bnd[data_off : data_off + uncomp_size]
    return None

def read_fmg_ids(fmg_data):
    group_count = struct.unpack_from("<i", fmg_data, 0x0C)[0]
    entry_count = struct.unpack_from("<i", fmg_data, 0x10)[0]
    str_off_table = struct.unpack_from("<I", fmg_data, 0x18)[0]
    
    ids_with_text = {}
    for g in range(group_count):
        off = 0x28 + g * GROUP_SIZE
        idx_start = struct.unpack_from("<i", fmg_data, off)[0]
        id_start = struct.unpack_from("<i", fmg_data, off + 4)[0]
        id_end = struct.unpack_from("<i", fmg_data, off + 8)[0]
        for i in range(id_end - id_start + 1):
            entry_id = id_start + i
            entry_idx = idx_start + i
            soff = str_off_table + entry_idx * 8
            if soff + 8 > len(fmg_data):
                continue
            str_off = struct.unpack_from("<q", fmg_data, soff)[0]
            if str_off > 0 and str_off < len(fmg_data):
                s = b""
                pos = str_off
                while pos < len(fmg_data) - 1:
                    c = fmg_data[pos : pos + 2]
                    if c == b"\x00\x00": break
                    s += c
                    pos += 2
                text = s.decode("utf-16-le", errors="replace")
                ids_with_text[entry_id] = text
            else:
                ids_with_text[entry_id] = None
    return ids_with_text

dcx = str(config.require_err_mod_dir() / "msg" / "engus" / "item_dlc02.msgbnd.dcx")
fmg = get_placename_fmg(dcx)
if fmg:
    ids = read_fmg_ids(fmg)
    print(f"PlaceName.fmg has {len(ids)} IDs")
    
    for eid in sorted(ids):
        if 10499990 <= eid <= 10500100:
            txt = ids[eid]
            if txt:
                print(f"  {eid}: {txt[:80]}")
            else:
                print(f"  {eid}: <null>")
    
    # Find free IDs in range 10600000+
    print("\nFree IDs in 10600000-10600010:")
    for check in range(10600000, 10600010):
        if check not in ids:
            print(f"  {check} - FREE")
        else:
            print(f"  {check} - USED: {ids[check]}")

    print("\nFree IDs in 10550000-10550010:")
    for check in range(10550000, 10550010):
        if check not in ids:
            print(f"  {check} - FREE")
        else:
            print(f"  {check} - USED: {ids[check]}")
else:
    print("PlaceName.fmg not found!")
