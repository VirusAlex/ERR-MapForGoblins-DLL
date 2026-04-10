"""
Elden Ring Reforged — Rune Piece Tracker
Reads .err save files and extracts information about picked-up Rune Pieces
from the GEOM (WorldGeomMan) section of each character slot.

Rune Pieces use AEG099_821 model with geometry index 0x1194 (4500).
"""

import struct
import sys
import os
import zlib
from pathlib import Path
from collections import defaultdict

# Constants
SLOT_SIZE = 0x280010       # Each character slot size (includes 16-byte checksum)
SLOT_DATA_SIZE = 0x280000  # Actual slot data after checksum
HEADER_SIZE = 0x300        # BND4 header size
NUM_SLOTS = 10
RUNE_PIECE_GEOM_IDX_MIN = 0x1194  # 4500 — geometry index range for AEG099_821
RUNE_PIECE_GEOM_IDX_MAX = 0x11B0  # upper bound of the range

# WorldArea CHR magic followed by 0x21042700
CHR_PATTERN = b'CHR '
CHR_MAGIC_VAL = 0x21042700

# GEOM magic
GEOM_MAGIC = b'MOEG'


def decode_map_id(raw_bytes):
    """Convert 4-byte map ID to human-readable format like m60_33_45_00."""
    return f"m{raw_bytes[3]:02d}_{raw_bytes[2]:02d}_{raw_bytes[1]:02d}_{raw_bytes[0]:02d}"


def find_geom_offset(slot_data):
    pos = 0
    while True:
        idx = slot_data.find(GEOM_MAGIC, pos)
        if idx < 0:
            break
        if idx >= 4:  # size field precedes the MOEG magic
            geom_size = struct.unpack_from('<i', slot_data, idx - 4)[0]
            if 0 < geom_size < 0x100000:
                return idx - 4
        pos = idx + 1

    # Fallback: navigate from CHR + 0x21042700 (WorldArea) to GEOM
    pos = 0
    while True:
        idx = slot_data.find(CHR_PATTERN, pos)
        if idx < 0:
            break
        if idx + 8 <= len(slot_data):
            magic_val = struct.unpack_from('<I', slot_data, idx + 4)[0]
            if magic_val == CHR_MAGIC_VAL:
                wa_size_off = idx - 4
                wa_size = struct.unpack_from('<i', slot_data, wa_size_off)[0]
                wa_end = wa_size_off + 4 + wa_size
                return wa_end
        pos = idx + 1

    return None


def parse_geom_section(slot_data, geom_off):
    geom_size = struct.unpack_from('<i', slot_data, geom_off)[0]
    if geom_size <= 0:
        return [], geom_off + 4 + max(geom_size, 0)

    data_start = geom_off + 4
    magic = slot_data[data_start:data_start + 4]
    unk4 = struct.unpack_from('<I', slot_data, data_start + 4)[0]

    chunks = []
    off = data_start + 8
    end = data_start + geom_size

    while off < end:
        map_raw = slot_data[off:off + 4]
        entry_size = struct.unpack_from('<i', slot_data, off + 4)[0]

        if entry_size <= 0 or map_raw == b'\xff\xff\xff\xff':
            off += 8  # Skip terminator
            break

        payload = slot_data[off + 8:off + entry_size]
        map_name = decode_map_id(map_raw)

        count = struct.unpack_from('<I', payload, 0)[0] if len(payload) >= 4 else 0
        total = struct.unpack_from('<I', payload, 4)[0] if len(payload) >= 8 else 0

        entries = []
        for i in range(count):
            entry_off = 8 + i * 8
            if entry_off + 8 <= len(payload):
                entry_bytes = payload[entry_off:entry_off + 8]
                geom_type = entry_bytes[0]
                flags = entry_bytes[1]
                geom_idx = struct.unpack_from('<H', entry_bytes, 2)[0]
                instance_hash = struct.unpack_from('<I', entry_bytes, 4)[0]
                entries.append({
                    'type': geom_type,
                    'flags': flags,
                    'geom_idx': geom_idx,
                    'instance_hash': instance_hash,
                    'raw': entry_bytes.hex(),
                })

        chunks.append({
            'map': map_name,
            'map_raw': map_raw.hex(),
            'destroyed_count': count,
            'total_count': total,
            'entries': entries,
        })

        off += entry_size

    return chunks, off


def parse_save_file(filepath):
    with open(filepath, 'rb') as f:
        data = f.read()

    if data[:4] != b'BND4':
        raise ValueError(f"Not a valid BND4 save file: {filepath}")

    results = []

    for slot_idx in range(NUM_SLOTS):
        slot_start = HEADER_SIZE + slot_idx * SLOT_SIZE + 0x10  # Skip checksum
        slot_end = slot_start + SLOT_DATA_SIZE

        if slot_end > len(data):
            break

        slot_data = data[slot_start:slot_end]
        version = struct.unpack_from('<I', slot_data, 0)[0]
        if version == 0:
            results.append(None)
            continue

        geom_off = find_geom_offset(slot_data)
        if geom_off is None:
            results.append(None)
            continue

        geom_chunks, _ = parse_geom_section(slot_data, geom_off)

        # GEOF starts at geom_off + 4 + geom_size (not after last parsed chunk)
        geom_size = struct.unpack_from('<i', slot_data, geom_off)[0]
        geof_off = geom_off + 4 + geom_size
        geof_chunks = []
        if geof_off + 4 < len(slot_data):
            geof_size = struct.unpack_from('<i', slot_data, geof_off)[0]
            if geof_size > 0:
                geof_chunks, _ = parse_geom_section(slot_data, geof_off)

        # GEOF is the primary pickup source; GEOM duplicates some entries for overworld tiles
        rune_pieces_by_map = {}
        for chunk in geof_chunks:
            rp_entries = [e for e in chunk['entries']
                          if RUNE_PIECE_GEOM_IDX_MIN <= e['geom_idx'] <= RUNE_PIECE_GEOM_IDX_MAX
                          and e['flags'] == 0x00]
            if rp_entries:
                key = chunk['map']
                if key not in rune_pieces_by_map:
                    rune_pieces_by_map[key] = {
                        'map': chunk['map'],
                        'picked_up': 0,
                        'total_destroyed': chunk['destroyed_count'],
                        'total_objects': chunk['total_count'],
                        'entries': [],
                    }
                rune_pieces_by_map[key]['picked_up'] += len(rp_entries)
                rune_pieces_by_map[key]['entries'].extend(rp_entries)

        geof_maps = set(rune_pieces_by_map.keys())
        for chunk in geom_chunks:
            rp_entries = [e for e in chunk['entries']
                          if RUNE_PIECE_GEOM_IDX_MIN <= e['geom_idx'] <= RUNE_PIECE_GEOM_IDX_MAX]
            if rp_entries and chunk['map'] not in geof_maps:
                key = chunk['map']
                rune_pieces_by_map[key] = {
                    'map': chunk['map'],
                    'picked_up': len(rp_entries),
                    'total_destroyed': chunk['destroyed_count'],
                    'total_objects': chunk['total_count'],
                    'entries': rp_entries,
                }

        rune_pieces = list(rune_pieces_by_map.values())

        results.append({
            'slot': slot_idx,
            'version': version,
            'geom_chunks': geom_chunks,
            'geof_chunks': geof_chunks,
            'rune_pieces': rune_pieces,
            'total_rune_pieces_picked': sum(rp['picked_up'] for rp in rune_pieces),
        })

    return results


def decompress_dcx(filepath):
    with open(filepath, 'rb') as f:
        data = f.read()

    if data[:4] != b'DCX\x00':
        raise ValueError(f"Not a DCX file: {filepath}")

    dcp_off = struct.unpack_from('>I', data, 0x0C)[0]
    comp_type = data[dcp_off + 4:dcp_off + 8]

    if comp_type == b'DFLT':
        return zlib.decompress(data[0x4C:], 15)
    elif comp_type == b'KRAK':
        raise ValueError(f"Kraken compression not supported: {filepath}")
    else:
        raise ValueError(f"Unknown compression type {comp_type}: {filepath}")


def find_rune_pieces_in_msb(msb_data, map_name):
    search = 'AEG099_821'.encode('utf-16-le')
    pieces = []
    pos = 0

    while True:
        idx = msb_data.find(search, pos)
        if idx < 0:
            break

        name_start = idx
        while name_start > 0:
            char = struct.unpack_from('<H', msb_data, name_start - 2)[0]
            if char == 0 or char > 0x7F:
                break
            name_start -= 2

        name_end = idx + len(search)
        while name_end + 1 < len(msb_data):
            char = struct.unpack_from('<H', msb_data, name_end)[0]
            if char == 0:
                break
            name_end += 2

        name = msb_data[name_start:name_end].decode('utf-16-le', errors='replace')

        # Only count part instances (AEG099_821_XXXX), not model definitions
        if '_' in name and name.startswith('AEG099_821_') and len(name) > 12:
            entry_start = name_start - 0xC0  # name offset within MSB part entry
            if entry_start >= 0:
                try:
                    px = struct.unpack_from('<f', msb_data, entry_start + 0x20)[0]
                    py = struct.unpack_from('<f', msb_data, entry_start + 0x24)[0]
                    pz = struct.unpack_from('<f', msb_data, entry_start + 0x28)[0]
                    pieces.append({
                        'name': name,
                        'map': map_name,
                        'position': (px, py, pz),
                    })
                except:
                    pieces.append({
                        'name': name,
                        'map': map_name,
                        'position': None,
                    })
            else:
                pieces.append({
                    'name': name,
                    'map': map_name,
                    'position': None,
                })

        pos = idx + len(search)

    return pieces


def scan_mod_for_rune_pieces(mod_path):
    msb_dir = Path(mod_path) / "mod" / "map" / "MapStudio"
    if not msb_dir.exists():
        print(f"MSB directory not found: {msb_dir}")
        return {}

    all_pieces = {}
    msb_files = sorted(msb_dir.glob("*.msb.dcx"))

    for msb_file in msb_files:
        map_name = msb_file.stem.replace('.msb', '')
        try:
            msb_data = decompress_dcx(str(msb_file))
            pieces = find_rune_pieces_in_msb(msb_data, map_name)
            if pieces:
                all_pieces[map_name] = pieces
        except Exception as e:
            pass  # Skip files that can't be decompressed (Kraken, etc.)

    return all_pieces


def main():
    import argparse

    parser = argparse.ArgumentParser(description="Elden Ring Reforged — Rune Piece Tracker")
    parser.add_argument("save_file", help="Path to .err or .sl2 save file")
    parser.add_argument("--slot", type=int, default=None, help="Character slot index (0-9), default: first active")
    parser.add_argument("--mod", type=str, default=None, help="Path to ERR mod root directory (for total counts)")
    parser.add_argument("--verbose", "-v", action="store_true", help="Show detailed per-entry information")
    parser.add_argument("--all-geom", action="store_true", help="Show all GEOM data, not just Rune Pieces")

    args = parser.parse_args()

    print(f"Reading save file: {args.save_file}")
    results = parse_save_file(args.save_file)

    active_slots = [(i, r) for i, r in enumerate(results) if r is not None]

    if not active_slots:
        print("No active character slots found!")
        return

    if args.slot is not None:
        if args.slot >= len(results) or results[args.slot] is None:
            print(f"Slot {args.slot} is not active. Active slots: {[i for i, _ in active_slots]}")
            return
        display_slots = [(args.slot, results[args.slot])]
    else:
        display_slots = active_slots

    mod_pieces = {}
    if args.mod:
        print(f"\nScanning mod for Rune Piece placements: {args.mod}")
        mod_pieces = scan_mod_for_rune_pieces(args.mod)
        total_in_mod = sum(len(v) for v in mod_pieces.values())
        print(f"Found {total_in_mod} Rune Pieces across {len(mod_pieces)} map tiles")

    for slot_idx, slot_data in display_slots:
        print(f"\n{'='*60}")
        print(f"Character Slot {slot_idx} (save version {slot_data['version']})")
        print(f"{'='*60}")

        total_picked = slot_data['total_rune_pieces_picked']
        print(f"\nAEG099_821 objects destroyed: {total_picked}")
        print(f"  (includes picked up + destroyed without pickup)")
        print(f"  Actual pickups = Runic Trace count in inventory")

        if mod_pieces:
            total_in_mod = sum(len(v) for v in mod_pieces.values())
            print(f"Total Rune Pieces in mod: {total_in_mod}")
            pct = (total_picked / total_in_mod * 100) if total_in_mod > 0 else 0
            print(f"Upper bound completion: {pct:.1f}%")

        if slot_data['rune_pieces']:
            print(f"\nPer-tile breakdown:")
            for rp in sorted(slot_data['rune_pieces'], key=lambda x: x['map']):
                tile_total = ""
                if mod_pieces:
                    # Find matching map tile
                    for mod_map, pieces in mod_pieces.items():
                        if mod_map == rp['map']:
                            tile_total = f" / {len(pieces)} total"
                            break
                print(f"  {rp['map']}: {rp['picked_up']} picked up{tile_total} "
                      f"(geom: {rp['total_destroyed']}/{rp['total_objects']} destroyed)")

                if args.verbose:
                    for e in rp['entries']:
                        print(f"    Entry: type={e['type']} flags=0x{e['flags']:02X} "
                              f"idx=0x{e['geom_idx']:04X} hash=0x{e['instance_hash']:08X}")

        if args.all_geom and slot_data['geom_chunks']:
            print(f"\nAll GEOM chunks:")
            for chunk in slot_data['geom_chunks']:
                if chunk['destroyed_count'] > 0:
                    print(f"  {chunk['map']}: {chunk['destroyed_count']}/{chunk['total_count']} destroyed")
                    for e in chunk['entries']:
                        marker = " <-- RUNE PIECE" if RUNE_PIECE_GEOM_IDX_MIN <= e['geom_idx'] <= RUNE_PIECE_GEOM_IDX_MAX else ""
                        print(f"    [{e['type']:2d},0x{e['flags']:02X}] "
                              f"idx=0x{e['geom_idx']:04X} hash=0x{e['instance_hash']:08X}{marker}")

        if mod_pieces:
            picked_maps = {rp['map'] for rp in slot_data['rune_pieces']}
            untouched = {m: p for m, p in mod_pieces.items() if m not in picked_maps}
            if untouched and args.verbose:
                print(f"\nMap tiles with unpicked Rune Pieces ({len(untouched)} tiles):")
                for map_name in sorted(untouched):
                    print(f"  {map_name}: {len(untouched[map_name])} Rune Pieces")


if __name__ == "__main__":
    main()
