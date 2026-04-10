#!/usr/bin/env python3
"""Check GEOF entries for a specific tile. Usage: python check_tile_geof.py m60_39_40_00"""
import pymem, struct, sys

RVA_GEOM_FLAG = 0x3D69D18
RVA_GEOM_NONACTIVE = 0x3D69D98
GEOM_IDX_MIN = 0x1194

def safe_read(pm, addr, size):
    try: return pm.read_bytes(addr, size)
    except: return None

def read_ptr(pm, addr):
    d = safe_read(pm, addr, 8)
    return struct.unpack('<Q', d)[0] if d else None

def is_valid_ptr(v):
    return v is not None and 0x10000 < v < 0x7FFFFFFFFFFF

def parse_tile(name):
    p = name.split('_')
    return (int(p[0][1:])<<24)|(int(p[1])<<16)|(int(p[2])<<8)|int(p[3])

def scan_singleton(pm, base, rva, name, target_tid):
    ptr = read_ptr(pm, base + rva)
    if not is_valid_ptr(ptr):
        print(f"  {name}: NULL")
        return

    raw = safe_read(pm, ptr + 0x08, 0x40000)
    if not raw:
        print(f"  {name}: cannot read table")
        return

    found_tile = False
    consecutive_empty = 0
    for off in range(0, len(raw) - 16 + 1, 16):
        id_val = struct.unpack('<Q', raw[off:off+8])[0]
        ptr_val = struct.unpack('<Q', raw[off+8:off+16])[0]

        if id_val == 0 and ptr_val == 0:
            consecutive_empty += 1
            if consecutive_empty > 256: break
            continue
        consecutive_empty = 0

        tid = id_val & 0xFFFFFFFF
        if tid != target_tid:
            continue

        found_tile = True
        print(f"\n  {name}: tile 0x{tid:08X} found, data_ptr=0x{ptr_val:016X}")

        # Read entries
        hdr = safe_read(pm, ptr_val, 32)
        if not hdr: continue

        countA = struct.unpack('<I', hdr[8:12])[0]
        countB = struct.unpack('<I', hdr[0:4])[0]
        count, estart = 0, 0
        if 0 < countA < 100000:
            count, estart = countA, ptr_val + 16
        elif 0 < countB < 100000:
            count, estart = countB, ptr_val + 8

        print(f"  Total entries on tile: {count}")
        edata = safe_read(pm, estart, count * 8)
        if not edata: continue

        aeg099_entries = []
        for ei in range(count):
            eo = ei * 8
            if eo + 8 > len(edata): break
            flags = edata[eo + 1]
            geom_idx = edata[eo + 2] | (edata[eo + 3] << 8)

            if geom_idx >= GEOM_IDX_MIN and flags in (0x00, 0x80):
                aeg_idx = (geom_idx - GEOM_IDX_MIN) * 2 + (1 if flags & 0x80 else 0)
                aeg099_entries.append((geom_idx, flags, aeg_idx, edata[eo:eo+8].hex()))

        print(f"  AEG099-range entries (geom >= 0x{GEOM_IDX_MIN:04X}): {len(aeg099_entries)}")
        for gidx, fl, aidx, raw_hex in aeg099_entries:
            print(f"    geom=0x{gidx:04X} flags=0x{fl:02X} aeg099_idx={aidx:3d}  raw={raw_hex}")

    if not found_tile:
        print(f"  {name}: tile 0x{target_tid:08X} NOT in table")

def main():
    tile = sys.argv[1]
    tid = parse_tile(tile)
    print(f"Checking GEOF for {tile} (0x{tid:08X})")

    pm = pymem.Pymem("eldenring.exe")
    base = pm.base_address

    scan_singleton(pm, base, RVA_GEOM_FLAG, "GeomFlagSaveDataManager", tid)
    scan_singleton(pm, base, RVA_GEOM_NONACTIVE, "GeomNonActiveBlockManager", tid)

if __name__ == "__main__":
    main()
