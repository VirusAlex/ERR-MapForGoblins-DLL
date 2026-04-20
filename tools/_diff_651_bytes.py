#!/usr/bin/env python3
"""Compare bytes of collected vs alive AEG099_651 geom_ins objects."""

import pymem, struct

RVA_WORLD_GEOM_MAN = 0x3D69BA8

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

def scan_wgm_651(pm, base):
    """Find all AEG099_651 geom_ins with their addresses and alive state."""
    wgm = read_ptr(pm, base + RVA_WORLD_GEOM_MAN)
    if not is_valid_ptr(wgm): return []
    tree_head = read_ptr(pm, wgm + 0x18 + 0x08)
    tree_size = read_u64(pm, wgm + 0x18 + 0x10)
    if not is_valid_ptr(tree_head) or not tree_size or tree_size > 1000: return []

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

    results = []
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
                    if name.startswith("AEG099_651"):
                        f263 = read_u8(pm, gi + 0x263) or 0
                        f269 = read_u8(pm, gi + 0x269) or 0
                        is_alive = bool(f263 & 0x02) and not bool(f269 & 0x60)
                        results.append({
                            'name': name, 'block_id': block_id,
                            'addr': gi, 'f263': f263, 'f269': f269,
                            'alive_check': is_alive,
                        })
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
    return results

def main():
    pm = pymem.Pymem("eldenring.exe")
    base = pm.base_address

    print("Scanning for AEG099_651 objects...")
    objects = scan_wgm_651(pm, base)
    print(f"Found {len(objects)} AEG099_651 objects\n")

    # Group by alive state
    alive_objs = [o for o in objects if o['f263'] & 0x02]
    dead_objs = [o for o in objects if not (o['f263'] & 0x02)]

    # Also find "just collected" = f263 alive but f269 != 0x00
    just_collected = [o for o in objects if (o['f263'] & 0x02) and o['f269'] != 0x00]

    print(f"Alive (f263 bit1=1, f269=0x00): {len([o for o in objects if (o['f263']&0x02) and o['f269']==0x00])}")
    print(f"Alive but f269!=0: {len(just_collected)}")
    for o in just_collected:
        print(f"  {o['name']} block=0x{o['block_id']:08X} f263=0x{o['f263']:02X} f269=0x{o['f269']:02X}")
    print(f"Dead (f263 bit1=0): {len(dead_objs)}")

    # Pick one alive and one "just collected" (or dead) for byte comparison
    ref_alive = next((o for o in objects if (o['f263'] & 0x02) and o['f269'] == 0x00), None)
    ref_collected = next((o for o in just_collected), None)
    if not ref_collected:
        ref_collected = next((o for o in dead_objs), None)

    if not ref_alive or not ref_collected:
        print("Need at least one alive and one collected 651 object!")
        return

    print(f"\nComparing:")
    print(f"  ALIVE:     {ref_alive['name']} @ 0x{ref_alive['addr']:X} block=0x{ref_alive['block_id']:08X}")
    print(f"  COLLECTED: {ref_collected['name']} @ 0x{ref_collected['addr']:X} block=0x{ref_collected['block_id']:08X}")

    DUMP_SIZE = 0x300
    alive_bytes = safe_read(pm, ref_alive['addr'], DUMP_SIZE)
    coll_bytes = safe_read(pm, ref_collected['addr'], DUMP_SIZE)

    if not alive_bytes or not coll_bytes:
        print("Failed to read memory!")
        return

    print(f"\nByte differences (first 0x{DUMP_SIZE:X} bytes):")
    print(f"{'Offset':>8s}  {'ALIVE':>5s}  {'COLL':>5s}  Notes")
    print("-" * 50)

    diffs = []
    for i in range(DUMP_SIZE):
        if alive_bytes[i] != coll_bytes[i]:
            notes = ""
            if i == 0x263: notes = " <-- f263 (persistent alive flag)"
            elif i == 0x269: notes = " <-- f269 (immediate pickup flag)"
            elif i == 0x1D8: notes = " <-- +0x1D8 (processing state, UNRELIABLE)"
            diffs.append(i)
            print(f"  +0x{i:03X}:  0x{alive_bytes[i]:02X}   0x{coll_bytes[i]:02X}{notes}")

    print(f"\nTotal differences: {len(diffs)} bytes")

    # Focus on range 0x260-0x270
    print(f"\nDetailed view 0x258-0x278:")
    for obj_name, data in [("ALIVE", alive_bytes), ("COLL", coll_bytes)]:
        print(f"  {obj_name}:")
        for off in range(0x258, 0x278, 16):
            chunk = data[off:off+16]
            hex_str = ' '.join(f'{b:02X}' for b in chunk)
            print(f"    +0x{off:03X}: {hex_str}")

if __name__ == "__main__":
    main()
