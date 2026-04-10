"""Count collected Rune Pieces from save file using unique (tile, instance_hash) pairs."""
import struct
import sys
import os
import config

GEOM_IDX_MIN = 0x1194
GEOM_IDX_MAX = 0x11A6


def find_sections(data, magic):
    results = []
    pos = 0
    while True:
        idx = data.find(magic, pos)
        if idx == -1:
            break
        if idx >= 4:
            total_size = struct.unpack_from('<i', data, idx - 4)[0]
            unk4 = struct.unpack_from('<I', data, idx + 4)[0]
            results.append((idx - 4, total_size, unk4))
        pos = idx + 4
    return results


def parse_section(data, section_offset, total_size):
    pos = section_offset + 12
    chunks = []
    while pos + 16 <= section_offset + total_size:
        map_id = data[pos:pos+4]
        if map_id == b'\xff\xff\xff\xff':
            break
        entry_size = struct.unpack_from('<i', data, pos + 4)[0]
        if entry_size <= 0 or entry_size > 0x100000:
            break
        count = struct.unpack_from('<I', data, pos + 8)[0]
        total = struct.unpack_from('<I', data, pos + 12)[0]

        entries = []
        for ei in range(count):
            eoff = pos + 16 + ei * 8
            if eoff + 8 > len(data):
                break
            etype = data[eoff]
            eflags = data[eoff + 1]
            geom_idx = struct.unpack_from('<H', data, eoff + 2)[0]
            inst_hash = struct.unpack_from('<I', data, eoff + 4)[0]
            entries.append((etype, eflags, geom_idx, inst_hash))

        chunks.append((map_id.hex(), count, total, entries))
        pos += entry_size
    return chunks


def decode_tile(hex_str):
    """Decode tile hex to human-readable map ID."""
    b = bytes.fromhex(hex_str)
    if b[3] == 0x3c:  # m60 open world
        return f"m60_{b[2]:02d}_{b[1]:02d}_{b[0]:02d}"
    # Dungeon tiles - encode differently
    val = struct.unpack('<I', b)[0]
    return f"dungeon_0x{val:08X}"


def analyze_save(path, label):
    print(f"\n{'='*70}")
    print(f"  {label}: {os.path.basename(path)}")
    print(f"{'='*70}")

    with open(path, 'rb') as f:
        data = f.read()

    geof_sections = find_sections(data, b"FOEG")

    for si, (offset, size, unk) in enumerate(geof_sections):
        chunks = parse_section(data, offset, size)

        # Method 1: raw entry count
        raw_count = 0
        # Method 2: unique (tile, hash) pairs
        unique_pieces = set()
        # Method 3: unique (tile, hash) but only flags=0x00
        unique_primary = set()

        # Details for breakdown
        tile_details = {}

        for tile_hex, count, total, entries in chunks:
            for etype, eflags, geom_idx, inst_hash in entries:
                if GEOM_IDX_MIN <= geom_idx <= GEOM_IDX_MAX:
                    if eflags == 0x00:
                        raw_count += 1
                        unique_primary.add((tile_hex, inst_hash))
                        key = (tile_hex, inst_hash)
                        if key not in tile_details:
                            tile_details[key] = {'shapes': [], 'tile': tile_hex, 'hash': inst_hash}
                        tile_details[key]['shapes'].append(geom_idx)
                    unique_pieces.add((tile_hex, inst_hash, eflags))

        print(f"\n  GEOF section [{si}]:")
        print(f"    Raw GEOF entries (flags=0x00, geom in range):  {raw_count}")
        print(f"    Unique (tile, hash) pairs (flags=0x00):        {len(unique_primary)}")
        print(f"    Unique (tile, hash, flags) including 0x80:     {len(unique_pieces)}")

        # Show pieces with multiple shapes (dungeon pieces)
        multi_shape = {k: v for k, v in tile_details.items() if len(v['shapes']) > 1}
        single_shape = {k: v for k, v in tile_details.items() if len(v['shapes']) == 1}

        print(f"\n    Single-shape pieces (open world):  {len(single_shape)}")
        print(f"    Multi-shape pieces (dungeons):     {len(multi_shape)}")
        print(f"    Overcounted entries from multi-shape: {raw_count - len(unique_primary)}")

        if multi_shape:
            print(f"\n    Multi-shape breakdown:")
            for (tile_hex, ihash), det in sorted(multi_shape.items()):
                tile_name = decode_tile(tile_hex)
                n = len(det['shapes'])
                shapes_str = ', '.join(f'0x{s:04X}' for s in sorted(det['shapes']))
                print(f"      {tile_name} hash=0x{ihash:08X}: {n} shapes [{shapes_str}]")


if __name__ == "__main__":
    import sys
    saves = []
    if len(sys.argv) < 2:
        print("Usage: _count_pieces.py <save_file.err> [save_file2.err ...]")
        sys.exit(1)
    for path in sys.argv[1:]:
        saves.append((path, os.path.basename(path)))

    for path, label in saves:
        analyze_save(path, label)
