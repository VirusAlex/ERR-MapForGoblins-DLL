import ctypes, struct, os
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
    comp_data = data[0x4C:0x4C+comp_size]
    result = decompress(comp_data, comp_size, out, uncomp_size, 0, 0, 0, None, 0, None, None, None, 0, 3)
    if result <= 0:
        raise RuntimeError(f'Decompress failed: {result}')
    return bytes(out)[:result]

def parse_fmg_entries(data):
    if len(data) < 0x28: return []
    version = data[2]
    wide = (version == 2)
    group_count = struct.unpack_from('<I', data, 0x0C)[0]
    string_count = struct.unpack_from('<I', data, 0x10)[0]
    if wide:
        str_off_ofs = struct.unpack_from('<Q', data, 0x18)[0]
    else:
        str_off_ofs = struct.unpack_from('<I', data, 0x18)[0]
    
    entries = []
    for g in range(group_count):
        goff = 0x28 + g * (16 if wide else 12)
        offset_index = struct.unpack_from('<I', data, goff)[0]
        first_id = struct.unpack_from('<I', data, goff + 4)[0]
        last_id = struct.unpack_from('<I', data, goff + 8)[0]
        
        for j in range(last_id - first_id + 1):
            si = offset_index + j
            if wide:
                soff = struct.unpack_from('<q', data, str_off_ofs + si * 8)[0]
            else:
                soff = struct.unpack_from('<i', data, str_off_ofs + si * 4)[0]
            
            eid = first_id + j
            if soff > 0 and soff < len(data):
                text = ''
                pos = soff
                while pos + 1 < len(data):
                    ch = struct.unpack_from('<H', data, pos)[0]
                    if ch == 0: break
                    text += chr(ch)
                    pos += 2
                entries.append((eid, text))
            else:
                entries.append((eid, None))
    return entries

def get_placename_fmg(bnd_data):
    file_count = struct.unpack_from('<I', bnd_data, 0x0C)[0]
    for i in range(file_count):
        eoff = 0x40 + i * 0x24
        comp_size = struct.unpack_from('<Q', bnd_data, eoff + 0x08)[0]
        data_off = struct.unpack_from('<I', bnd_data, eoff + 0x18)[0]
        name_off = struct.unpack_from('<I', bnd_data, eoff + 0x20)[0]
        name = ''
        pos = name_off
        while pos + 1 < len(bnd_data):
            ch = struct.unpack_from('<H', bnd_data, pos)[0]
            if ch == 0: break
            name += chr(ch) if ch < 0x10000 else '?'
            pos += 2
        if 'PlaceName' in name and 'dlc' not in name.split('\\')[-1].lower():
            return bnd_data[data_off:data_off + comp_size]
    return None

import sys

if len(sys.argv) != 3:
    print("Usage: compare_fmg_detail.py <file_a.dcx> <file_b.dcx>")
    print("Compares PlaceName FMG entries between two DCX-compressed msgbnd files.")
    sys.exit(1)

path_a, path_b = sys.argv[1], sys.argv[2]
label_a, label_b = os.path.basename(path_a), os.path.basename(path_b)

print("Loading and decompressing...")
bnd_a = dcx_decompress(open(path_a, 'rb').read())
bnd_b = dcx_decompress(open(path_b, 'rb').read())

fmg_a = get_placename_fmg(bnd_a)
fmg_b = get_placename_fmg(bnd_b)

if fmg_a is None or fmg_b is None:
    print(f"ERROR: Could not find PlaceName.fmg (A={fmg_a is not None}, B={fmg_b is not None})")
    exit(1)

print(f"PlaceName.fmg sizes: {label_a}={len(fmg_a)}, {label_b}={len(fmg_b)}, diff={len(fmg_a)-len(fmg_b)}")

entries_a = parse_fmg_entries(fmg_a)
entries_b = parse_fmg_entries(fmg_b)

print(f"Entry counts: {label_a}={len(entries_a)}, {label_b}={len(entries_b)}")

dict_a = {e[0]: e[1] for e in entries_a}
dict_b = {e[0]: e[1] for e in entries_b}

ids_a = set(dict_a.keys())
ids_b = set(dict_b.keys())

only_a = ids_a - ids_b
only_b = ids_b - ids_a

if only_a:
    print(f"\nIDs only in {label_a} ({len(only_a)}):")
    for eid in sorted(only_a)[:20]:
        print(f"  {eid}: {repr(dict_a[eid])}")

if only_b:
    print(f"\nIDs only in {label_b} ({len(only_b)}):")
    for eid in sorted(only_b)[:20]:
        print(f"  {eid}: {repr(dict_b[eid])}")

if not only_a and not only_b:
    print("\nSame set of IDs!")

diffs = []
for eid in sorted(ids_a & ids_b):
    a = dict_a[eid]
    b = dict_b[eid]
    if a != b:
        diffs.append((eid, a, b))

print(f"\nText differences in common IDs: {len(diffs)}")
for eid, a, b in diffs[:30]:
    print(f"  ID {eid}:")
    print(f"    {label_a}: {repr(a)[:120]}")
    print(f"    {label_b}: {repr(b)[:120]}")
