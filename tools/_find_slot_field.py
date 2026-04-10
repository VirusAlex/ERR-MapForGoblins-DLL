#!/usr/bin/env python3
"""
Read candidate fields from AEG099_821 GeomIns to find which one = GEOF slot.
Tests on m60_46_38_00 where we know: _9003=slot0, _9002=slot1, _9001=slot2, _9000=slot3
Usage: python find_slot_field.py m60_46_38_00
"""
import pymem, struct, sys

RVA_WGM = 0x3D69BA8

def sr(pm, a, s):
    try: return pm.read_bytes(a, s)
    except: return None

def rp(pm, a):
    d = sr(pm, a, 8)
    return struct.unpack('<Q', d)[0] if d else None

def r32(pm, a):
    d = sr(pm, a, 4)
    return struct.unpack('<I', d)[0] if d else None

def r8(pm, a):
    d = sr(pm, a, 1)
    return d[0] if d else None

def vp(v): return v is not None and 0x10000 < v < 0x7FFFFFFFFFFF

def pt(n):
    p = n.split('_')
    return (int(p[0][1:])<<24)|(int(p[1])<<16)|(int(p[2])<<8)|int(p[3])

tile = sys.argv[1]
tid = pt(tile)
pm = pymem.Pymem("eldenring.exe")
base = pm.base_address
wgm = rp(pm, base + RVA_WGM)
head = rp(pm, wgm + 0x18 + 0x08)

def nil(n): v=r8(pm,n+0x19); return v is None or v!=0
def minn(n):
    while n and not nil(n):
        l=rp(pm,n)
        if not l or nil(l): break
        n=l
    return n

cur = minn(rp(pm, head+0x08))
while cur and cur!=head and not nil(cur):
    bid = r32(pm, cur+0x20)
    bd = rp(pm, cur+0x28)
    if bid == tid and vp(bd):
        vb = rp(pm, bd+0x288+0x08)
        ve = rp(pm, bd+0x288+0x10)
        if vb and ve and ve > vb:
            cnt = min((ve-vb)//8, 50000)
            pieces = []
            for i in range(cnt):
                gi = rp(pm, vb+i*8)
                if not gi or not vp(gi): continue
                mp = rp(pm, gi+0x48)
                if not mp or not vp(mp): continue
                np = rp(pm, mp)
                if not np or not vp(np): continue
                nr = sr(pm, np, 128)
                if not nr: continue
                try: name = nr.decode('utf-16-le').split('\x00')[0]
                except: continue
                if 'AEG099_821' in name:
                    # Read many candidate u32 fields
                    fields = {}
                    for off in range(0, 0x100, 4):
                        v = r32(pm, gi + off)
                        if v is not None:
                            fields[off] = v
                    # Also read +0x80 area specifically with u16
                    d84 = sr(pm, gi + 0x84, 2)
                    fields['0x84_u16'] = struct.unpack('<H', d84)[0] if d84 else None
                    pieces.append((i, name, gi, fields))

            print(f"{tile}: {len(pieces)} AEG099_821 pieces")
            print(f"Looking for field where values = 0,1,2,3... (the GEOF slot)")
            print()

            # For each field offset, show values across all pieces
            # Highlight fields that give sequential small numbers
            all_offsets = sorted(set(o for _,_,_,f in pieces for o in f if isinstance(o, int)))

            for off in all_offsets:
                vals = [f.get(off) for _,_,_,f in pieces]
                if all(v is not None and v < 100 for v in vals):
                    names = [n for _,n,_,_ in pieces]
                    val_str = ', '.join(f'{n}={v}' for n, v in zip(names, vals))
                    print(f"  +0x{off:03X}: {val_str}")
        break
    r = rp(pm, cur+0x10)
    if r and not nil(r): cur = minn(r)
    else:
        p = rp(pm, cur+0x08)
        while p and p!=head:
            if cur != rp(pm, p+0x10): break
            cur=p; p=rp(pm, cur+0x08)
        cur=p
