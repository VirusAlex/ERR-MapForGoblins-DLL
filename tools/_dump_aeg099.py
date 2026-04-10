#!/usr/bin/env python3
"""
Dump ALL AEG099_* objects on a tile to understand geom_idx slot ordering.
Usage: python dump_aeg099.py <tile_name>
"""
import pymem, struct, sys

RVA_WORLD_GEOM_MAN = 0x3D69BA8

def safe_read(pm, addr, size):
    try: return pm.read_bytes(addr, size)
    except: return None

def read_ptr(pm, addr):
    d = safe_read(pm, addr, 8)
    return struct.unpack('<Q', d)[0] if d else None

def read_u8(pm, addr):
    d = safe_read(pm, addr, 1)
    return d[0] if d else None

def read_u32(pm, addr):
    d = safe_read(pm, addr, 4)
    return struct.unpack('<I', d)[0] if d else None

def is_valid_ptr(v):
    return v is not None and 0x10000 < v < 0x7FFFFFFFFFFF

def parse_tile(name):
    p = name.split('_')
    return (int(p[0][1:])<<24)|(int(p[1])<<16)|(int(p[2])<<8)|int(p[3])

def main():
    tile = sys.argv[1]
    tid = parse_tile(tile)
    pm = pymem.Pymem("eldenring.exe")
    base = pm.base_address

    wgm = read_ptr(pm, base + RVA_WORLD_GEOM_MAN)
    head = read_ptr(pm, wgm + 0x18 + 0x08)

    def nil(n): v = read_u8(pm, n+0x19); return v is None or v != 0
    def left(n): return read_ptr(pm, n)
    def right(n): return read_ptr(pm, n+0x10)
    def parent(n): return read_ptr(pm, n+0x08)
    def minn(n):
        while n and not nil(n):
            l = left(n)
            if not l or nil(l): break
            n = l
        return n

    cur = minn(parent(head))
    while cur and cur != head and not nil(cur):
        bid = read_u32(pm, cur + 0x20)
        bdata = read_ptr(pm, cur + 0x28)

        if bid == tid and is_valid_ptr(bdata):
            vb = read_ptr(pm, bdata + 0x288 + 0x08)
            ve = read_ptr(pm, bdata + 0x288 + 0x10)
            if not vb or not ve or ve <= vb: break
            count = (ve - vb) // 8

            print(f"{tile}: {count} total GeomIns")
            print(f"\nAll AEG099_* objects (in vector order = MSB order):")
            print(f"{'vec_idx':>7} {'aeg099_idx':>10} {'name':35s} {'0x263_bit1':>10}")
            print("-" * 70)

            aeg099_count = 0
            for i in range(min(count, 50000)):
                gi = read_ptr(pm, vb + i * 8)
                if not gi or not is_valid_ptr(gi): continue
                mp = read_ptr(pm, gi + 0x18 + 0x18 + 0x18)
                if not mp or not is_valid_ptr(mp): continue
                np = read_ptr(pm, mp)
                if not np or not is_valid_ptr(np): continue
                nr = safe_read(pm, np, 128)
                if not nr: continue
                try: name = nr.decode('utf-16-le').split('\x00')[0]
                except: continue

                if name.startswith('AEG099_'):
                    b263 = read_u8(pm, gi + 0x263) or 0
                    flag = "alive" if (b263 & 0x02) else "COLLECTED"
                    marker = " <<<" if "821" in name else ""

                    # What GEOF slot would this be?
                    geom_idx = 0x1194 + aeg099_count // 2
                    flags_val = 0x80 if (aeg099_count % 2) else 0x00

                    print(f"{i:7d} {aeg099_count:10d} {name:35s} {flag:>10s}  "
                          f"(geom=0x{geom_idx:04X} flags=0x{flags_val:02X}){marker}")
                    aeg099_count += 1

            print(f"\nTotal AEG099_* objects: {aeg099_count}")
            break

        r = right(cur)
        if r and not nil(r): cur = minn(r)
        else:
            p = parent(cur)
            while p and p != head:
                if cur != right(p): break
                cur = p; p = parent(cur)
            cur = p
    else:
        print(f"{tile} not in WGM")

if __name__ == "__main__":
    main()
