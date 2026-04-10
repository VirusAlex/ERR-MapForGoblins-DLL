import struct, sys
from pathlib import Path
import config

fmg_path = str(config.TOOLS_DIR.parent / "release" / "PlaceName_dlc01.fmg")
data = open(fmg_path, "rb").read()

entry_count = struct.unpack_from("<i", data, 0x10)[0]
str_off_table = struct.unpack_from("<q", data, 0x18)[0]
group_count = struct.unpack_from("<i", data, 0x0C)[0]
groups_off = 0x28

target_ids = {10500002, 10500000, 10500001, 10500003, 10500004, 10500005}

for g in range(group_count):
    off = groups_off + g * 0x18
    idx_start = struct.unpack_from("<i", data, off)[0]
    id_start = struct.unpack_from("<i", data, off + 4)[0]
    id_end = struct.unpack_from("<i", data, off + 8)[0]
    for i in range(id_end - id_start + 1):
        entry_id = id_start + i
        if entry_id in target_ids:
            entry_idx = idx_start + i
            str_off = struct.unpack_from("<q", data, str_off_table + entry_idx * 8)[0]
            if str_off > 0 and str_off < len(data):
                s = b""
                pos = str_off
                while pos < len(data) - 1:
                    c = data[pos : pos + 2]
                    if c == b"\x00\x00":
                        break
                    s += c
                    pos += 2
                text = s.decode("utf-16-le", errors="replace")
            else:
                text = "<null>"
            print(f"ID {entry_id}: \"{text}\"")
