"""Add new entries to PlaceName_dlc01.fmg (FMG v2, 16-byte groups)."""
import struct
import config

GROUP_SIZE = 16  # 4 ints: first_index, first_id, last_id, pad

def read_fmg(path):
    data = open(path, "rb").read()
    group_count = struct.unpack_from("<i", data, 0x0C)[0]
    entry_count = struct.unpack_from("<i", data, 0x10)[0]
    str_off_table = struct.unpack_from("<I", data, 0x18)[0]

    entries = []
    for g in range(group_count):
        off = 0x28 + g * GROUP_SIZE
        idx_start = struct.unpack_from("<i", data, off)[0]
        id_start = struct.unpack_from("<i", data, off + 4)[0]
        id_end = struct.unpack_from("<i", data, off + 8)[0]
        for i in range(id_end - id_start + 1):
            entry_id = id_start + i
            entry_idx = idx_start + i
            soff = str_off_table + entry_idx * 8
            if soff + 8 > len(data):
                entries.append((entry_id, None))
                continue
            str_off = struct.unpack_from("<q", data, soff)[0]
            text = None
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
            entries.append((entry_id, text))
    return entries, data[:0x28]  # return header bytes too

def build_fmg(entries, orig_header):
    entries.sort(key=lambda e: e[0])
    groups = []
    if entries:
        g_start_id = entries[0][0]
        g_start_idx = 0
        for i in range(1, len(entries)):
            if entries[i][0] != entries[i - 1][0] + 1:
                groups.append((g_start_idx, g_start_id, entries[i - 1][0]))
                g_start_id = entries[i][0]
                g_start_idx = i
        groups.append((g_start_idx, g_start_id, entries[-1][0]))

    entry_count = len(entries)
    group_count = len(groups)

    header_sz = 0x28
    groups_sz = group_count * GROUP_SIZE
    str_off_table_off = header_sz + groups_sz
    strings_start = str_off_table_off + entry_count * 8

    string_data = bytearray()
    str_offsets = []
    for entry_id, text in entries:
        if text is None:
            str_offsets.append(0)
        else:
            str_offsets.append(strings_start + len(string_data))
            string_data += text.encode("utf-16-le") + b"\x00\x00"

    total_size = strings_start + len(string_data)
    out = bytearray(total_size)

    out[:0x28] = orig_header[:0x28]
    struct.pack_into("<I", out, 0x04, total_size)
    struct.pack_into("<i", out, 0x0C, group_count)
    struct.pack_into("<i", out, 0x10, entry_count)
    struct.pack_into("<q", out, 0x18, str_off_table_off)

    for gi, (idx_start, id_start, id_end) in enumerate(groups):
        off = header_sz + gi * GROUP_SIZE
        struct.pack_into("<i", out, off, idx_start)
        struct.pack_into("<i", out, off + 4, id_start)
        struct.pack_into("<i", out, off + 8, id_end)
        struct.pack_into("<i", out, off + 12, 0)

    for i, soff in enumerate(str_offsets):
        struct.pack_into("<q", out, str_off_table_off + i * 8, soff)

    out[strings_start : strings_start + len(string_data)] = string_data

    return bytes(out)


fmg_path = str(config.DATA_DIR / "PlaceName_dlc01.fmg")
entries, orig_header = read_fmg(fmg_path)
print(f"Read {len(entries)} entries")

existing_ids = {e[0] for e in entries}

new_entries = [
    (10600001, "Rune Piece"),
    (10600002, "Ember Piece"),
]

added = 0
for eid, text in new_entries:
    if eid in existing_ids:
        print(f"  ID {eid} already exists, skipping")
    else:
        entries.append((eid, text))
        print(f"  Added ID {eid}: \"{text}\"")
        added += 1

print(f"Added {added} entries, total now: {len(entries)}")

new_data = build_fmg(entries, orig_header)
open(fmg_path, "wb").write(new_data)
print(f"Written {len(new_data)} bytes to {fmg_path}")

verify_entries, _ = read_fmg(fmg_path)
for eid, text in new_entries:
    found = [e for e in verify_entries if e[0] == eid]
    if found:
        print(f"  Verified ID {eid}: \"{found[0][1]}\"")
    else:
        print(f"  ERROR: ID {eid} not found after write!")

known = [e for e in verify_entries if e[0] == 9000000]
if known:
    print(f"  Sanity check ID 9000000: \"{known[0][1]}\"")
