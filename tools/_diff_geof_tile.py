"""
Compare GEOF data across multiple save files to determine piece→GEOF mapping.
Use --tile to specify target tile (default: m60_33_45_00).
"""

import struct, os
import config

GEOM_IDX_MIN = 0x1194
GEOM_IDX_MAX = 0x11A6
TARGET_TILE = (60 << 24) | (33 << 16) | (45 << 8) | 0  # m60_33_45_00

def tile_str(tid):
    return f"m{(tid>>24)&0xFF:02d}_{(tid>>16)&0xFF:02d}_{(tid>>8)&0xFF:02d}_{tid&0xFF:02d}"

def parse_geof_sections(data):
    magic = b'FOEG'
    sections = []
    pos = 4
    while pos + 4 < len(data):
        idx = data.find(magic, pos)
        if idx < 4:
            break
        total_size = struct.unpack_from('<i', data, idx - 4)[0]
        if total_size <= 12 or total_size > 0x100000:
            pos = idx + 4
            continue

        section_start = idx - 4
        chunk_pos = section_start + 12
        section_end = section_start + total_size

        entries = []
        while chunk_pos + 16 <= section_end and chunk_pos + 16 <= len(data):
            if data[chunk_pos:chunk_pos+4] == b'\xff\xff\xff\xff':
                break
            entry_size = struct.unpack_from('<i', data, chunk_pos + 4)[0]
            if entry_size <= 0 or entry_size > 0x100000:
                break
            tile_id = struct.unpack_from('<I', data, chunk_pos)[0]
            count = struct.unpack_from('<I', data, chunk_pos + 8)[0]

            for ei in range(count):
                eoff = chunk_pos + 16 + ei * 8
                if eoff + 8 > len(data):
                    break
                raw = data[eoff:eoff+8]
                etype, flags, geom_idx = raw[0], raw[1], struct.unpack_from('<H', raw, 2)[0]
                ihash = struct.unpack_from('<I', raw, 4)[0]
                entries.append({
                    'tile_id': tile_id,
                    'type': etype,
                    'flags': flags,
                    'geom_idx': geom_idx,
                    'instance_hash': ihash,
                    'raw': raw.hex(),
                })
            chunk_pos += entry_size
        sections.append({'offset': section_start, 'entries': entries})
        pos = idx + 4
    return sections

def get_tile_entries(sections, section_idx, tile_id):
    if section_idx >= len(sections):
        return []
    return [e for e in sections[section_idx]['entries'] if e['tile_id'] == tile_id]

def get_rune_entries(sections, section_idx, tile_id=None):
    if section_idx >= len(sections):
        return []
    result = []
    for e in sections[section_idx]['entries']:
        if tile_id and e['tile_id'] != tile_id:
            continue
        if e['geom_idx'] >= GEOM_IDX_MIN and e['geom_idx'] <= GEOM_IDX_MAX and e['flags'] in (0x00, 0x80):
            result.append(e)
    return result

def print_entries(label, entries):
    print(f"\n  {label}:")
    if not entries:
        print(f"    (none)")
        return
    for e in entries:
        print(f"    tile={tile_str(e['tile_id'])}  type=0x{e['type']:02X}  flags=0x{e['flags']:02X}  "
              f"geom_idx=0x{e['geom_idx']:04X}  hash=0x{e['instance_hash']:08X}  raw={e['raw']}")

def tile_id_from_str(s):
    """Parse tile name like 'm60_33_45_00' into integer ID."""
    parts = s.replace('m', '').split('_')
    aa, bb, cc, dd = int(parts[0]), int(parts[1]), int(parts[2]), int(parts[3])
    return (aa << 24) | (bb << 16) | (cc << 8) | dd

def main():
    import sys, glob, argparse

    parser = argparse.ArgumentParser(
        description="Compare GEOF sections across multiple saves. First file is treated as baseline.")
    parser.add_argument('paths', nargs='*', help='Save files (.err) or directories containing them')
    parser.add_argument('--tile', default='m60_33_45_00',
                        help='Target tile name, e.g. m60_33_45_00 (default: m60_33_45_00)')
    args = parser.parse_args()

    global TARGET_TILE
    TARGET_TILE = tile_id_from_str(args.tile)

    if not args.paths:
        parser.print_help()
        sys.exit(1)

    # Collect files
    paths = []
    for arg in args.paths:
        if os.path.isdir(arg):
            paths.extend(sorted(glob.glob(os.path.join(arg, '*.err'))))
        elif os.path.isfile(arg):
            paths.append(arg)
    if len(paths) < 2:
        print(f"Need at least 2 .err files, got {len(paths)}")
        sys.exit(1)

    # Build keys from filenames
    saves = {}
    keys_ordered = []
    for path in paths:
        key = os.path.splitext(os.path.basename(path))[0]
        keys_ordered.append(key)
        with open(path, 'rb') as f:
            data = f.read()
        sections = parse_geof_sections(data)
        saves[key] = sections
        rune_count_s2 = len(get_rune_entries(sections, 2))
        print(f"{key}: {len(data)} bytes, {len(sections)} sections, slot2 rune pieces: {rune_count_s2}")

    print(f"\n{'='*80}")
    print(f"ANALYSIS: tile {tile_str(TARGET_TILE)}")
    print(f"{'='*80}")

    # Show ALL entries (not just rune) for the target tile
    for key in keys_ordered:
        all_tile = get_tile_entries(saves[key], 2, TARGET_TILE)
        rune_tile = get_rune_entries(saves[key], 2, TARGET_TILE)
        print(f"\n--- {key} ---")
        print(f"  All entries on tile: {len(all_tile)}")
        print(f"  Rune piece entries: {len(rune_tile)}")
        if all_tile:
            for e in all_tile:
                is_rune = "RUNE" if (e['geom_idx'] >= GEOM_IDX_MIN and e['geom_idx'] <= GEOM_IDX_MAX and e['flags'] in (0x00, 0x80)) else "    "
                print(f"    {is_rune}  type=0x{e['type']:02X}  flags=0x{e['flags']:02X}  "
                      f"geom_idx=0x{e['geom_idx']:04X}  hash=0x{e['instance_hash']:08X}  raw={e['raw']}")

    # Diff: what changed?
    print(f"\n{'='*80}")
    print(f"DIFFS")
    print(f"{'='*80}")

    baseline = keys_ordered[0]
    before_rune = set()
    for e in get_rune_entries(saves[baseline], 2, TARGET_TILE):
        before_rune.add((e['geom_idx'], e['flags'], e['instance_hash']))

    for key in keys_ordered[1:]:
        current = set()
        entries = {}
        for e in get_rune_entries(saves[key], 2, TARGET_TILE):
            k = (e['geom_idx'], e['flags'], e['instance_hash'])
            current.add(k)
            entries[k] = e

        new_entries = current - before_rune
        print(f"\n  {key} vs before: +{len(new_entries)} new GEOF entries")
        for geom, flags, ihash in sorted(new_entries):
            e = entries[(geom, flags, ihash)]
            pidx = (geom - GEOM_IDX_MIN) * 2 + (1 if flags == 0x80 else 0)
            print(f"    NEW: geom_idx=0x{geom:04X}  flags=0x{flags:02X}  hash=0x{ihash:08X}  piece_idx={pidx}  raw={e['raw']}")

    # Also show total rune piece count changes across all tiles
    print(f"\n{'='*80}")
    print(f"TOTAL RUNE PIECE COUNT (slot 2, all tiles)")
    print(f"{'='*80}")
    for key in keys_ordered:
        total = len(get_rune_entries(saves[key], 2))
        print(f"  {key}: {total} rune pieces")

    # Show new entries across ALL tiles (not just target)
    print(f"\n{'='*80}")
    print(f"ALL NEW RUNE ENTRIES (across all tiles)")
    print(f"{'='*80}")

    before_all = set()
    for e in get_rune_entries(saves[keys_ordered[0]], 2):
        before_all.add((e['tile_id'], e['geom_idx'], e['flags'], e['instance_hash']))

    for key in keys_ordered[1:]:
        current_all = {}
        for e in get_rune_entries(saves[key], 2):
            k = (e['tile_id'], e['geom_idx'], e['flags'], e['instance_hash'])
            current_all[k] = e

        new = set(current_all.keys()) - before_all
        print(f"\n  {key}: +{len(new)} new entries")
        for tile, geom, flags, ihash in sorted(new):
            e = current_all[(tile, geom, flags, ihash)]
            pidx = (geom - GEOM_IDX_MIN) * 2 + (1 if flags == 0x80 else 0)
            print(f"    {tile_str(tile)}  geom_idx=0x{geom:04X}  flags=0x{flags:02X}  hash=0x{ihash:08X}  piece_idx={pidx}  raw={e['raw']}")

if __name__ == '__main__':
    main()
