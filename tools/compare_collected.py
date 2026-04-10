#!/usr/bin/env python3
"""Compare DLL memory singletons vs save file GEOF. Run while game is loaded."""
import pymem, struct, json, os, glob

MODEL_HASH = 0x009A1C6D

with open('MapForGoblins/data/rune_pieces.json') as f:
    pieces = json.load(f)

slot_to_name = {}
tile_pieces = {}
for p in pieces:
    parts = p['map'].split('_')
    area, gx, gz, idx = int(parts[0][1:]), int(parts[1]), int(parts[2]), int(parts[3])
    tid = (area << 24) | (gx << 16) | (gz << 8) | idx
    iid = p.get('instance_id', -1)
    slot = iid - 9000 if iid >= 9000 else -1
    slot_to_name[(tid, slot)] = p['name']
    if tid not in tile_pieces:
        tile_pieces[tid] = []
    tile_pieces[tid].append({'name': p['name'], 'slot': slot, 'map': p['map']})

def tile_name(tid):
    return f"m{(tid>>24)&0xFF:02d}_{(tid>>16)&0xFF:02d}_{(tid>>8)&0xFF:02d}_{tid&0xFF:02d}"

pm = pymem.Pymem("eldenring.exe")
base = pm.base_address

def read_ptr(addr):
    try:
        return struct.unpack('<Q', pm.read_bytes(addr, 8))[0]
    except: return None

memory_collected = set()

for rva in [0x3D69D18, 0x3D69D98]:
    ptr = read_ptr(base + rva)
    if not ptr or ptr < 0x10000: continue
    try:
        raw = pm.read_bytes(ptr + 0x08, 0x40000)
    except: continue
    ce = 0
    for off in range(0, len(raw)-16+1, 16):
        idv = struct.unpack('<Q', raw[off:off+8])[0]
        pv = struct.unpack('<Q', raw[off+8:off+16])[0]
        if idv == 0 and pv == 0:
            ce += 1;
            if ce > 256: break
            continue
        ce = 0
        tid = idv & 0xFFFFFFFF
        if pv < 0x10000 or pv > 0x7FFFFFFFFFFF: continue
        try:
            hdr = pm.read_bytes(pv, 32)
        except: continue
        cA = struct.unpack('<I', hdr[8:12])[0]
        cB = struct.unpack('<I', hdr[0:4])[0]
        count, es = 0, 0
        if 0 < cA < 100000: count, es = cA, pv+16
        elif 0 < cB < 100000: count, es = cB, pv+8
        if not count: continue
        try:
            edata = pm.read_bytes(es, count*8)
        except: continue
        for ei in range(count):
            eo = ei*8
            if eo+8 > len(edata): break
            fl = edata[eo+1]
            gi = edata[eo+2]|(edata[eo+3]<<8)
            mh = edata[eo+4]|(edata[eo+5]<<8)|(edata[eo+6]<<16)|(edata[eo+7]<<24)
            if mh == MODEL_HASH and fl in (0x00, 0x80):
                slot = (gi - 0x1194)*2 + (1 if fl & 0x80 else 0)
                memory_collected.add((tid, slot))

print(f"Memory GEOF: {len(memory_collected)} 821-entries across singletons")

save_dir = os.path.expandvars(r"%APPDATA%\EldenRing")
save_files = []
for root, dirs, files in os.walk(save_dir):
    for f in files:
        if f.endswith('.err') or f.endswith('.sl2'):
            save_files.append(os.path.join(root, f))

save_collected = set()
save_path = None
for sf in sorted(save_files, key=os.path.getmtime, reverse=True):
    if 'ER0000' in sf:
        save_path = sf
        break

if save_path:
    print(f"Save file: {save_path}")
    with open(save_path, 'rb') as f:
        data = f.read()

    magic = b'FOEG'
    pos = 0
    while True:
        idx = data.find(magic, pos)
        if idx == -1: break
        if idx < 4:
            pos = idx + 4; continue
        total_size = struct.unpack_from('<i', data, idx - 4)[0]
        if total_size <= 12 or total_size > 0x100000:
            pos = idx + 4; continue

        sec_start = idx - 4
        chunk_pos = sec_start + 12
        sec_end = sec_start + total_size

        while chunk_pos + 16 <= sec_end and chunk_pos + 16 <= len(data):
            if data[chunk_pos:chunk_pos+4] == b'\xff\xff\xff\xff':
                break
            entry_size = struct.unpack_from('<i', data, chunk_pos + 4)[0]
            if entry_size <= 0 or entry_size > 0x100000: break

            tid_bytes = struct.unpack_from('<I', data, chunk_pos)[0]
            count = struct.unpack_from('<I', data, chunk_pos + 8)[0]

            for ei in range(count):
                eoff = chunk_pos + 16 + ei * 8
                if eoff + 8 > len(data): break
                fl = data[eoff + 1]
                gi = data[eoff + 2] | (data[eoff + 3] << 8)
                mh = data[eoff + 4]|(data[eoff + 5]<<8)|(data[eoff + 6]<<16)|(data[eoff + 7]<<24)
                if mh == MODEL_HASH and fl in (0x00, 0x80):
                    slot = (gi - 0x1194)*2 + (1 if fl & 0x80 else 0)
                    save_collected.add((tid_bytes, slot))

            chunk_pos += entry_size
        pos = idx + 4

    print(f"Save file GEOF: {len(save_collected)} 821-entries")
else:
    print("Save file not found!")

def entry_to_name(tid, slot):
    name = slot_to_name.get((tid, slot))
    if name: return f"{tile_name(tid)} {name} (slot={slot})"
    return f"{tile_name(tid)} UNKNOWN (slot={slot})"

in_save_not_memory = save_collected - memory_collected
in_memory_not_save = memory_collected - save_collected

print(f"\n=== In SAVE but NOT in memory singletons: {len(in_save_not_memory)} ===")
for tid, slot in sorted(in_save_not_memory):
    print(f"  {entry_to_name(tid, slot)}")

print(f"\n=== In memory but NOT in save: {len(in_memory_not_save)} ===")
for tid, slot in sorted(in_memory_not_save):
    print(f"  {entry_to_name(tid, slot)}")

matched = sum(1 for (t,s) in save_collected if (t,s) in slot_to_name)
unmatched = len(save_collected) - matched
print(f"\nSave entries matching our pieces: {matched}")
print(f"Save entries NOT matching (unknown slot): {unmatched}")
