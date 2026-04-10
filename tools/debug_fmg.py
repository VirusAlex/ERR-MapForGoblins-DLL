import struct
import config

data = open(str(config.DATA_DIR / "PlaceName_dlc01.fmg"), "rb").read()
print(f"File size: {len(data)}")
print("Header hex:")
for row in range(0, 0x28, 16):
    hexstr = " ".join(f"{b:02X}" for b in data[row:row+16])
    print(f"  {row:04X}: {hexstr}")

group_count = struct.unpack_from("<i", data, 0x0C)[0]
entry_count = struct.unpack_from("<i", data, 0x10)[0]
str_off_table = struct.unpack_from("<q", data, 0x18)[0]
print(f"Groups: {group_count}, Entries: {entry_count}, StrOffTable: 0x{str_off_table:X}")
print(f"Expected groups end: 0x{0x28 + group_count * 0x18:X}")
print(f"Expected str_off end: 0x{str_off_table + entry_count * 8:X}")

total = 0
for g in range(group_count):
    off = 0x28 + g * 0x18
    idx_start = struct.unpack_from("<i", data, off)[0]
    id_start = struct.unpack_from("<i", data, off + 4)[0]
    id_end = struct.unpack_from("<i", data, off + 8)[0]
    cnt = id_end - id_start + 1
    total += cnt
    if g < 5 or g >= group_count - 5:
        print(f"  Group {g}: idx_start={idx_start}, id_range=[{id_start}, {id_end}], count={cnt}")

print(f"Total entries from groups: {total}")
