#!/usr/bin/env python3
"""
Dump ALL GeomIns names on a specific tile from game memory.
Shows the vector index (position in geom_ins_vector) for each object.
This reveals how geom_idx maps to MSB names.

Usage: python dump_tile_geom.py <tile_name>
Example: python dump_tile_geom.py m60_39_40_00
"""
import pymem
import struct
import sys

RVA_WORLD_GEOM_MAN = 0x3D69BA8

def safe_read(pm, addr, size):
    try:
        return pm.read_bytes(addr, size)
    except:
        return None

def read_ptr(pm, addr):
    d = safe_read(pm, addr, 8)
    return struct.unpack('<Q', d)[0] if d else None

def read_u8(pm, addr):
    d = safe_read(pm, addr, 1)
    return d[0] if d else None

def read_u32(pm, addr):
    d = safe_read(pm, addr, 4)
    return struct.unpack('<I', d)[0] if d else None

def is_valid_ptr(val):
    return val is not None and 0x10000 < val < 0x7FFFFFFFFFFF

def parse_tile_name(name):
    """m60_39_40_00 -> tile_id"""
    parts = name.split('_')
    area = int(parts[0][1:])
    gx = int(parts[1])
    gz = int(parts[2])
    idx = int(parts[3])
    return (area << 24) | (gx << 16) | (gz << 8) | idx

def main():
    if len(sys.argv) < 2:
        print("Usage: python dump_tile_geom.py m60_39_40_00")
        sys.exit(1)

    target_tile = sys.argv[1]
    target_id = parse_tile_name(target_tile)
    print(f"Looking for tile {target_tile} (0x{target_id:08X})")

    pm = pymem.Pymem("eldenring.exe")
    base = pm.base_address

    wgm = read_ptr(pm, base + RVA_WORLD_GEOM_MAN)
    if not is_valid_ptr(wgm):
        print("WGM not found")
        return

    tree_head = read_ptr(pm, wgm + 0x18 + 0x08)
    tree_size = read_ptr(pm, wgm + 0x18 + 0x10)
    if not is_valid_ptr(tree_head) or not tree_size:
        print("Tree invalid")
        return

    print(f"WGM tree: {tree_size} blocks")

    # Walk RB tree to find our block
    def get_is_nil(n):
        v = read_u8(pm, n + 0x19)
        return v is None or v != 0
    def get_left(n): return read_ptr(pm, n)
    def get_right(n): return read_ptr(pm, n + 0x10)
    def get_parent(n): return read_ptr(pm, n + 0x08)
    def min_node(n):
        while n and not get_is_nil(n):
            l = get_left(n)
            if not l or get_is_nil(l): break
            n = l
        return n

    root = get_parent(tree_head)
    current = min_node(root)
    found = False

    while current and current != tree_head and not get_is_nil(current):
        block_id = read_u32(pm, current + 0x20)
        block_data = read_ptr(pm, current + 0x28)

        if block_id == target_id and is_valid_ptr(block_data):
            found = True
            vec_begin = read_ptr(pm, block_data + 0x288 + 0x08)
            vec_end = read_ptr(pm, block_data + 0x288 + 0x10)

            if not vec_begin or not vec_end or vec_end <= vec_begin:
                print("Empty vector")
                break

            count = (vec_end - vec_begin) // 8
            print(f"\nFound {target_tile}: {count} GeomIns in vector")
            print(f"{'idx':>5} {'name':30s} {'flag_263':>10} {'flag_269':>10} {'flag_1d8':>10}")
            print("-" * 75)

            aeg821_indices = []

            for i in range(min(count, 10000)):
                gi = read_ptr(pm, vec_begin + i * 8)
                if not gi or not is_valid_ptr(gi):
                    continue

                # Read name
                msb_part = read_ptr(pm, gi + 0x18 + 0x18 + 0x18)
                if not msb_part or not is_valid_ptr(msb_part):
                    continue
                name_p = read_ptr(pm, msb_part)
                if not name_p or not is_valid_ptr(name_p):
                    continue
                name_raw = safe_read(pm, name_p, 128)
                if not name_raw:
                    continue
                try:
                    name = name_raw.decode('utf-16-le').split('\x00')[0]
                except:
                    continue

                # Only show AEG099_821 objects (to keep output manageable)
                if 'AEG099_821' in name:
                    b263 = read_u8(pm, gi + 0x263) or 0
                    b269 = read_u8(pm, gi + 0x269) or 0
                    b1d8 = read_u32(pm, gi + 0x1D8) or 0
                    flag263 = "CLR(coll)" if not (b263 & 0x02) else "SET(alive)"
                    print(f"{i:5d} {name:30s} {flag263:>10} 0x{b269:02X}       0x{b1d8:08X}")
                    aeg821_indices.append((i, name))

            # Also show a few non-821 objects near the AEG099_821 indices
            if aeg821_indices:
                print(f"\n--- Objects near AEG099_821 in vector (±5 positions) ---")
                target_positions = set()
                for idx, _ in aeg821_indices:
                    for j in range(max(0, idx-5), idx+6):
                        target_positions.add(j)

                for i in sorted(target_positions):
                    if i >= count: continue
                    gi = read_ptr(pm, vec_begin + i * 8)
                    if not gi or not is_valid_ptr(gi):
                        continue
                    msb_part = read_ptr(pm, gi + 0x18 + 0x18 + 0x18)
                    if not msb_part or not is_valid_ptr(msb_part):
                        print(f"{i:5d} [no name]")
                        continue
                    name_p = read_ptr(pm, msb_part)
                    if not name_p or not is_valid_ptr(name_p):
                        print(f"{i:5d} [no name ptr]")
                        continue
                    name_raw = safe_read(pm, name_p, 128)
                    if not name_raw:
                        continue
                    try:
                        name = name_raw.decode('utf-16-le').split('\x00')[0]
                    except:
                        continue
                    marker = " <<<" if 'AEG099_821' in name else ""
                    print(f"{i:5d} {name}{marker}")
            break

        # Next in-order
        right = get_right(current)
        if right and not get_is_nil(right):
            current = min_node(right)
        else:
            parent = get_parent(current)
            while parent and parent != tree_head:
                if current != get_right(parent): break
                current = parent
                parent = get_parent(current)
            current = parent

    if not found:
        print(f"\n{target_tile} not in WGM (tile not loaded). Teleport closer and retry.")

if __name__ == "__main__":
    main()
