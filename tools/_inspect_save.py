"""Inspect Elden Ring save file structure."""
import struct, sys

if len(sys.argv) != 2:
    print("Usage: _inspect_save.py <save_file.err>")
    sys.exit(1)

PATH = sys.argv[1]

with open(PATH, 'rb') as f:
    data = f.read()

print(f"File size: {len(data)} bytes ({len(data)/1024/1024:.1f} MB)")
print(f"Magic: {data[:4]}")
print()

# Hex dump of first 256 bytes
print("Header (first 256 bytes):")
for i in range(0, 256, 16):
    hex_part = ' '.join(f'{data[i+j]:02X}' for j in range(16))
    ascii_part = ''.join(chr(data[i+j]) if 32 <= data[i+j] < 127 else '.' for j in range(16))
    print(f'  {i:08X}: {hex_part}  {ascii_part}')

# SL2/ERR format: BND4 container with save slots
# Check for BND4 header
if data[:4] == b'BND4':
    print("\nBND4 archive detected!")
    file_count = struct.unpack_from('<I', data, 0x0C)[0]
    print(f"File count: {file_count}")
    header_size = struct.unpack_from('<Q', data, 0x10)[0]
    print(f"Header size: {header_size}")

# Look for save slot markers (usually "USER_DATA" or slot headers)
# Search for known patterns
patterns = [b'USER_DATA', b'BND4', b'\x00\x00\x00\x01', b'REGU']
for pat in [b'USER_DATA', b'SL2_', b'SAVE']:
    idx = data.find(pat)
    if idx >= 0:
        print(f"\nFound '{pat}' at offset 0x{idx:08X}")

# Find all BND4 markers
print("\nBND4 markers:")
pos = 0
while True:
    pos = data.find(b'BND4', pos)
    if pos < 0:
        break
    print(f"  BND4 at 0x{pos:08X}")
    pos += 4

# Look for slot boundaries by finding repeating large structures
print("\nLooking for repeating 2.8MB patterns (save slots)...")
slot_size_guess = len(data) // 10  # 10 slots
print(f"  Guessed slot size: {slot_size_guess} bytes ({slot_size_guess/1024/1024:.2f} MB)")

# Check if file has identical regions (backup)
half = len(data) // 2
first_half_hash = hash(data[:half])
second_half_hash = hash(data[half:])
print(f"  First half hash: {first_half_hash}")
print(f"  Second half hash: {second_half_hash}")
print(f"  Halves identical: {data[:half] == data[half:]}")

# Scan for regions that look like flag bitmaps
# (should be mostly 0x00 or 0xFF bytes)
print("\nRegion analysis (1KB blocks):")
for offset in range(0, min(len(data), 0x600000), 0x10000):
    block = data[offset:offset+0x10000]
    zero_pct = sum(1 for b in block if b == 0) / len(block) * 100
    ff_pct = sum(1 for b in block if b == 0xFF) / len(block) * 100
    if zero_pct > 90 or ff_pct > 50:
        print(f"  0x{offset:08X}: {zero_pct:.0f}% zeros, {ff_pct:.0f}% FFs")
