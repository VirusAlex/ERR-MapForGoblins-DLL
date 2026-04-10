"""Convert rune_pieces.json to a compact binary format for the DLL."""
import json, struct
from pathlib import Path
import config

INPUT  = config.DATA_DIR / 'rune_pieces.json'
OUTPUT = config.DATA_DIR / 'pieces.bin'

with open(INPUT) as f:
    pieces = json.load(f)

def tile_str_to_id(tile: str) -> int:
    """m60_34_45_00 -> 0x3C222D00"""
    parts = tile.replace('m', '').split('_')
    aa, bb, cc, dd = int(parts[0]), int(parts[1]), int(parts[2]), int(parts[3])
    return (aa << 24) | (bb << 16) | (cc << 8) | dd

# Binary format:
# Header: "RPDB" (4 bytes) + count (uint32) = 8 bytes
# Records: [x:f32, y:f32, z:f32, tile_id:u32] = 16 bytes each

with open(OUTPUT, 'wb') as f:
    f.write(b'RPDB')
    f.write(struct.pack('<I', len(pieces)))

    for p in pieces:
        tid = tile_str_to_id(p['map'])
        f.write(struct.pack('<fffI', p['x'], p['y'], p['z'], tid))

print(f"Written {len(pieces)} pieces to {OUTPUT}")
print(f"File size: {OUTPUT.stat().st_size} bytes ({OUTPUT.stat().st_size // 1024} KB)")

# Verify round-trip
with open(OUTPUT, 'rb') as f:
    magic = f.read(4)
    count = struct.unpack('<I', f.read(4))[0]
    print(f"Verify: magic={magic}, count={count}")
    rec = struct.unpack('<fffI', f.read(16))
    print(f"First record: x={rec[0]:.3f} y={rec[1]:.3f} z={rec[2]:.3f} tile=0x{rec[3]:08X}")
