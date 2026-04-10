"""Parse GEOF/GEOM sections from Elden Ring save files to count collected Rune Pieces."""
import struct
import sys
import os
import config

GEOM_IDX_MIN = 0x1194  # AEG099_821 collision shapes range
GEOM_IDX_MAX = 0x11A6

def find_sections(data, magic):
    """Find all occurrences of a 4-byte magic in data, return (offset_of_total_size, total_size, unk4)."""
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
    """Parse GEOF or GEOM section into list of chunk tuples."""
    pos = section_offset + 12  # skip total_size(4) + magic(4) + unk4(4)
    end = section_offset + total_size
    chunks = []
    while pos + 16 <= end:
        map_id_bytes = data[pos:pos+4]
        if map_id_bytes == b'\xff\xff\xff\xff':
            break
        entry_size = struct.unpack_from('<i', data, pos + 4)[0]
        if entry_size <= 0 or entry_size > 0x100000:
            print(f"  WARNING: bad entry_size={entry_size} at offset 0x{pos:08X}, stopping")
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

        chunks.append((map_id_bytes.hex(), count, total, entries))
        pos += entry_size
    return chunks


def count_rune_pieces(chunks):
    """Count entries matching AEG099_821 geom indices."""
    results = []
    for map_hex, count, total, entries in chunks:
        for etype, eflags, geom_idx, inst_hash in entries:
            if GEOM_IDX_MIN <= geom_idx <= GEOM_IDX_MAX and eflags == 0x00:
                results.append((map_hex, etype, geom_idx, inst_hash))
    return results


def analyze_file(path, label):
    print(f"\n{'='*60}")
    print(f"  {label}: {os.path.basename(path)}")
    print(f"{'='*60}")

    with open(path, 'rb') as f:
        data = f.read()
    print(f"File size: {len(data):,} bytes")

    for magic_name, magic_bytes in [("GEOF", b"FOEG"), ("GEOM", b"MOEG")]:
        sections = find_sections(data, magic_bytes)
        print(f"\n--- {magic_name} sections: {len(sections)} ---")
        for sec_idx, (offset, size, unk) in enumerate(sections):
            print(f"  [{sec_idx}] offset=0x{offset:08X}  size={size}  unk=0x{unk:08X}")
            chunks = parse_section(data, offset, size)
            total_entries = sum(c[1] for c in chunks)
            print(f"       chunks={len(chunks)}  total_entries={total_entries}")

            rune = count_rune_pieces(chunks)
            print(f"       Rune Piece entries: {len(rune)}")

            if magic_name == "GEOF" and chunks:
                # Show per-tile summary for tiles with rune pieces
                for map_hex, cnt, tot, entries in chunks:
                    rp = [(gi, ih) for (_, ef, gi, ih) in entries
                           if GEOM_IDX_MIN <= gi <= GEOM_IDX_MAX and ef == 0x00]
                    if rp:
                        print(f"         tile {map_hex}: {cnt} destroyed / {tot} total, rune_pieces={len(rp)}")
                        for gi, ih in rp:
                            print(f"           geom_idx=0x{gi:04X}  hash=0x{ih:08X}")


if __name__ == "__main__":
    import glob

    if len(sys.argv) < 2:
        print("Usage: _parse_geof.py <file1.err> [file2.err ...]")
        print("       _parse_geof.py <directory>  (processes all .err files in it)")
        print("Parses GEOF/GEOM sections and counts Rune Piece entries.")
        sys.exit(1)

    files = []
    for arg in sys.argv[1:]:
        if os.path.isdir(arg):
            files.extend(sorted(glob.glob(os.path.join(arg, '*.err'))))
        elif os.path.isfile(arg):
            files.append(arg)
        else:
            print(f"Warning: {arg} not found, skipping")

    for path in files:
        analyze_file(path, os.path.basename(path))
