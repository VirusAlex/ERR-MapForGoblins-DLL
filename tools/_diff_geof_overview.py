"""Diff GEOF entries between BEFORE and AFTER save files to find newly collected pieces."""
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
    chunks = {}
    while pos + 16 <= section_offset + total_size:
        map_id = data[pos:pos+4]
        if map_id == b'\xff\xff\xff\xff':
            break
        entry_size = struct.unpack_from('<i', data, pos + 4)[0]
        if entry_size <= 0 or entry_size > 0x100000:
            break
        count = struct.unpack_from('<I', data, pos + 8)[0]
        total = struct.unpack_from('<I', data, pos + 12)[0]

        entries = set()
        for ei in range(count):
            eoff = pos + 16 + ei * 8
            if eoff + 8 > len(data):
                break
            raw = data[eoff:eoff+8]
            entries.add(raw)

        chunks[map_id.hex()] = {
            'count': count,
            'total': total,
            'entries': entries,
            'entries_raw': [data[pos+16+i*8:pos+16+i*8+8] for i in range(count)],
        }
        pos += entry_size
    return chunks


def decode_entry(raw):
    etype = raw[0]
    eflags = raw[1]
    geom_idx = struct.unpack_from('<H', raw, 2)[0]
    inst_hash = struct.unpack_from('<I', raw, 4)[0]
    return etype, eflags, geom_idx, inst_hash


def diff_sections(before_chunks, after_chunks, section_name):
    all_tiles = sorted(set(list(before_chunks.keys()) + list(after_chunks.keys())))
    added_total = []
    removed_total = []

    for tile in all_tiles:
        bc = before_chunks.get(tile, {'entries': set()})
        ac = after_chunks.get(tile, {'entries': set()})

        added = ac['entries'] - bc['entries']
        removed = bc['entries'] - ac['entries']

        for raw in sorted(added):
            t, f, gi, ih = decode_entry(raw)
            is_rune = GEOM_IDX_MIN <= gi <= GEOM_IDX_MAX
            added_total.append((tile, t, f, gi, ih, is_rune))

        for raw in sorted(removed):
            t, f, gi, ih = decode_entry(raw)
            is_rune = GEOM_IDX_MIN <= gi <= GEOM_IDX_MAX
            removed_total.append((tile, t, f, gi, ih, is_rune))

    print(f"\n--- {section_name} DIFF ---")
    if added_total:
        print(f"  ADDED entries: {len(added_total)}")
        rune_added = [e for e in added_total if e[5]]
        other_added = [e for e in added_total if not e[5]]
        if rune_added:
            print(f"    RUNE PIECES added: {len(rune_added)}")
            for tile, t, f, gi, ih, _ in rune_added:
                print(f"      tile={tile} type={t} flags=0x{f:02X} geom_idx=0x{gi:04X} hash=0x{ih:08X}")
        if other_added:
            print(f"    OTHER added: {len(other_added)}")
            for tile, t, f, gi, ih, _ in other_added[:20]:
                print(f"      tile={tile} type={t} flags=0x{f:02X} geom_idx=0x{gi:04X} hash=0x{ih:08X}")
    else:
        print("  No entries added")

    if removed_total:
        print(f"  REMOVED entries: {len(removed_total)}")
        for tile, t, f, gi, ih, is_rune in removed_total[:10]:
            label = " [RUNE]" if is_rune else ""
            print(f"      tile={tile} type={t} flags=0x{f:02X} geom_idx=0x{gi:04X} hash=0x{ih:08X}{label}")
    else:
        print("  No entries removed")


def process_pair(before_path, after_path, label):
    print(f"\n{'='*70}")
    print(f"  {label}")
    print(f"  BEFORE: {before_path}")
    print(f"  AFTER:  {after_path}")
    print(f"{'='*70}")

    with open(before_path, 'rb') as f:
        before_data = f.read()
    with open(after_path, 'rb') as f:
        after_data = f.read()

    for magic_name, magic_bytes in [("GEOF", b"FOEG"), ("GEOM", b"MOEG")]:
        b_sections = find_sections(before_data, magic_bytes)
        a_sections = find_sections(after_data, magic_bytes)

        for si in range(min(len(b_sections), len(a_sections))):
            b_off, b_sz, _ = b_sections[si]
            a_off, a_sz, _ = a_sections[si]

            b_chunks = parse_section(before_data, b_off, b_sz)
            a_chunks = parse_section(after_data, a_off, a_sz)

            b_rune = sum(1 for c in b_chunks.values()
                        for e in c['entries']
                        if GEOM_IDX_MIN <= decode_entry(e)[2] <= GEOM_IDX_MAX and decode_entry(e)[1] == 0x00)
            a_rune = sum(1 for c in a_chunks.values()
                        for e in c['entries']
                        if GEOM_IDX_MIN <= decode_entry(e)[2] <= GEOM_IDX_MAX and decode_entry(e)[1] == 0x00)

            if b_rune != a_rune or b_sz != a_sz:
                print(f"\n{magic_name} section [{si}]: rune_before={b_rune} rune_after={a_rune} (delta={a_rune-b_rune})")
                diff_sections(b_chunks, a_chunks, f"{magic_name}[{si}]")


if __name__ == "__main__":
    import glob

    if len(sys.argv) < 3:
        print("Usage: _diff_geof.py <before.err> <after.err>")
        print("       _diff_geof.py <directory>  (processes all .err files as sequential pairs)")
        print("Diffs GEOF entries between save files to find newly collected pieces.")
        if len(sys.argv) == 2 and os.path.isdir(sys.argv[1]):
            pass  # fall through to directory mode
        else:
            sys.exit(1)

    if len(sys.argv) == 2 and os.path.isdir(sys.argv[1]):
        # Directory mode: grab all .err files sorted by name, diff consecutive pairs
        err_files = sorted(glob.glob(os.path.join(sys.argv[1], '*.err')))
        if len(err_files) < 2:
            print(f"Need at least 2 .err files in {sys.argv[1]}, found {len(err_files)}")
            sys.exit(1)
        for i in range(len(err_files) - 1):
            process_pair(err_files[i], err_files[i+1],
                         f"{os.path.basename(err_files[i])} -> {os.path.basename(err_files[i+1])}")
    else:
        # Explicit file pair(s): process argv as pairs
        files = sys.argv[1:]
        if len(files) % 2 != 0:
            print("Error: provide an even number of files (before/after pairs)")
            sys.exit(1)
        for i in range(0, len(files), 2):
            process_pair(files[i], files[i+1],
                         f"{os.path.basename(files[i])} -> {os.path.basename(files[i+1])}")
