#!/usr/bin/env python3
"""Simulate DLL refresh() from live game memory. Shows exactly what would be collected."""

import pymem, struct, json, re
from pathlib import Path
from collections import defaultdict

RVA_GEOM_FLAG      = 0x3D69D18
RVA_GEOM_NONACTIVE = 0x3D69D98
RVA_WORLD_GEOM_MAN = 0x3D69BA8
GEOM_IDX_MIN = 0x1194

def safe_read(pm, addr, size):
    try: return pm.read_bytes(addr, size)
    except: return None
def read_u8(pm, a):
    d = safe_read(pm, a, 1); return struct.unpack('<B', d)[0] if d else None
def read_u32(pm, a):
    d = safe_read(pm, a, 4); return struct.unpack('<I', d)[0] if d else None
def read_u64(pm, a):
    d = safe_read(pm, a, 8); return struct.unpack('<Q', d)[0] if d else None
def read_ptr(pm, a): return read_u64(pm, a)
def is_valid_ptr(v): return v and 0x10000 < v < 0x7FFFFFFFFFFF

def encode_tile(a, gx, gz): return (a << 24) | (gx << 16) | (gz << 8)
def tile_str(t): return f"m{(t>>24)&0xFF:02d}_{(t>>16)&0xFF:02d}_{(t>>8)&0xFF:02d}"
def prefix_from_name(name):
    i = name.rfind('_')
    return name[:i] if i > 0 else ""
def prefix_from_mid(mid):
    if mid < 10000000: return ""
    raw = mid - 10000000
    return f"AEG{raw//1000:03d}_{raw%1000:03d}"
def mid_from_prefix(p):
    m = re.match(r'AEG(\d+)_(\d+)', p)
    return 10000000 + int(m.group(1))*1000 + int(m.group(2)) if m else 0

# ── Load map data (replicate DLL initialize()) ──
def load_map_data():
    project = Path(__file__).parent.parent
    slots_dir = project / "data" / "massedit_generated"

    # Build: tile -> name -> row_id, tile -> prefix -> slot -> row_id
    tile_name_to_row = defaultdict(dict)     # tile -> {obj_name: row_id}
    tile_slot_to_row = defaultdict(lambda: defaultdict(dict))  # tile -> prefix -> {slot: row_id}
    tracked_prefixes = set()
    tracked_model_ids = set()

    for path in slots_dir.glob("*_slots.json"):
        with open(path) as f:
            slots_data = json.load(f)
        massedit_name = path.stem.replace("_slots", "")
        massedit_path = slots_dir / f"{massedit_name}.MASSEDIT"
        row_coords = {}
        if massedit_path.exists():
            pat = re.compile(r"id\s+(\d+):\s+(\w+):\s*=\s*(.+);")
            with open(massedit_path) as f:
                for line in f:
                    m = pat.search(line)
                    if m:
                        rid = int(m.group(1))
                        if rid not in row_coords: row_coords[rid] = {}
                        row_coords[rid][m.group(2)] = m.group(3).strip()

        for row_str, meta in slots_data.items():
            row_id = int(row_str)
            obj_name = meta.get("object_name", "")
            geom_slot = meta.get("geom_slot", -1)
            if not obj_name: continue

            prefix = prefix_from_name(obj_name)
            if not prefix: continue

            coords = row_coords.get(row_id, {})
            area = int(float(coords.get("areaNo", "0")))
            gx = int(float(coords.get("gridXNo", "0")))
            gz = int(float(coords.get("gridZNo", "0")))
            tile = encode_tile(area, gx, gz)

            tracked_prefixes.add(prefix)
            tile_name_to_row[tile][obj_name] = row_id
            if geom_slot >= 0:
                tile_slot_to_row[tile][prefix][geom_slot] = row_id

    for p in tracked_prefixes:
        mid = mid_from_prefix(p)
        if mid: tracked_model_ids.add(mid)

    return tile_name_to_row, tile_slot_to_row, tracked_prefixes, tracked_model_ids

# ── WGM scan (returns tile -> set of alive names) ──
def scan_wgm_alive(pm, base, tracked_prefixes):
    alive = {}  # tile_id -> set of alive object names
    wgm = read_ptr(pm, base + RVA_WORLD_GEOM_MAN)
    if not is_valid_ptr(wgm): return alive
    tree_head = read_ptr(pm, wgm + 0x18 + 0x08)
    tree_size = read_u64(pm, wgm + 0x18 + 0x10)
    if not is_valid_ptr(tree_head) or not tree_size or tree_size > 1000: return alive

    def get_nil(n): v = read_u8(pm, n+0x19); return v is None or v != 0
    def get_left(n): return read_ptr(pm, n)
    def get_right(n): return read_ptr(pm, n+0x10)
    def get_parent(n): return read_ptr(pm, n+0x08)
    def min_node(n):
        while n and not get_nil(n):
            l = get_left(n)
            if not l or get_nil(l): break
            n = l
        return n

    current = min_node(read_ptr(pm, tree_head + 0x08))
    visited = 0
    while current and current != tree_head and not get_nil(current) and visited < 500:
        visited += 1
        block_id = read_u32(pm, current + 0x20)
        block_data = read_ptr(pm, current + 0x28)
        if block_data:
            vb = read_ptr(pm, block_data + 0x288 + 0x08)
            ve = read_ptr(pm, block_data + 0x288 + 0x10)
            if vb and ve and ve > vb:
                for i in range(min((ve-vb)//8, 10000)):
                    gi = read_ptr(pm, vb + i*8)
                    if not gi: continue
                    mp = read_ptr(pm, gi + 0x48)
                    if not mp: continue
                    np = read_ptr(pm, mp)
                    if not is_valid_ptr(np): continue
                    raw = safe_read(pm, np, 128)
                    if not raw: continue
                    name = raw.decode('utf-16-le', errors='ignore').split('\0')[0]
                    prefix = prefix_from_name(name)
                    if not prefix or prefix not in tracked_prefixes: continue
                    if block_id not in alive: alive[block_id] = set()
                    f263 = read_u8(pm, gi + 0x263) or 0
                    f269 = read_u8(pm, gi + 0x269) or 0
                    is_alive = bool(f263 & 0x02) and not bool(f269 & 0x60)
                    if is_alive:
                        alive[block_id].add(name)
        right = get_right(current)
        if right and not get_nil(right):
            current = min_node(right)
        else:
            parent = get_parent(current)
            w = 0
            while parent and parent != tree_head and w < 500:
                w += 1
                if current != get_right(parent): break
                current = parent; parent = get_parent(current)
            if w >= 500: break
            current = parent
    return alive

# ── GEOF scan ──
def scan_geof(pm, base, tracked_model_ids):
    entries = []
    for rva in [RVA_GEOM_FLAG, RVA_GEOM_NONACTIVE]:
        ptr = read_ptr(pm, base + rva)
        if not is_valid_ptr(ptr): continue
        ce = 0
        for off in range(0x08, 0x20000, 16):
            iv = read_u64(pm, ptr+off); pv = read_u64(pm, ptr+off+8)
            if iv is None or pv is None:
                ce += 1
                if ce > 256: break
                continue
            if iv == 0 and pv == 0:
                ce += 1
                if ce > 256: break
                continue
            ce = 0
            tid = iv & 0xFFFFFFFF
            if ((tid>>24)&0xFF) < 0x0A or ((tid>>24)&0xFF) > 0x3D: continue
            if not is_valid_ptr(pv): continue
            hdr = safe_read(pm, pv, 16)
            if not hdr: continue
            cA = struct.unpack_from('<I', hdr, 8)[0]
            cB = struct.unpack_from('<I', hdr, 0)[0]
            if 0 < cA < 100000: cnt, es = cA, pv+16
            elif 0 < cB < 100000: cnt, es = cB, pv+8
            else: continue
            for ei in range(cnt):
                e = safe_read(pm, es+ei*8, 8)
                if not e: break
                fl, gidx = e[1], e[2]|(e[3]<<8)
                mh = struct.unpack_from('<I', e, 4)[0]
                if fl in (0x00, 0x80) and gidx >= GEOM_IDX_MIN and mh in tracked_model_ids:
                    slot = (gidx - GEOM_IDX_MIN)*2 + (1 if fl & 0x80 else 0)
                    entries.append((tid, prefix_from_mid(mh), slot))
    return entries

def main():
    pm = pymem.Pymem("eldenring.exe")
    base = pm.base_address

    print("Loading map data...")
    tile_name_to_row, tile_slot_to_row, tracked_prefixes, tracked_model_ids = load_map_data()
    print(f"  {len(tracked_prefixes)} prefixes, {len(tracked_model_ids)} model IDs")

    print("\nScanning WGM...")
    alive = scan_wgm_alive(pm, base, tracked_prefixes)
    wgm_tiles = set(alive.keys())
    print(f"  {len(wgm_tiles)} WGM tiles")

    print("Scanning GEOF...")
    geof = scan_geof(pm, base, tracked_model_ids)
    print(f"  {len(geof)} GEOF entries")

    # ── Compute collected (exactly like DLL refresh()) ──
    collected = set()  # row_ids
    collected_detail = []  # (row_id, source, tile, name_or_prefix)

    # WGM phase
    for tile_id, alive_names in alive.items():
        names = tile_name_to_row.get(tile_id, {})
        for obj_name, row_id in names.items():
            if obj_name not in alive_names:
                collected.add(row_id)
                collected_detail.append((row_id, "WGM", tile_id, obj_name))

    # GEOF phase (skip WGM tiles)
    geof_by_tile_prefix = defaultdict(lambda: defaultdict(list))
    for tid, prefix, slot in geof:
        geof_by_tile_prefix[tid][prefix].append(slot)

    for tid, prefix_slots in geof_by_tile_prefix.items():
        if tid in wgm_tiles: continue
        tile_data = tile_slot_to_row.get(tid, {})
        for prefix, slots in prefix_slots.items():
            prefix_data = tile_data.get(prefix, {})
            for slot in slots:
                row_id = prefix_data.get(slot)
                if row_id is not None:
                    collected.add(row_id)
                    collected_detail.append((row_id, "GEOF", tid, f"{prefix} slot={slot}"))

    # ── Report ──
    print(f"\n=== COLLECTED SET: {len(collected)} entries ===\n")

    # Group by source
    wgm_entries = [(r,t,n) for r,s,t,n in collected_detail if s == "WGM"]
    geof_entries = [(r,t,n) for r,s,t,n in collected_detail if s == "GEOF"]

    print(f"From WGM: {len(wgm_entries)}")
    for row_id, tile_id, name in sorted(wgm_entries, key=lambda x: (x[1], x[2])):
        print(f"  row={row_id:>8d}  tile=0x{tile_id:08X} ({tile_str(tile_id)})  {name}")

    print(f"\nFrom GEOF: {len(geof_entries)}")
    # Show only material node GEOF (not 821/822)
    mat_geof = [(r,t,n) for r,t,n in geof_entries if not n.startswith("AEG099_821") and not n.startswith("AEG099_822")]
    piece_geof = [(r,t,n) for r,t,n in geof_entries if n.startswith("AEG099_821") or n.startswith("AEG099_822")]
    print(f"  ({len(piece_geof)} pieces + {len(mat_geof)} material nodes)")
    for row_id, tile_id, desc in sorted(mat_geof, key=lambda x: (x[1], x[2])):
        print(f"  row={row_id:>8d}  tile=0x{tile_id:08X} ({tile_str(tile_id)})  {desc}")

    # ── Check: is AEG099_691_9000 on any WGM tile collected? ──
    print(f"\n=== AEG099_691 status ===")
    for tile_id, alive_names in alive.items():
        names = tile_name_to_row.get(tile_id, {})
        for obj_name in sorted(names.keys()):
            if "691" in obj_name:
                in_alive = obj_name in alive_names
                in_collected = names[obj_name] in collected
                print(f"  {obj_name} tile=0x{tile_id:08X}  row={names[obj_name]}  "
                      f"alive_in_wgm={in_alive}  in_collected={in_collected}")

    # Also check GEOF for 691
    for tid, prefix, slot in geof:
        if "691" in prefix:
            tile_data = tile_slot_to_row.get(tid, {})
            prefix_data = tile_data.get(prefix, {})
            row_id = prefix_data.get(slot)
            in_wgm = tid in wgm_tiles
            print(f"  GEOF: {prefix} slot={slot} tile=0x{tid:08X}  row={row_id}  "
                  f"{'SKIPPED(WGM)' if in_wgm else 'COUNTED'}")

if __name__ == "__main__":
    main()
