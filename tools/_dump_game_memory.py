#!/usr/bin/env python3
"""
Elden Ring Memory Dumper — Rune Piece Research Tool
====================================================
Captures structured dump of the game's geom management memory structures.
Run while the game is loaded with a character in the world.
Requires administrator privileges.

Usage:
    python dump_game_memory.py [label]

    label: optional tag for this dump (e.g. "spawn", "walked_away", "far")

Output: dumps/dump_YYYYMMDD_HHMMSS_label/
"""

import pymem
import pymem.process
import struct
import os
import sys
import json
from datetime import datetime
import ctypes
from ctypes import wintypes

# ─── Constants ──────────────────────────────────────────────────────────

# RVAs (offsets from eldenring.exe base)
RVA_GEOM_FLAG      = 0x3D69D18   # GeomFlagSaveDataManager
RVA_GEOM_NONACTIVE = 0x3D69D98   # GeomNonActiveBlockManager
RVA_WORLD_GEOM_MAN = 0x3D69BA8   # CSWorldGeomMan

# Nearby RVAs to scan for unknown singletons
SCAN_RVA_START = 0x3D69000
SCAN_RVA_END   = 0x3D6A000

# Rune piece geom indices
GEOM_IDX_MIN = 0x1194
GEOM_IDX_MAX = 0x11A6

# ─── Windows API for VirtualQueryEx ─────────────────────────────────────

kernel32 = ctypes.windll.kernel32

class MEMORY_BASIC_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("BaseAddress", ctypes.c_void_p),
        ("AllocationBase", ctypes.c_void_p),
        ("AllocationProtect", wintypes.DWORD),
        ("RegionSize", ctypes.c_size_t),
        ("State", wintypes.DWORD),
        ("Protect", wintypes.DWORD),
        ("Type", wintypes.DWORD),
    ]

MEM_COMMIT  = 0x1000
PAGE_NOACCESS = 0x01
PAGE_GUARD  = 0x100

VirtualQueryEx = kernel32.VirtualQueryEx
VirtualQueryEx.argtypes = [
    wintypes.HANDLE, ctypes.c_void_p,
    ctypes.POINTER(MEMORY_BASIC_INFORMATION), ctypes.c_size_t
]
VirtualQueryEx.restype = ctypes.c_size_t

# ─── Helper Functions ───────────────────────────────────────────────────

def safe_read(pm, addr, size):
    """Read memory safely, return bytes or None."""
    try:
        return pm.read_bytes(addr, size)
    except:
        return None

def read_u8(pm, addr):
    d = safe_read(pm, addr, 1)
    return struct.unpack('<B', d)[0] if d else None

def read_u16(pm, addr):
    d = safe_read(pm, addr, 2)
    return struct.unpack('<H', d)[0] if d else None

def read_u32(pm, addr):
    d = safe_read(pm, addr, 4)
    return struct.unpack('<I', d)[0] if d else None

def read_u64(pm, addr):
    d = safe_read(pm, addr, 8)
    return struct.unpack('<Q', d)[0] if d else None

def read_ptr(pm, addr):
    return read_u64(pm, addr)

def is_valid_ptr(val):
    return val is not None and val > 0x10000 and val < 0x7FFFFFFFFFFF

def hex_dump(data, base_offset=0, width=16):
    """Format bytes as hex dump string."""
    lines = []
    for i in range(0, len(data), width):
        chunk = data[i:i+width]
        hex_str = ' '.join(f'{b:02X}' for b in chunk)
        ascii_str = ''.join(chr(b) if 32 <= b < 127 else '.' for b in chunk)
        lines.append(f'  +{base_offset+i:04X}: {hex_str:<{width*3}}  {ascii_str}')
    return '\n'.join(lines)

def decode_tile_id(tile_id):
    area  = (tile_id >> 24) & 0xFF
    gridX = (tile_id >> 16) & 0xFF
    gridZ = (tile_id >> 8) & 0xFF
    index = tile_id & 0xFF
    return area, gridX, gridZ, index

def tile_name(tile_id):
    a, gx, gz, idx = decode_tile_id(tile_id)
    return f"m{a:02d}_{gx:02d}_{gz:02d}_{idx:02d}"

def is_rune_geom(geom_idx, flags):
    return (GEOM_IDX_MIN <= geom_idx <= GEOM_IDX_MAX and
            flags in (0x00, 0x80))

def piece_index_from_geof(geom_idx, flags):
    return (geom_idx - GEOM_IDX_MIN) * 2 + (1 if (flags & 0x80) else 0)


# ─── Main Dumper Class ──────────────────────────────────────────────────

class GameDumper:
    def __init__(self, label=""):
        print("Attaching to eldenring.exe...")
        self.pm = pymem.Pymem("eldenring.exe")
        self.base = self.pm.base_address
        self.handle = self.pm.process_handle

        ts = datetime.now().strftime("%Y%m%d_%H%M%S")
        dirname = f"dump_{ts}"
        if label:
            dirname += f"_{label}"
        self.dump_dir = os.path.join("dumps", dirname)
        os.makedirs(self.dump_dir, exist_ok=True)
        os.makedirs(os.path.join(self.dump_dir, "blocks"), exist_ok=True)
        os.makedirs(os.path.join(self.dump_dir, "geom_ins"), exist_ok=True)

        self.report = open(os.path.join(self.dump_dir, "report.txt"), "w", encoding="utf-8")
        self.rune_tile_ids = set()  # filled from rune_pieces.json
        self.tile_piece_counts = {}  # filled from rune_pieces.json

    def log(self, msg=""):
        self.report.write(msg + "\n")
        print(msg)

    def log_section(self, title):
        self.log(f"\n{'='*70}")
        self.log(f"  {title}")
        self.log(f"{'='*70}")

    def save_bin(self, path, data):
        """Save raw binary data to file."""
        full = os.path.join(self.dump_dir, path)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "wb") as f:
            f.write(data)

    def load_rune_pieces(self):
        """Load rune_pieces.json to get known tile IDs."""
        script_dir = os.path.dirname(os.path.abspath(__file__))
        json_path = os.path.join(script_dir, "MapForGoblins", "data", "rune_pieces.json")
        if not os.path.exists(json_path):
            self.log(f"[WARN] rune_pieces.json not found at {json_path}")
            return
        with open(json_path) as f:
            pieces = json.load(f)

        tile_counts = {}
        for p in pieces:
            m = p["map"]
            parts = m.split("_")
            if len(parts) != 4:
                continue
            area  = int(parts[0][1:])
            gridX = int(parts[1])
            gridZ = int(parts[2])
            index = int(parts[3])
            tid = (area << 24) | (gridX << 16) | (gridZ << 8) | index
            self.rune_tile_ids.add(tid)
            tile_counts[tid] = tile_counts.get(tid, 0) + 1

        self.log(f"Loaded {len(pieces)} pieces across {len(self.rune_tile_ids)} tiles")
        self.tile_piece_counts = tile_counts

    # ─── Section 1: Global Singleton Region ─────────────────────────────

    def dump_global_region(self):
        self.log_section("SECTION 1: Global Singleton Pointer Region")

        # Read the region around known RVAs
        region_start = self.base + SCAN_RVA_START
        region_size = SCAN_RVA_END - SCAN_RVA_START
        data = safe_read(self.pm, region_start, region_size)
        if not data:
            self.log("[ERROR] Cannot read global region")
            return

        self.save_bin("global_region.bin", data)
        self.log(f"Base address: 0x{self.base:016X}")
        self.log(f"Global region: 0x{region_start:016X} - 0x{region_start+region_size:016X}")

        # Show known RVAs
        known = {
            RVA_GEOM_FLAG: "GeomFlagSaveDataManager",
            RVA_GEOM_NONACTIVE: "GeomNonActiveBlockManager",
            RVA_WORLD_GEOM_MAN: "CSWorldGeomMan",
        }

        self.log("\nKnown singletons:")
        for rva, name in sorted(known.items()):
            ptr = read_ptr(self.pm, self.base + rva)
            if ptr is not None:
                self.log(f"  base+0x{rva:X} ({name}): 0x{ptr:016X}")
            else:
                self.log(f"  base+0x{rva:X} ({name}): NULL")

        # Scan for unknown pointers in the region
        self.log("\nScanning for valid heap pointers in global region...")
        found_ptrs = []
        for off in range(0, region_size, 8):
            rva = SCAN_RVA_START + off
            val = struct.unpack('<Q', data[off:off+8])[0]
            if is_valid_ptr(val) and (val & 0x7) == 0:
                # Check if this is a known RVA
                is_known = rva in known
                # Try to read 8 bytes at the target to verify it's a valid pointer
                probe = safe_read(self.pm, val, 8)
                if probe:
                    found_ptrs.append((rva, val, is_known))

        self.log(f"Found {len(found_ptrs)} valid pointers ({sum(1 for _,_,k in found_ptrs if k)} known)")

        # Show all found pointers with first 32 bytes at target
        for rva, ptr, is_known in found_ptrs:
            marker = " [KNOWN]" if is_known else ""
            name = known.get(rva, "")
            if name:
                name = f" ({name})"
            self.log(f"\n  base+0x{rva:X}{name}{marker} -> 0x{ptr:016X}")
            target_data = safe_read(self.pm, ptr, 64)
            if target_data:
                self.log(hex_dump(target_data, 0, 16))

    # ─── Section 2: GEOF Singleton Dump ─────────────────────────────────

    def dump_geof_singleton(self, rva, name):
        self.log_section(f"SECTION 2: {name} (base+0x{rva:X})")

        ptr = read_ptr(self.pm, self.base + rva)
        if not is_valid_ptr(ptr):
            self.log(f"[ERROR] {name} pointer is NULL or invalid: {ptr}")
            return {}

        self.log(f"Singleton ptr: 0x{ptr:016X}")

        # Dump the singleton object header (first 0x100 bytes)
        header = safe_read(self.pm, ptr, 0x100)
        if header:
            self.log(f"\nSingleton header (0x100 bytes):")
            self.log(hex_dump(header, 0, 16))
            self.save_bin(f"{name}_header.bin", header)

        # Read the table: entries at ptr+0x08, stepping by 16 bytes
        # Each entry: [tile_id:u64] [data_ptr:u64]
        tiles_data = {}  # tile_id -> list of GEOF entries
        all_entries = []
        tiles_found = 0
        consecutive_empty = 0

        raw_table = safe_read(self.pm, ptr + 0x08, 0x40000)  # 256KB of table data
        if raw_table:
            self.save_bin(f"{name}_table.bin", raw_table)
        else:
            self.log("[ERROR] Cannot read GEOF table")
            return {}

        # Parse from buffer (much faster than per-entry ReadProcessMemory)
        table_len = len(raw_table)
        for buf_off in range(0, table_len - 16 + 1, 16):
            id_val = struct.unpack('<Q', raw_table[buf_off:buf_off+8])[0]
            ptr_val = struct.unpack('<Q', raw_table[buf_off+8:buf_off+16])[0]

            if id_val == 0 and ptr_val == 0:
                consecutive_empty += 1
                if consecutive_empty > 256:
                    break
                continue
            consecutive_empty = 0

            tile_id = id_val & 0xFFFFFFFF
            area = (tile_id >> 24) & 0xFF
            if area < 0x0A or area > 0x3D:
                continue
            if not is_valid_ptr(ptr_val):
                continue

            tiles_found += 1
            has_rune = tile_id in self.rune_tile_ids

            # Read the data block to get entries
            data_header = safe_read(self.pm, ptr_val, 32)
            if not data_header:
                continue

            # Try layout A: count at +8, entries at +16
            countA = struct.unpack('<I', data_header[8:12])[0]
            # Try layout B: count at +0, entries at +8
            countB = struct.unpack('<I', data_header[0:4])[0]

            count = 0
            entries_start = 0
            if 0 < countA < 100000:
                count = countA
                entries_start = ptr_val + 16
            elif 0 < countB < 100000:
                count = countB
                entries_start = ptr_val + 8

            rune_entries = []
            if count > 0:
                entry_data = safe_read(self.pm, entries_start, min(count * 8, 0x100000))
                if entry_data:
                    for ei in range(count):
                        eoff = ei * 8
                        if eoff + 8 > len(entry_data):
                            break
                        entry_flags = entry_data[eoff + 1]
                        geom_idx = entry_data[eoff + 2] | (entry_data[eoff + 3] << 8)

                        if is_rune_geom(geom_idx, entry_flags):
                            pidx = piece_index_from_geof(geom_idx, entry_flags)
                            rune_entries.append({
                                'flags': entry_flags,
                                'geom_idx': geom_idx,
                                'piece_idx': pidx,
                                'raw': entry_data[eoff:eoff+8].hex()
                            })

            if has_rune or rune_entries:
                tiles_data[tile_id] = {
                    'count': count,
                    'rune_entries': rune_entries,
                    'data_ptr': ptr_val,
                    'has_rune_pieces': has_rune,
                    'expected_pieces': self.tile_piece_counts.get(tile_id, 0)
                }

        # Report
        rune_tiles = {tid: d for tid, d in tiles_data.items() if d['rune_entries']}
        self.log(f"\nTotal tiles in table: {tiles_found}")
        self.log(f"Tiles with rune piece GEOF entries: {len(rune_tiles)}")
        total_entries = sum(len(d['rune_entries']) for d in rune_tiles.values())
        self.log(f"Total rune piece entries: {total_entries}")

        # List all tiles with rune entries
        self.log(f"\nTiles with rune piece entries:")
        for tid in sorted(rune_tiles.keys()):
            d = rune_tiles[tid]
            tname = tile_name(tid)
            self.log(f"  {tname} (0x{tid:08X}): {len(d['rune_entries'])} entries "
                     f"(expected {d['expected_pieces']} pieces)")
            for e in d['rune_entries']:
                self.log(f"    geom=0x{e['geom_idx']:04X} flags=0x{e['flags']:02X} "
                         f"slot={e['piece_idx']} raw={e['raw']}")

        # List rune-piece tiles NOT in GEOF
        missing = self.rune_tile_ids - set(tiles_data.keys())
        if missing:
            self.log(f"\nRune piece tiles NOT in {name}: {len(missing)}")
            for tid in sorted(missing):
                self.log(f"  {tile_name(tid)} (0x{tid:08X}) - {self.tile_piece_counts.get(tid, '?')} pieces")

        return tiles_data

    # ─── Section 3: CSWorldGeomMan (WGM) Dump ───────────────────────────

    def dump_wgm(self):
        self.log_section("SECTION 3: CSWorldGeomMan (WGM) — Loaded Tiles")

        wgm_ptr = read_ptr(self.pm, self.base + RVA_WORLD_GEOM_MAN)
        if not is_valid_ptr(wgm_ptr):
            self.log("[ERROR] WGM pointer is NULL or invalid")
            return {}

        self.log(f"WGM ptr: 0x{wgm_ptr:016X}")

        # Dump full WGM object header
        wgm_header = safe_read(self.pm, wgm_ptr, 0x200)
        if wgm_header:
            self.log(f"\nWGM object (0x200 bytes):")
            self.log(hex_dump(wgm_header, 0, 16))
            self.save_bin("wgm_header.bin", wgm_header)

        # Tree at WGM+0x18: +0x00 allocator, +0x08 head, +0x10 size
        tree_head = read_ptr(self.pm, wgm_ptr + 0x18 + 0x08)
        tree_size = read_u64(self.pm, wgm_ptr + 0x18 + 0x10)

        if tree_head is None or tree_size is None:
            self.log("\nTree: head or size unreadable")
            self.log("[ERROR] Invalid tree")
            return {}

        self.log(f"\nTree: head=0x{tree_head:016X}, size={tree_size}")

        if not is_valid_ptr(tree_head) or tree_size == 0 or tree_size > 1000:
            self.log("[ERROR] Invalid tree")
            return {}

        # Walk tree
        blocks = {}  # block_id -> info dict

        def get_is_nil(node):
            v = read_u8(self.pm, node + 0x19)
            return v is None or v != 0

        def get_left(node):
            return read_ptr(self.pm, node)

        def get_right(node):
            return read_ptr(self.pm, node + 0x10)

        def get_parent(node):
            return read_ptr(self.pm, node + 0x08)

        def min_node(node):
            while node and not get_is_nil(node):
                left = get_left(node)
                if not left or get_is_nil(left):
                    break
                node = left
            return node

        # root = tree_head->parent
        root = get_parent(tree_head)
        current = min_node(root)
        nodes_visited = 0

        while current and current != tree_head and not get_is_nil(current) and nodes_visited < 500:
            nodes_visited += 1

            block_id = read_u32(self.pm, current + 0x20)
            block_data_ptr = read_ptr(self.pm, current + 0x28)

            if block_id is not None and block_data_ptr and is_valid_ptr(block_data_ptr):
                has_rune = block_id in self.rune_tile_ids

                # Dump full BlockData (0x400 bytes)
                block_raw = safe_read(self.pm, block_data_ptr, 0x400)
                if block_raw:
                    bname = tile_name(block_id)
                    self.save_bin(f"blocks/{bname}_0x{block_id:08X}.bin", block_raw)

                # Read geom_ins_vector at BlockData+0x288
                vec_begin = read_ptr(self.pm, block_data_ptr + 0x288 + 0x08)
                vec_end   = read_ptr(self.pm, block_data_ptr + 0x288 + 0x10)

                geom_ins_list = []
                if vec_begin and vec_end and vec_end > vec_begin:
                    count = min((vec_end - vec_begin) // 8, 10000)

                    for i in range(count):
                        gi_ptr = read_ptr(self.pm, vec_begin + i * 8)
                        if not gi_ptr or not is_valid_ptr(gi_ptr):
                            continue

                        # Read name: gi+0x18+0x18+0x18 -> msb_part_ptr -> name_ptr -> wchar name
                        msb_part_ptr = read_ptr(self.pm, gi_ptr + 0x18 + 0x18 + 0x18)
                        if not msb_part_ptr or not is_valid_ptr(msb_part_ptr):
                            continue

                        name_ptr = read_ptr(self.pm, msb_part_ptr)
                        if not name_ptr or not is_valid_ptr(name_ptr):
                            continue

                        name_raw = safe_read(self.pm, name_ptr, 128)
                        if not name_raw:
                            continue

                        # Decode UTF-16LE
                        try:
                            name_str = name_raw.decode('utf-16-le').split('\x00')[0]
                        except:
                            name_str = ""

                        if not name_str.startswith("AEG099_821"):
                            continue

                        # This is a rune piece GeomIns! Dump it fully
                        gi_raw = safe_read(self.pm, gi_ptr, 0x300)
                        if gi_raw:
                            self.save_bin(
                                f"geom_ins/{tile_name(block_id)}_{name_str}_0x{gi_ptr:012X}.bin",
                                gi_raw)

                        # Read key flags
                        flag_1d8 = read_u32(self.pm, gi_ptr + 0x1D8)
                        flag_2c  = read_u32(self.pm, gi_ptr + 0x2C)

                        # Read more potential flags
                        extra_flags = {}
                        for foff in [0x08, 0x0C, 0x10, 0x14, 0x18, 0x1C, 0x20, 0x24, 0x28,
                                     0x30, 0x34, 0x38, 0x3C, 0x40, 0x44, 0x48,
                                     0x50, 0x58, 0x60, 0x68, 0x70, 0x78, 0x80,
                                     0x90, 0xA0, 0xB0, 0xC0, 0xD0, 0xE0, 0xF0,
                                     0x100, 0x110, 0x120, 0x130, 0x140, 0x150,
                                     0x160, 0x170, 0x180, 0x190, 0x1A0, 0x1B0,
                                     0x1C0, 0x1D0, 0x1D4, 0x1D8, 0x1DC, 0x1E0,
                                     0x1E4, 0x1E8, 0x1F0, 0x200, 0x210, 0x220,
                                     0x230, 0x240, 0x250, 0x260, 0x270, 0x280,
                                     0x290, 0x2A0, 0x2B0, 0x2C0, 0x2D0, 0x2E0, 0x2F0]:
                            v = read_u32(self.pm, gi_ptr + foff)
                            if v is not None:
                                extra_flags[foff] = v

                        geom_ins_list.append({
                            'name': name_str,
                            'ptr': gi_ptr,
                            'flag_1d8': flag_1d8,
                            'flag_2c': flag_2c,
                            'extra_flags': extra_flags
                        })

                blocks[block_id] = {
                    'data_ptr': block_data_ptr,
                    'node_ptr': current,
                    'has_rune': has_rune,
                    'geom_ins_count': len(geom_ins_list) if vec_begin else 0,
                    'vec_total': (vec_end - vec_begin) // 8 if (vec_begin and vec_end and vec_end > vec_begin) else 0,
                    'rune_geom_ins': geom_ins_list,
                }

            # Next in-order
            right = get_right(current)
            if right and not get_is_nil(right):
                current = min_node(right)
            else:
                parent = get_parent(current)
                while parent and parent != tree_head:
                    parent_right = get_right(parent)
                    if current != parent_right:
                        break
                    current = parent
                    parent = get_parent(current)
                current = parent

        # Report
        self.log(f"\nLoaded blocks: {len(blocks)}")
        rune_blocks = {bid: b for bid, b in blocks.items() if b['rune_geom_ins']}
        self.log(f"Blocks with rune piece GeomIns: {len(rune_blocks)}")

        # Detailed dump of rune piece blocks
        for bid in sorted(rune_blocks.keys()):
            b = rune_blocks[bid]
            bname = tile_name(bid)
            expected = self.tile_piece_counts.get(bid, '?')
            self.log(f"\n  {bname} (0x{bid:08X}): {len(b['rune_geom_ins'])} rune GeomIns "
                     f"(expected {expected} pieces, {b['vec_total']} total GeomIns)")
            self.log(f"    BlockData @ 0x{b['data_ptr']:016X}")

            for gi in b['rune_geom_ins']:
                self.log(f"\n    {gi['name']} @ 0x{gi['ptr']:016X}")
                f1d8 = gi['flag_1d8']
                f2c = gi['flag_2c']
                self.log(f"      +0x1D8 = 0x{f1d8:08X}  +0x2C = 0x{f2c:08X}"
                         if f1d8 is not None and f2c is not None else
                         f"      +0x1D8 = {f1d8}  +0x2C = {f2c}")
                # Show all extra flags that are non-zero
                nonzero = {k: v for k, v in gi['extra_flags'].items()
                           if v is not None and v != 0 and k not in (0x1D8, 0x2C)}
                if nonzero:
                    flag_str = '  '.join(f'+0x{k:X}=0x{v:08X}' for k, v in sorted(nonzero.items()))
                    self.log(f"      Non-zero: {flag_str}")

        # Also list loaded rune tiles with NO GeomIns (all collected?)
        empty_rune = {bid: b for bid, b in blocks.items()
                      if bid in self.rune_tile_ids and not b['rune_geom_ins']}
        if empty_rune:
            self.log(f"\n  Loaded rune tiles with 0 rune GeomIns (all collected on tile?):")
            for bid in sorted(empty_rune.keys()):
                b = empty_rune[bid]
                self.log(f"    {tile_name(bid)} (0x{bid:08X}): "
                         f"{b['vec_total']} total GeomIns, 0 rune pieces")

        # List all loaded blocks
        self.log(f"\n  All loaded blocks ({len(blocks)}):")
        for bid in sorted(blocks.keys()):
            b = blocks[bid]
            rune_str = f", {len(b['rune_geom_ins'])} rune" if b['rune_geom_ins'] else ""
            self.log(f"    {tile_name(bid)} (0x{bid:08X}): "
                     f"{b['vec_total']} GeomIns{rune_str}")

        return blocks

    # ─── Section 4: BlockData Deep Dump ─────────────────────────────────

    def dump_block_data_deep(self, blocks):
        """Deeper analysis of BlockData structures for rune-piece tiles."""
        self.log_section("SECTION 4: BlockData Deep Analysis (Rune Piece Tiles)")

        for bid in sorted(blocks.keys()):
            if bid not in self.rune_tile_ids:
                continue
            b = blocks[bid]
            bname = tile_name(bid)
            self.log(f"\n--- {bname} (0x{bid:08X}) BlockData @ 0x{b['data_ptr']:016X} ---")

            # Read larger block: 0x400 bytes
            raw = safe_read(self.pm, b['data_ptr'], 0x400)
            if not raw:
                self.log("  [Cannot read]")
                continue

            self.log(hex_dump(raw, 0, 16))

            # Look for pointers in BlockData that might lead to GEOF data
            self.log(f"\n  Pointers in BlockData:")
            for off in range(0, len(raw), 8):
                val = struct.unpack('<Q', raw[off:off+8])[0]
                if is_valid_ptr(val) and (val & 0x3) == 0:
                    probe = safe_read(self.pm, val, 32)
                    if probe:
                        # Check if it looks like GEOF data
                        has_geof = b'FOEG' in probe
                        marker = " *** GEOF MAGIC ***" if has_geof else ""
                        self.log(f"    +0x{off:03X} -> 0x{val:016X}{marker}")
                        self.log(hex_dump(probe, 0, 16))

    # ─── Section 5: Full Memory Scan ────────────────────────────────────

    def scan_all_memory(self, wgm_tiles=None, geof_tiles=None):
        self.log_section("SECTION 5: Full Memory Scan")

        # Patterns to search for
        geof_magic = b'FOEG'

        # Only scan for "problem" tiles: in WGM but NOT in GEOF
        # These are the tiles whose GEOF data we can't find
        target_tiles = {}
        if wgm_tiles and geof_tiles:
            problem_tids = (self.rune_tile_ids & set(wgm_tiles)) - set(geof_tiles)
            for tid in problem_tids:
                target_tiles[struct.pack('<I', tid)] = tile_name(tid)
            self.log(f"Scanning for {len(target_tiles)} problem tile IDs (WGM but not GEOF)")
        else:
            self.log("No WGM/GEOF tile sets provided, scanning for GEOF magic only")

        # Enumerate committed memory regions
        self.log("Enumerating committed memory regions...")
        regions = []
        addr = 0
        mbi = MEMORY_BASIC_INFORMATION()

        while addr < 0x7FFFFFFFFFFF:
            result = VirtualQueryEx(
                self.handle, ctypes.c_void_p(addr),
                ctypes.byref(mbi), ctypes.sizeof(mbi))
            if result == 0:
                break

            base_addr = mbi.BaseAddress or 0  # ctypes returns None for 0
            region_size = mbi.RegionSize or 0

            if region_size == 0:
                break

            if (mbi.State == MEM_COMMIT and
                mbi.Protect not in (PAGE_NOACCESS, PAGE_GUARD, 0) and
                not (mbi.Protect & PAGE_GUARD)):
                regions.append((base_addr, region_size))

            new_addr = base_addr + region_size
            if new_addr <= addr:
                break
            addr = new_addr

        total_bytes = sum(size for _, size in regions)
        self.log(f"Committed regions: {len(regions)}, total: {total_bytes / (1024*1024):.1f} MB")

        if total_bytes == 0:
            self.log("[WARN] No committed memory found")
            return

        # Scan all regions
        geof_hits = []
        tile_hits = {}  # tile_name -> list of addresses

        scanned = 0
        last_pct = -1
        CHUNK = 4 * 1024 * 1024  # 4MB chunks (fewer syscalls)

        # Known GEOF singleton addresses (to exclude from scan results)
        known_geof_ptrs = set()
        for rva in [RVA_GEOM_FLAG, RVA_GEOM_NONACTIVE]:
            ptr = read_ptr(self.pm, self.base + rva)
            if is_valid_ptr(ptr):
                known_geof_ptrs.add(ptr)

        self.log("Scanning for GEOF magic and problem tile IDs...")

        for region_base, region_size in regions:
            # Read in chunks
            for chunk_off in range(0, region_size, CHUNK):
                chunk_size = min(CHUNK, region_size - chunk_off)
                chunk_addr = region_base + chunk_off

                data = safe_read(self.pm, chunk_addr, chunk_size)
                if not data:
                    continue

                scanned += len(data)

                # Search for GEOF magic
                pos = 0
                while True:
                    idx = data.find(geof_magic, pos)
                    if idx == -1:
                        break
                    hit_addr = chunk_addr + idx

                    # Check if this is in a known singleton's data range
                    is_known = any(abs(hit_addr - kp) < 0x100000 for kp in known_geof_ptrs)

                    # Get context (64 bytes before and after)
                    ctx_start = max(0, idx - 32)
                    ctx_end = min(len(data), idx + 64)
                    context = data[ctx_start:ctx_end]

                    geof_hits.append({
                        'addr': hit_addr,
                        'is_known': is_known,
                        'context': context,
                        'ctx_offset': idx - ctx_start
                    })
                    pos = idx + 4

                # Search for problem tile IDs (only a few patterns, fast)
                for pattern, tname in target_tiles.items():
                    pos = 0
                    while True:
                        idx = data.find(pattern, pos)
                        if idx == -1:
                            break
                        hit_addr = chunk_addr + idx
                        if tname not in tile_hits:
                            tile_hits[tname] = []
                        tile_hits[tname].append(hit_addr)
                        pos = idx + 4

                # Progress
                pct = scanned * 100 // total_bytes
                if pct >= last_pct + 5:
                    last_pct = pct
                    print(f"\r  Scanning... {pct}% ({scanned//(1024*1024)} MB)", end="", flush=True)

        print(f"\r  Scan complete: {scanned//(1024*1024)} MB scanned        ")

        # Report GEOF hits
        self.log(f"\n--- GEOF Magic Hits: {len(geof_hits)} ---")
        known_count = sum(1 for h in geof_hits if h['is_known'])
        unknown_count = len(geof_hits) - known_count
        self.log(f"  In known singletons: {known_count}")
        self.log(f"  UNKNOWN locations:   {unknown_count}")

        for h in geof_hits:
            marker = " [KNOWN]" if h['is_known'] else " *** NEW ***"
            self.log(f"\n  FOEG @ 0x{h['addr']:016X}{marker}")
            ctx_base = h['addr'] - h['ctx_offset']
            for i in range(0, len(h['context']), 16):
                chunk = h['context'][i:i+16]
                hex_str = ' '.join(f'{b:02X}' for b in chunk)
                self.log(f"    0x{ctx_base+i:016X}: {hex_str}")

        # Report tile ID hits
        self.log(f"\n--- Problem Tile ID Scan Results ---")
        self.log(f"Problem tiles found in memory: {len(tile_hits)}")

        for tname in sorted(tile_hits.keys()):
            hits = tile_hits[tname]
            self.log(f"\n  {tname}: {len(hits)} hits")
            for addr in hits[:30]:  # Show first 30
                in_exe = self.base <= addr < self.base + 0x4000000
                region_info = "exe" if in_exe else "heap"
                self.log(f"    0x{addr:016X} ({region_info})")

    # ─── Section 6: Scan for GEOF-like entry patterns ───────────────────

    def scan_geof_entries_in_blocks(self, blocks):
        """Scan BlockData for GEOF-like entry patterns."""
        self.log_section("SECTION 6: GEOF-like Entry Patterns in BlockData")

        for bid in sorted(blocks.keys()):
            if bid not in self.rune_tile_ids:
                continue
            b = blocks[bid]
            bname = tile_name(bid)

            # Read larger area around BlockData
            raw = safe_read(self.pm, b['data_ptr'], 0x1000)
            if not raw:
                continue

            # Scan for geom indices 0x1194-0x11A6 as u16 LE
            hits = []
            for off in range(0, len(raw) - 2):
                val = struct.unpack('<H', raw[off:off+2])[0]
                if GEOM_IDX_MIN <= val <= GEOM_IDX_MAX:
                    # Check if byte before is 0x00 or 0x80 (flags pattern)
                    if off > 0 and raw[off-1] in (0x00, 0x80):
                        hits.append((off-1, raw[off-1], val))

            if hits:
                self.log(f"\n  {bname} (0x{bid:08X}): {len(hits)} GEOF-like entries in BlockData")
                for off, flags, geom_idx in hits:
                    pidx = piece_index_from_geof(geom_idx, flags)
                    ctx = raw[max(0,off-4):off+12]
                    self.log(f"    +0x{off:03X}: flags=0x{flags:02X} geom=0x{geom_idx:04X} "
                             f"slot={pidx} context={ctx.hex()}")

    # ─── Section 7: Follow GeomIns pointer chains deep ──────────────────

    def dump_geom_ins_deep(self, blocks):
        """Deep dump of CSWorldGeomIns structures for rune pieces."""
        self.log_section("SECTION 7: CSWorldGeomIns Deep Analysis")

        for bid in sorted(blocks.keys()):
            b = blocks[bid]
            if not b['rune_geom_ins']:
                continue

            bname = tile_name(bid)
            self.log(f"\n--- {bname} ---")

            for gi in b['rune_geom_ins']:
                gi_ptr = gi['ptr']
                name = gi['name']

                self.log(f"\n  {name} @ 0x{gi_ptr:016X}")

                # Full hex dump of GeomIns (0x300 bytes)
                gi_raw = safe_read(self.pm, gi_ptr, 0x300)
                if gi_raw:
                    self.log(f"  Full GeomIns dump:")
                    self.log(hex_dump(gi_raw, 0, 16))

                # Follow pointer at +0x18 (CSWorldGeomInfo)
                info_ptr = read_ptr(self.pm, gi_ptr + 0x18)
                if is_valid_ptr(info_ptr):
                    info_raw = safe_read(self.pm, info_ptr, 0x100)
                    if info_raw:
                        self.log(f"\n  CSWorldGeomInfo @ 0x{info_ptr:016X}:")
                        self.log(hex_dump(info_raw, 0, 16))

                # Follow pointer chain to name for verification
                f1d8 = gi['flag_1d8']
                f2c = gi['flag_2c']
                if f1d8 is not None and f2c is not None:
                    self.log(f"  flag_1d8=0x{f1d8:08X}  flag_2c=0x{f2c:08X}")
                else:
                    self.log(f"  flag_1d8={f1d8}  flag_2c={f2c}")

    # ─── Run everything ─────────────────────────────────────────────────

    def run(self):
        try:
            self._run_impl()
        except Exception:
            # Ensure report file is closed even on error
            if not self.report.closed:
                self.report.close()
            raise

    def _run_impl(self):
        self.log(f"Elden Ring Memory Dump")
        self.log(f"Date: {datetime.now().isoformat()}")
        self.log(f"Game base: 0x{self.base:016X}")
        self.log(f"Output: {self.dump_dir}")

        self.load_rune_pieces()

        # Section 1: Global region
        self.dump_global_region()

        # Section 2: GEOF singletons
        geof_main = self.dump_geof_singleton(RVA_GEOM_FLAG, "GeomFlagSaveDataManager")
        geof_nonactive = self.dump_geof_singleton(RVA_GEOM_NONACTIVE, "GeomNonActiveBlockManager")

        # Section 3: WGM
        blocks = self.dump_wgm()

        # Section 4: BlockData deep analysis
        if blocks:
            self.dump_block_data_deep(blocks)

        # Section 5: Full memory scan (this takes a while)
        # Pass tile sets so scan only looks for "problem" tiles, not all 321
        geof_tiles = set()
        if geof_main:
            geof_tiles |= set(geof_main.keys())
        if geof_nonactive:
            geof_tiles |= set(geof_nonactive.keys())
        self.scan_all_memory(wgm_tiles=set(blocks.keys()) if blocks else None,
                             geof_tiles=geof_tiles or None)

        # Section 6: GEOF patterns in BlockData
        if blocks:
            self.scan_geof_entries_in_blocks(blocks)

        # Section 7: Deep GeomIns dump
        if blocks:
            self.dump_geom_ins_deep(blocks)

        # Summary
        self.log_section("SUMMARY")

        # Count tiles covered
        geof_main_tiles = set(geof_main.keys()) if geof_main else set()
        geof_na_tiles = set(geof_nonactive.keys()) if geof_nonactive else set()
        wgm_tiles = set(blocks.keys()) if blocks else set()
        all_covered = geof_main_tiles | geof_na_tiles | wgm_tiles

        rune_in_geof = len(self.rune_tile_ids & (geof_main_tiles | geof_na_tiles))
        rune_in_wgm = len(self.rune_tile_ids & wgm_tiles)
        rune_nowhere = len(self.rune_tile_ids - all_covered)

        self.log(f"Rune piece tiles: {len(self.rune_tile_ids)} total")
        self.log(f"  In GEOF singletons: {rune_in_geof}")
        self.log(f"  In WGM (loaded):    {rune_in_wgm}")
        self.log(f"  Not found anywhere: {rune_nowhere}")

        # These "not found" tiles are NOT loaded and NOT in GEOF —
        # they're far-away tiles that will appear in GEOF when their region loads

        # What we care about: rune tiles in WGM but NOT in GEOF
        wgm_only = (self.rune_tile_ids & wgm_tiles) - geof_main_tiles - geof_na_tiles
        self.log(f"\n  In WGM but NOT in GEOF (the problem tiles): {len(wgm_only)}")
        for tid in sorted(wgm_only):
            b = blocks.get(tid, {})
            rune_gi = b.get('rune_geom_ins', [])
            expected = self.tile_piece_counts.get(tid, '?')
            self.log(f"    {tile_name(tid)}: {len(rune_gi)} alive GeomIns "
                     f"(expected {expected} pieces)")

        self.log(f"\nDump complete: {self.dump_dir}")
        self.report.close()
        print(f"Report saved to: {os.path.join(self.dump_dir, 'report.txt')}")


# ─── Entry Point ────────────────────────────────────────────────────────

if __name__ == "__main__":
    label = sys.argv[1] if len(sys.argv) > 1 else ""

    try:
        dumper = GameDumper(label)
        dumper.run()
    except pymem.exception.ProcessNotFound:
        print("ERROR: eldenring.exe not found. Is the game running?")
        sys.exit(1)
    except pymem.exception.CouldNotOpenProcess:
        print("ERROR: Cannot open process. Try running as administrator.")
        sys.exit(1)
    except Exception as e:
        print(f"ERROR: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)
