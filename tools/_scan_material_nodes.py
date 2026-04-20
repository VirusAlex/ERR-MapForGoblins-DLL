#!/usr/bin/env python3
"""Quick scan: WGM + GEOF state for material nodes. Run while game is loaded."""

import pymem
import struct
import json
from pathlib import Path

RVA_GEOM_FLAG      = 0x3D69D18
RVA_GEOM_NONACTIVE = 0x3D69D98
RVA_WORLD_GEOM_MAN = 0x3D69BA8

def safe_read(pm, addr, size):
    try: return pm.read_bytes(addr, size)
    except: return None

def read_u8(pm, a):
    d = safe_read(pm, a, 1); return struct.unpack('<B', d)[0] if d else None
def read_u16(pm, a):
    d = safe_read(pm, a, 2); return struct.unpack('<H', d)[0] if d else None
def read_u32(pm, a):
    d = safe_read(pm, a, 4); return struct.unpack('<I', d)[0] if d else None
def read_u64(pm, a):
    d = safe_read(pm, a, 8); return struct.unpack('<Q', d)[0] if d else None
def read_ptr(pm, a): return read_u64(pm, a)
def is_valid_ptr(v): return v and 0x10000 < v < 0x7FFFFFFFFFFF

def tile_name(tid):
    return f"m{(tid>>24)&0xFF:02d}_{(tid>>16)&0xFF:02d}_{(tid>>8)&0xFF:02d}_{tid&0xFF:02d}"

def encode_tile(area, gx, gz):
    return (area << 24) | (gx << 16) | (gz << 8)

def prefix_from_model_id(mid):
    if mid < 10000000: return None
    raw = mid - 10000000
    return f"AEG{raw//1000:03d}_{raw%1000:03d}"

def model_id_from_name(name):
    # "AEG099_691_9000" -> 10099691
    parts = name.split("_")
    if len(parts) < 3 or not name.startswith("AEG"): return 0
    group = int(parts[0][3:])
    model = int(parts[1])
    return 10000000 + group * 1000 + model

# ── Load our generated map data ──
def load_map_data():
    """Load slots JSONs to build tile->name->slot mapping."""
    project = Path(__file__).parent.parent
    slots_dir = project / "data" / "massedit_generated"

    # tile -> set of object_names
    tile_names = {}
    name_to_tile = {}

    for path in slots_dir.glob("*_slots.json"):
        with open(path) as f:
            data = json.load(f)

        # Need MASSEDIT to get areaNo/gridXNo/gridZNo for each row
        massedit_name = path.stem.replace("_slots", "")
        massedit_path = slots_dir / f"{massedit_name}.MASSEDIT"

        # Parse MASSEDIT for grid coords per row
        row_coords = {}
        if massedit_path.exists():
            import re
            pat = re.compile(r"id\s+(\d+):\s+(\w+):\s*=\s*(.+);")
            with open(massedit_path) as f:
                for line in f:
                    m = pat.search(line)
                    if m:
                        rid, field, val = int(m.group(1)), m.group(2), m.group(3).strip()
                        if rid not in row_coords:
                            row_coords[rid] = {}
                        row_coords[rid][field] = val

        for row_str, meta in data.items():
            row_id = int(row_str)
            obj_name = meta.get("object_name", "")
            if not obj_name: continue

            coords = row_coords.get(row_id, {})
            area = int(float(coords.get("areaNo", "0")))
            gx = int(float(coords.get("gridXNo", "0")))
            gz = int(float(coords.get("gridZNo", "0")))
            tile = encode_tile(area, gx, gz)

            if tile not in tile_names:
                tile_names[tile] = set()
            tile_names[tile].add(obj_name)
            name_to_tile[obj_name + f"@{tile:08X}"] = row_id

    return tile_names, name_to_tile

# ── Scan WGM ──
def scan_wgm(pm, base):
    """Read all tracked geom objects from CSWorldGeomMan."""
    wgm = read_ptr(pm, base + RVA_WORLD_GEOM_MAN)
    if not is_valid_ptr(wgm):
        print("WGM: NULL")
        return {}

    tree_head = read_ptr(pm, wgm + 0x18 + 0x08)
    tree_size = read_u64(pm, wgm + 0x18 + 0x10)
    if not is_valid_ptr(tree_head) or not tree_size or tree_size > 1000:
        print(f"WGM: tree_head invalid or size={tree_size}")
        return {}

    print(f"WGM: {tree_size} blocks in tree")

    def get_nil(node):
        v = read_u8(pm, node + 0x19)
        return v is None or v != 0
    def get_left(n): return read_ptr(pm, n)
    def get_right(n): return read_ptr(pm, n + 0x10)
    def get_parent(n): return read_ptr(pm, n + 0x08)

    def min_node(n):
        while n and not get_nil(n):
            l = get_left(n)
            if not l or get_nil(l): break
            n = l
        return n

    root = read_ptr(pm, tree_head + 0x08)
    current = min_node(root)

    # tile_id -> list of {name, alive, f263, f269, model_id}
    results = {}
    visited = 0

    while current and current != tree_head and not get_nil(current) and visited < 500:
        visited += 1
        block_id = read_u32(pm, current + 0x20)
        block_data = read_ptr(pm, current + 0x28)

        if block_data:
            vec_begin = read_ptr(pm, block_data + 0x288 + 0x08)
            vec_end = read_ptr(pm, block_data + 0x288 + 0x10)

            if vec_begin and vec_end and vec_end > vec_begin:
                count = min((vec_end - vec_begin) // 8, 10000)

                for i in range(count):
                    geom_ins = read_ptr(pm, vec_begin + i * 8)
                    if not geom_ins: continue

                    msb_part = read_ptr(pm, geom_ins + 0x48)
                    if not msb_part: continue
                    name_ptr = read_ptr(pm, msb_part)
                    if not is_valid_ptr(name_ptr): continue

                    name_raw = safe_read(pm, name_ptr, 128)
                    if not name_raw: continue
                    name = name_raw.decode('utf-16-le', errors='ignore').split('\0')[0]

                    if not name.startswith("AEG"): continue

                    # Check if tracked (has underscore pattern like AEG099_691_9000)
                    parts = name.split("_")
                    if len(parts) < 3: continue

                    mid = model_id_from_name(name)

                    f263 = read_u8(pm, geom_ins + 0x263)
                    f269 = read_u8(pm, geom_ins + 0x269)
                    alive = bool((f263 & 0x02)) and not bool((f269 & 0x60))

                    # Also read model_hash at +0x28 for GEOF comparison
                    model_hash = read_u32(pm, geom_ins + 0x28)

                    if block_id not in results:
                        results[block_id] = []
                    results[block_id].append({
                        'name': name,
                        'alive': alive,
                        'f263': f263,
                        'f269': f269,
                        'model_id': mid,
                        'model_hash': model_hash,
                        'geom_ins': geom_ins,
                    })

        # In-order successor
        right = get_right(current)
        if right and not get_nil(right):
            current = min_node(right)
        else:
            parent = get_parent(current)
            w = 0
            while parent and parent != tree_head and w < 500:
                w += 1
                if current != get_right(parent): break
                current = parent
                parent = get_parent(current)
            if w >= 500: break
            current = parent

    return results

# ── Scan GEOF ──
def scan_geof_singleton(pm, base, rva, name):
    """Read GEOF entries from a singleton."""
    ptr = read_ptr(pm, base + rva)
    if not is_valid_ptr(ptr):
        print(f"  {name}: NULL")
        return []

    results = []
    consecutive_empty = 0
    for off in range(0x08, 0x20000, 16):
        id_val = read_u64(pm, ptr + off)
        ptr_val = read_u64(pm, ptr + off + 8)

        if id_val == 0 and ptr_val == 0:
            consecutive_empty += 1
            if consecutive_empty > 256: break
            continue
        consecutive_empty = 0

        tile_id = id_val & 0xFFFFFFFF
        area = (tile_id >> 24) & 0xFF
        if area < 0x0A or area > 0x3D: continue
        if not is_valid_ptr(ptr_val): continue

        # Read header
        header = safe_read(pm, ptr_val, 16)
        if not header: continue

        countA = struct.unpack_from('<I', header, 8)[0]
        countB = struct.unpack_from('<I', header, 0)[0]

        if 0 < countA < 100000:
            count, entries_start = countA, ptr_val + 16
        elif 0 < countB < 100000:
            count, entries_start = countB, ptr_val + 8
        else:
            continue

        for ei in range(count):
            entry = safe_read(pm, entries_start + ei * 8, 8)
            if not entry: break

            flags = entry[1]
            geom_idx = entry[2] | (entry[3] << 8)
            model_hash = struct.unpack_from('<I', entry, 4)[0]

            if flags in (0x00, 0x80) and geom_idx >= 0x1194:
                results.append({
                    'tile_id': tile_id,
                    'flags': flags,
                    'geom_idx': geom_idx,
                    'model_hash': model_hash,
                    'slot': (geom_idx - 0x1194) * 2 + (1 if flags & 0x80 else 0),
                    'prefix': prefix_from_model_id(model_hash),
                })

    print(f"  {name}: {len(results)} entries")
    return results


def main():
    pm = pymem.Pymem("eldenring.exe")
    base = pm.base_address
    print(f"Base: 0x{base:X}\n")

    # Load our map data
    print("Loading map data...")
    tile_names, name_to_tile = load_map_data()
    print(f"  {sum(len(v) for v in tile_names.values())} entries across {len(tile_names)} tiles\n")

    # ── WGM scan ──
    print("=== WGM (loaded tiles) ===")
    wgm = scan_wgm(pm, base)

    # Filter: only tiles where we have data OR dead material nodes
    wgm_material = {}
    for tile_id, objects in wgm.items():
        our_names = tile_names.get(tile_id, set())
        for obj in objects:
            # Only include if: in our data for this tile, or dead
            if obj['name'] in our_names or not obj['alive']:
                if tile_id not in wgm_material:
                    wgm_material[tile_id] = []
                wgm_material[tile_id].append(obj)

    print(f"\nWGM tiles with tracked objects: {len(wgm_material)}")
    for tile_id in sorted(wgm_material.keys()):
        objects = wgm_material[tile_id]
        our_names = tile_names.get(tile_id, set())
        print(f"\n  Tile 0x{tile_id:08X} ({tile_name(tile_id)}):")
        print(f"    WGM objects: {len(objects)}, Our data: {len(our_names)} entries")

        for obj in objects:
            in_data = "OK" if obj['name'] in our_names else "XX NOT IN DATA"
            status = "ALIVE" if obj['alive'] else "DEAD"
            print(f"    {obj['name']:25s} {status:5s}  f263=0x{obj['f263']:02X} f269=0x{obj['f269']:02X}  "
                  f"model_hash=0x{obj['model_hash']:08X} model_id={obj['model_id']}  {in_data}")

        # Show entries in our data that WGM doesn't have
        wgm_names = {o['name'] for o in objects}
        missing = our_names - wgm_names
        if missing:
            print(f"    --- In our data but NOT in WGM geom_ins: ---")
            for n in sorted(missing):
                print(f"    {n:25s}  (not in geom_ins vector)")

    # ── GEOF scan ──
    print("\n\n=== GEOF (unloaded tiles) ===")
    geof_active = scan_geof_singleton(pm, base, RVA_GEOM_FLAG, "GeomFlagSaveDataManager")
    geof_nonact = scan_geof_singleton(pm, base, RVA_GEOM_NONACTIVE, "GeomNonActiveBlockManager")
    geof_all = geof_active + geof_nonact

    # Group by tile and prefix
    geof_by_tile = {}
    for e in geof_all:
        tid = e['tile_id']
        if tid not in geof_by_tile:
            geof_by_tile[tid] = []
        geof_by_tile[tid].append(e)

    # Show GEOF entries that match our tiles
    wgm_tile_set = set(wgm_material.keys())
    print(f"\nGEOF tiles: {len(geof_by_tile)}, WGM tiles: {len(wgm_tile_set)}")

    matched_geof = 0
    unmatched_geof = 0
    for tid in sorted(geof_by_tile.keys()):
        entries = geof_by_tile[tid]
        our_names = tile_names.get(tid, set())
        in_wgm = "  (IN WGM - skipped)" if tid in wgm_tile_set else ""

        # Only show tiles we have data for
        if not our_names and tid not in wgm_tile_set:
            continue

        # Show non-821/822 entries
        non_piece = [e for e in entries if e['prefix'] and e['prefix'] not in ('AEG099_821', 'AEG099_822')]
        if non_piece:
            print(f"\n  Tile 0x{tid:08X} ({tile_name(tid)}): {len(entries)} GEOF entries, "
                  f"{len(our_names)} in our data{in_wgm}")
            for e in non_piece:
                print(f"    {e['prefix']}  geom_idx=0x{e['geom_idx']:04X} flags=0x{e['flags']:02X} "
                      f"slot={e['slot']}  model_hash=0x{e['model_hash']:08X}")

    # ── Summary: check model_hash vs model_id ──
    print("\n\n=== Model Hash Analysis ===")
    # From WGM, get model_hash for each prefix
    hash_map = {}
    for tile_id, objects in wgm_material.items():
        for obj in objects:
            prefix = prefix_from_model_id(obj['model_id'])
            if prefix and prefix not in hash_map:
                hash_map[prefix] = obj['model_hash']

    print("Prefix -> model_hash (from WGM +0x28) vs expected model_id:")
    for prefix in sorted(hash_map.keys()):
        expected_mid = model_id_from_name(prefix + "_9000")
        actual_hash = hash_map[prefix]
        match = "OK" if actual_hash == expected_mid else f"XX MISMATCH!"
        print(f"  {prefix}: hash=0x{actual_hash:08X} ({actual_hash}), "
              f"expected_id=0x{expected_mid:08X} ({expected_mid})  {match}")


if __name__ == "__main__":
    main()
