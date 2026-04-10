"""
Find tiles where some but not all Rune Pieces are collected.
Cross-references GEOF data from save file with rune_pieces.json.
"""

import json, struct, os, glob
import config

GEOM_IDX_MIN = 0x1194
GEOM_IDX_MAX = 0x11A6

def find_save_file():
    appdata = os.environ.get("APPDATA", "")
    er_dir = os.path.join(appdata, "EldenRing")
    if not os.path.isdir(er_dir):
        return None
    for d in os.listdir(er_dir):
        full = os.path.join(er_dir, d)
        if not os.path.isdir(full):
            continue
        for name in ["ER0000.err", "ER0000.sl2"]:
            path = os.path.join(full, name)
            if os.path.isfile(path):
                return path
    return None

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
                if geom_idx >= GEOM_IDX_MIN and geom_idx <= GEOM_IDX_MAX and flags in (0x00, 0x80):
                    entries.append({
                        'tile_id': tile_id,
                        'type': etype,
                        'flags': flags,
                        'geom_idx': geom_idx,
                        'instance_hash': ihash,
                    })
            chunk_pos += entry_size

        sections.append({
            'offset': section_start,
            'piece_count': len(entries),
            'entries': entries,
        })
        pos = idx + 4

    return sections

def tile_str(tid):
    aa = (tid >> 24) & 0xFF
    bb = (tid >> 16) & 0xFF
    cc = (tid >> 8) & 0xFF
    dd = tid & 0xFF
    return f"m{aa:02d}_{bb:02d}_{cc:02d}_{dd:02d}"

def tile_id_from_str(s):
    parts = s.replace('m','').split('_')
    aa, bb, cc, dd = int(parts[0]), int(parts[1]), int(parts[2]), int(parts[3])
    return (aa << 24) | (bb << 16) | (cc << 8) | dd

def main():
    pieces_path = str(config.DATA_DIR / 'rune_pieces.json')

    with open(pieces_path, 'r') as f:
        pieces_list = json.load(f)

    # Build tile -> pieces map
    tile_pieces = {}
    for p in pieces_list:
        tid = tile_id_from_str(p['map'])
        if tid not in tile_pieces:
            tile_pieces[tid] = []
        tile_pieces[tid].append(p)

    print(f"Database: {len(pieces_list)} pieces across {len(tile_pieces)} tiles")
    print()

    # Find save file
    save_path = find_save_file()
    if not save_path:
        print("ERROR: Save file not found in APPDATA/EldenRing/")
        return
    print(f"Save file: {save_path}")

    with open(save_path, 'rb') as f:
        data = f.read()
    print(f"Save size: {len(data)} bytes")

    sections = parse_geof_sections(data)
    print(f"GEOF sections found: {len(sections)}")
    for i, sec in enumerate(sections):
        print(f"  [{i}] offset=0x{sec['offset']:08X}, pieces={sec['piece_count']}")

    if not sections:
        print("No GEOF data found!")
        return

    # Use section from command line, or largest by default
    import sys
    if len(sys.argv) > 1 and sys.argv[1].isdigit():
        best = int(sys.argv[1])
        if best >= len(sections):
            print(f"ERROR: Section [{best}] does not exist")
            return
    else:
        best = max(range(len(sections)), key=lambda i: sections[i]['piece_count'])
    geof = sections[best]['entries']
    print(f"\nUsing section [{best}] with {len(geof)} entries")

    # Count GEOF per tile
    geof_per_tile = {}
    geof_entries_per_tile = {}
    for e in geof:
        tid = e['tile_id']
        geof_per_tile[tid] = geof_per_tile.get(tid, 0) + 1
        if tid not in geof_entries_per_tile:
            geof_entries_per_tile[tid] = []
        geof_entries_per_tile[tid].append(e)

    # Find partial tiles
    print("\n" + "="*80)
    print("PARTIAL TILES (some collected, some not)")
    print("="*80)

    partial_tiles = []
    full_tiles = []
    zero_tiles = []

    for tid, pieces in sorted(tile_pieces.items()):
        n_pieces = len(pieces)
        n_geof = geof_per_tile.get(tid, 0)

        if n_pieces <= 1:
            continue  # single-piece tiles are always exact match

        if n_geof == 0:
            zero_tiles.append((tid, n_pieces))
        elif n_geof >= n_pieces:
            full_tiles.append((tid, n_pieces, n_geof))
        else:
            partial_tiles.append((tid, n_pieces, n_geof))

    print(f"\nMulti-piece tiles summary:")
    print(f"  Total multi-piece tiles: {len(partial_tiles) + len(full_tiles) + len(zero_tiles)}")
    print(f"  Fully collected: {len(full_tiles)}")
    print(f"  Partially collected: {len(partial_tiles)}")
    print(f"  Not touched: {len(zero_tiles)}")

    if partial_tiles:
        print(f"\n--- Partial tiles (best for testing) ---\n")
        for tid, n_pieces, n_geof in sorted(partial_tiles, key=lambda x: x[1]):
            ts = tile_str(tid)
            remaining = n_pieces - n_geof
            print(f"  {ts}: {n_geof}/{n_pieces} collected ({remaining} remaining)")

            # Show GEOF entries with piece_index
            for e in geof_entries_per_tile.get(tid, []):
                pidx = (e['geom_idx'] - GEOM_IDX_MIN) * 2 + (1 if e['flags'] == 0x80 else 0)
                print(f"    GEOF: geom_idx=0x{e['geom_idx']:04X}, flags=0x{e['flags']:02X}, hash=0x{e['instance_hash']:08X}, piece_idx={pidx}")

            # Show all pieces on this tile
            for p in tile_pieces[tid]:
                print(f"    Piece: {p['name']}  pos=({p['x']:.1f}, {p['y']:.1f}, {p['z']:.1f})")
            print()

    # Also show some stats about single-piece tiles
    single_tiles = [(tid, ps) for tid, ps in tile_pieces.items() if len(ps) == 1]
    single_collected = sum(1 for tid, _ in single_tiles if geof_per_tile.get(tid, 0) >= 1)
    print(f"\nSingle-piece tiles: {single_collected}/{len(single_tiles)} collected (exact match)")

    # Total coverage
    total_hidden = single_collected
    for tid, n_pieces, n_geof in full_tiles:
        total_hidden += n_pieces
    total_unknown = sum(n_geof for _, _, n_geof in partial_tiles)
    print(f"\nTotal pieces we can confidently hide: {total_hidden}")
    print(f"Pieces on partial tiles (ambiguous): {total_unknown} GEOF entries across {len(partial_tiles)} tiles")

if __name__ == '__main__':
    main()
