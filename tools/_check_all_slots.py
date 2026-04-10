#!/usr/bin/env python3
"""
Check GEOF slot assignments for ALL known rune piece tiles.
Shows which tiles are in GEOF, their 821-hash entries, and slot numbers.
Also shows which tiles are in WGM (loaded) and the vector order of pieces.
"""
import pymem, struct, sys, json, os

RVA_GEOM_FLAG = 0x3D69D18
RVA_GEOM_NONACTIVE = 0x3D69D98
RVA_WORLD_GEOM_MAN = 0x3D69BA8
MODEL_HASH_821 = 0x009A1C6D

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

def tile_name(tid):
    return f"m{(tid>>24)&0xFF:02d}_{(tid>>16)&0xFF:02d}_{(tid>>8)&0xFF:02d}_{tid&0xFF:02d}"

def main():
    pm = pymem.Pymem("eldenring.exe")
    base = pm.base_address

    # Load rune pieces data
    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
              'MapForGoblins', 'data', 'rune_pieces.json')) as f:
        pieces = json.load(f)

    # Build tile -> piece names (ordered by JSON position)
    tile_pieces = {}
    for p in pieces:
        m = p['map']
        parts = m.split('_')
        tid = (int(parts[0][1:])<<24)|(int(parts[1])<<16)|(int(parts[2])<<8)|int(parts[3])
        if tid not in tile_pieces:
            tile_pieces[tid] = []
        tile_pieces[tid].append(p['name'])

    # Read GEOF entries filtered by model hash
    geof_data = {}  # tid -> list of (slot, geom_idx, flags)
    for rva in [RVA_GEOM_FLAG, RVA_GEOM_NONACTIVE]:
        ptr = read_ptr(pm, base + rva)
        if not is_valid_ptr(ptr): continue
        raw = safe_read(pm, ptr + 0x08, 0x40000)
        if not raw: continue

        ce = 0
        for off in range(0, len(raw)-16+1, 16):
            idv = struct.unpack('<Q', raw[off:off+8])[0]
            pv = struct.unpack('<Q', raw[off+8:off+16])[0]
            if idv == 0 and pv == 0:
                ce += 1
                if ce > 256: break
                continue
            ce = 0
            tid = idv & 0xFFFFFFFF
            if tid not in tile_pieces: continue
            if not is_valid_ptr(pv): continue

            hdr = safe_read(pm, pv, 32)
            if not hdr: continue
            cA = struct.unpack('<I', hdr[8:12])[0]
            cB = struct.unpack('<I', hdr[0:4])[0]
            count, es = 0, 0
            if 0 < cA < 100000: count, es = cA, pv+16
            elif 0 < cB < 100000: count, es = cB, pv+8
            if count == 0: continue

            edata = safe_read(pm, es, count*8)
            if not edata: continue

            for ei in range(count):
                eo = ei*8
                if eo+8 > len(edata): break
                fl = edata[eo+1]
                gi = edata[eo+2]|(edata[eo+3]<<8)
                mh = edata[eo+4]|(edata[eo+5]<<8)|(edata[eo+6]<<16)|(edata[eo+7]<<24)
                if mh == MODEL_HASH_821 and fl in (0x00, 0x80):
                    slot = (gi - 0x1194)*2 + (1 if fl & 0x80 else 0)
                    if tid not in geof_data: geof_data[tid] = []
                    geof_data[tid].append((slot, gi, fl))

    # Read WGM to find loaded tiles + piece vector order
    wgm_data = {}  # tid -> list of (vec_idx, name, alive)
    wgm = read_ptr(pm, base + RVA_WORLD_GEOM_MAN)
    if is_valid_ptr(wgm):
        head = read_ptr(pm, wgm + 0x18 + 0x08)
        if is_valid_ptr(head):
            def nil(n): v=read_u8(pm,n+0x19); return v is None or v!=0
            def minn(n):
                while n and not nil(n):
                    l=read_ptr(pm,n)
                    if not l or nil(l): break
                    n=l
                return n
            cur = minn(read_ptr(pm, head+0x08))
            while cur and cur!=head and not nil(cur):
                bid = read_u32(pm, cur+0x20)
                bd = read_ptr(pm, cur+0x28)
                if bid and is_valid_ptr(bd) and bid in tile_pieces:
                    vb = read_ptr(pm, bd+0x288+0x08)
                    ve = read_ptr(pm, bd+0x288+0x10)
                    if vb and ve and ve > vb:
                        cnt = min((ve-vb)//8, 50000)
                        for i in range(cnt):
                            gi = read_ptr(pm, vb+i*8)
                            if not gi or not is_valid_ptr(gi): continue
                            mp = read_ptr(pm, gi+0x48)
                            if not mp or not is_valid_ptr(mp): continue
                            np = read_ptr(pm, mp)
                            if not np or not is_valid_ptr(np): continue
                            nr = safe_read(pm, np, 128)
                            if not nr: continue
                            try: name = nr.decode('utf-16-le').split('\x00')[0]
                            except: continue
                            if name.startswith('AEG099_821'):
                                b263 = read_u8(pm, gi+0x263) or 0
                                alive = bool(b263 & 0x02)
                                if bid not in wgm_data: wgm_data[bid] = []
                                wgm_data[bid].append((i, name, alive))
                r = read_ptr(pm, cur+0x10)
                if r and not nil(r): cur = minn(r)
                else:
                    p = read_ptr(pm, cur+0x08)
                    while p and p!=head:
                        if cur != read_ptr(pm, p+0x10): break
                        cur=p; p=read_ptr(pm, cur+0x08)
                    cur=p

    # Report
    print(f"{'Tile':<20} {'Pieces':>6} {'Source':<6} {'Details'}")
    print("-"*80)

    for tid in sorted(tile_pieces.keys()):
        tname = tile_name(tid)
        pnames = tile_pieces[tid]
        n = len(pnames)

        if tid in wgm_data:
            items = wgm_data[tid]
            detail_parts = []
            for vec_idx, name, alive in items:
                status = "alive" if alive else "COLL"
                detail_parts.append(f"{name}[{vec_idx}]={status}")
            print(f"{tname:<20} {n:>6} {'WGM':<6} {', '.join(detail_parts)}")
        elif tid in geof_data:
            slots = geof_data[tid]
            slots_sorted = sorted(slots, key=lambda x: x[0])
            detail_parts = []
            for slot, gi, fl in slots_sorted:
                detail_parts.append(f"slot={slot}(0x{gi:04X}/{fl:02X})")
            print(f"{tname:<20} {n:>6} {'GEOF':<6} {len(slots)} 821-entries: {', '.join(detail_parts)}")
        else:
            print(f"{tname:<20} {n:>6} {'---':<6} not in GEOF or WGM")

if __name__ == "__main__":
    main()
