import ctypes, struct, sys, os
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

if len(sys.argv) < 3:
    print("Usage: compare_bnd.py <file_a.dcx> <file_b.dcx> [file_c.dcx ...]")
    print("Compares BND4 structure of two or more DCX-compressed files.")
    sys.exit(1)

files = []
for path in sys.argv[1:]:
    label = os.path.basename(path)
    data = dcx_decompress(open(path, 'rb').read())
    files.append((label, data))

print('BND sizes: ' + ', '.join(f'{label}={len(data)}' for label, data in files))

def parse_bnd4(data):
    file_count = struct.unpack_from('<i', data, 0x0C)[0]
    header_size = struct.unpack_from('<q', data, 0x10)[0]
    file_header_size = struct.unpack_from('<q', data, 0x20)[0]
    data_start = struct.unpack_from('<q', data, 0x28)[0]
    fmt = data[0x31]
    extended = data[0x32]
    
    info = {
        'file_count': file_count,
        'header_size': header_size,
        'file_header_size': file_header_size,
        'data_start': data_start,
        'format': fmt,
        'extended': extended,
    }
    
    entries = []
    for i in range(file_count):
        off = 0x40 + i * file_header_size
        flags = data[off]
        comp_size = struct.unpack_from('<q', data, off + 0x08)[0]
        uncomp_size = struct.unpack_from('<q', data, off + 0x10)[0] if file_header_size >= 0x24 else comp_size
        data_off = struct.unpack_from('<i', data, off + 0x18)[0]
        file_id = struct.unpack_from('<i', data, off + 0x1C)[0]
        name_off = struct.unpack_from('<i', data, off + 0x20)[0]
        
        name = ''
        if name_off > 0 and name_off < len(data):
            j = name_off
            while j + 1 < len(data):
                ch = struct.unpack_from('<H', data, j)[0]
                if ch == 0: break
                name += chr(ch) if ch < 128 else '?'
                j += 2
        
        entries.append({
            'idx': i, 'flags': flags, 'comp_size': comp_size, 'uncomp_size': uncomp_size,
            'data_off': data_off, 'id': file_id, 'name_off': name_off, 'name': name
        })
    
    return info, entries

print('\n=== BND4 HEADERS ===')
for label, data in files:
    info, entries = parse_bnd4(data)
    print(f'\n--- {label} ---')
    print(f'  Hex header[0x00-0x3F]:')
    for row in range(0, 0x40, 16):
        hex_str = ' '.join(f'{b:02X}' for b in data[row:row+16])
        print(f'    {row:04X}: {hex_str}')
    print(f'  file_count={info["file_count"]}, header_size=0x{info["header_size"]:X}')
    print(f'  file_header_size=0x{info["file_header_size"]:X}, data_start=0x{info["data_start"]:X}')
    print(f'  format=0x{info["format"]:02X}, extended=0x{info["extended"]:02X}')

print('\n=== ENTRIES ===')
parsed = [(label, data, *parse_bnd4(data)) for label, data in files]
max_entries = max(len(entries) for _, _, _, entries in parsed)

for i in range(max_entries):
    row = [(label, data, entries[i] if i < len(entries) else None) for label, data, _, entries in parsed]
    ref = next(e for _, _, e in row if e is not None)
    short_name = ref['name'].split('\\')[-1]
    print(f'\n[{i}] {short_name} (id={ref["id"]})')
    for label, _, e in row:
        if e:
            print(f'  {label:12s} flags=0x{e["flags"]:02X} comp={e["comp_size"]:>8} uncomp={e["uncomp_size"]:>8} off=0x{e["data_off"]:08X} nameoff=0x{e["name_off"]:08X}')

    if len(row) >= 2 and row[0][2] and row[1][2]:
        d0 = row[0][1][row[0][2]['data_off']:row[0][2]['data_off']+row[0][2]['comp_size']]
        d1 = row[1][1][row[1][2]['data_off']:row[1][2]['data_off']+row[1][2]['comp_size']]
        if d0 == d1:
            print(f'  Data: MATCH')
        else:
            print(f'  Data: DIFFER ({row[0][0]}_len={len(d0)}, {row[1][0]}_len={len(d1)})')
            for j in range(min(len(d0), len(d1))):
                if d0[j] != d1[j]:
                    print(f'  First diff at byte {j} (0x{j:X}): 0x{d0[j]:02X} vs 0x{d1[j]:02X}')
                    start = max(0, j-8)
                    print(f'  {row[0][0]} [{start:04X}]: {" ".join(f"{b:02X}" for b in d0[start:j+24])}')
                    print(f'  {row[1][0]} [{start:04X}]: {" ".join(f"{b:02X}" for b in d1[start:j+24])}')
                    break

print('\n=== EXTENDED DATA CHECK ===')
for label, data, info, entries in parsed:
    if info['extended'] == 4:
        hash_offset = struct.unpack_from('<q', data, 0x38)[0]
        print(f'{label}: Extended=4, hash_groups_offset=0x{hash_offset:X}')
        if hash_offset > 0 and hash_offset < len(data):
            hdata = data[hash_offset:hash_offset+64]
            print(f'  Hash data: {" ".join(f"{b:02X}" for b in hdata[:32])}')
    else:
        print(f'{label}: Extended=0x{info["extended"]:02X} (no hash groups)')
