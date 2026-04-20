#!/usr/bin/env python3
"""
Extract map marker (beacon/stamp) coordinates from an Elden Ring save file
and find nearby MASSEDIT entries.

Usage:
    py extract_markers.py <save_file.err> [--slot N] [--radius R] [--category CAT]

Examples:
    py extract_markers.py my_save.err
    py extract_markers.py my_save.err --slot 2 --radius 400
    py extract_markers.py my_save.err --slot 0 --category "Reforged - Rune Pieces"
"""

import argparse
import json
import re
import struct
import sys
from collections import defaultdict
from pathlib import Path

# Marker entry: 16 bytes = {int32 idx, float x, float z, uint16 type, uint16 pad}
MARKER_SIZE = 16

# Type classification (high byte):
#   0x01xx = beacon (blue beam)
#   0x06xx = icon marker
#   0x09xx = stamp marker
# Low byte = icon variant (0x00, 0x01, 0x0A, etc.)
BEACON_TYPES = {0x01}  # high byte
# Stamps use various high bytes (0x02-0x0A) for different icon variants
# We accept any non-beacon type as a stamp in the stamp area

# Empty beacon pattern: idx=-1, x=0, z=0, type=0x0100, pad=0
EMPTY_BEACON = b"\xff\xff\xff\xff\x00\x00\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00"

# Coordinate conversion: map UI coords <-> world coords
# worldX = mapX + OFFSET_X
# worldZ = -mapZ + OFFSET_Z
# Verified on 8+ graces, error < 2 units.
OFFSET_X = 7042
OFFSET_Z = 16511


def map_to_world(map_x, map_z):
    """Convert map UI coordinates to world coordinates."""
    return map_x + OFFSET_X, -map_z + OFFSET_Z


def world_to_tile(world_x, world_z):
    """Convert world coordinates to tile + local position."""
    grid_x = int(world_x // 256)
    grid_z = int(world_z // 256)
    pos_x = world_x - grid_x * 256
    pos_z = world_z - grid_z * 256
    return grid_x, grid_z, pos_x, pos_z


def _classify_type(typ):
    """Classify marker type by high byte."""
    hi = (typ >> 8) & 0xFF
    if hi in BEACON_TYPES:
        return "beacon"
    if hi >= 0x02:  # any non-zero, non-beacon type is a stamp/marker
        return "stamp"
    return None


def _parse_marker(data, off):
    """Parse a 16-byte marker entry. Returns dict or None."""
    if off + MARKER_SIZE > len(data):
        return None
    idx, x, z, typ, pad = struct.unpack_from("<i f f H H", data, off)
    # Empty slot
    if idx == -1:
        return {"index": idx, "map_x": 0.0, "map_z": 0.0, "type": typ, "empty": True}
    # Valid marker: idx >= 0, finite coords, reasonable range
    if idx < 0 or not (x == x) or not (z == z):  # NaN check
        return None
    if abs(x) > 25000 or abs(z) > 25000:
        return None
    kind = _classify_type(typ)
    if kind is None:
        return None
    return {
        "index": idx,
        "map_x": x,
        "map_z": z,
        "type": typ,
        "kind": kind,
        "empty": False,
    }


def _extract_slot_data(save_path, slot):
    """Extract slot data using SoulsFormats BND4 parser."""
    try:
        import os
        tools_dir = Path(__file__).parent
        sys.path.insert(0, str(tools_dir))
        os.environ.setdefault("PYTHONNET_RUNTIME", "coreclr")
        from pythonnet import load

        load("coreclr")
        import clr
        from System import Array, Object
        from System import Type as SysType
        from System.Reflection import Assembly, BindingFlags

        config_mod = __import__("config")
        dll_path = str(config_mod.SOULSFORMATS_DLL)
        asm = Assembly.LoadFrom(dll_path)
        clr.AddReference(dll_path)

        _str_type = SysType.GetType("System.String")
        _bnd4_read = asm.GetType("SoulsFormats.BND4").GetMethod(
            "Read",
            BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
            None,
            Array[SysType]([_str_type]),
            None,
        )
        bnd = _bnd4_read.Invoke(None, Array[Object]([str(save_path)]))
        if slot >= bnd.Files.Count:
            print(f"ERROR: Slot {slot} not found (save has {bnd.Files.Count} files)")
            return None
        return bytes(bnd.Files[slot].Bytes.ToArray())
    except Exception as e:
        print(f"SoulsFormats BND4 read failed ({e}), falling back to raw offsets")
        return _extract_slot_data_raw(save_path, slot)


def _extract_slot_data_raw(save_path, slot):
    """Fallback: extract slot data using raw file offsets."""
    SAVE_HEADER = 0x310
    SLOT_STRIDE = 0x280000 + 0x10
    SLOT_SIZE = 0x280000
    with open(save_path, "rb") as f:
        f.seek(SAVE_HEADER + slot * SLOT_STRIDE + 0x10)
        return f.read(SLOT_SIZE)


def find_markers(slot_data):
    """Find all markers in slot data.

    Save layout (from empirical analysis):
      - 10 beacon slots (5 DLC type=0x010A + 5 base type=0x0100), each 16 bytes
      - Stamp/marker area: filled entries + empty slots (idx=-1, type=0x0100)
        Types: 0x0900/0x0901 (base stamps), 0x090A (DLC stamps),
               0x0600 (icon markers), 0x080A (DLC variant),
               0x0100 (base beacons in stamp slots = legacy beacons)
      - Trailing empty slots until 0xFFFF padding
    """
    # Step 1: Find the first empty beacon to locate the array
    first_empty = slot_data.find(EMPTY_BEACON)
    if first_empty == -1:
        return _find_markers_bruteforce(slot_data)

    # Step 2: Walk backwards to find start of beacon array
    beacon_start = first_empty
    while beacon_start >= MARKER_SIZE:
        candidate = beacon_start - MARKER_SIZE
        idx = struct.unpack_from("<i", slot_data, candidate)[0]
        typ = struct.unpack_from("<H", slot_data, candidate + 12)[0]
        hi = (typ >> 8) & 0xFF
        if idx == -1 or hi == 0x01:
            beacon_start = candidate
        else:
            break

    # Step 3: Read exactly 5 beacon slots (game limit)
    beacons = []
    off = beacon_start
    for _ in range(5):
        if off + MARKER_SIZE > len(slot_data):
            break
        entry = _parse_marker(slot_data, off)
        if entry and not entry["empty"] and entry.get("kind") == "beacon":
            beacons.append(entry)
        off += MARKER_SIZE

    # Step 4: Read stamp/marker slots until we hit 0xFFFF padding
    stamps = []
    empty_run = 0
    while off < len(slot_data) - MARKER_SIZE:
        raw = slot_data[off : off + MARKER_SIZE]
        # Check for 0xFFFF padding (end of marker area)
        if struct.unpack_from("<H", raw, 12)[0] == 0xFFFF:
            break
        # Check for all-zero (shouldn't happen in marker area, but safety)
        if raw == b"\x00" * MARKER_SIZE:
            empty_run += 1
            if empty_run >= 5:
                break
            off += MARKER_SIZE
            continue
        empty_run = 0

        idx = struct.unpack_from("<i", raw, 0)[0]
        if idx == -1:
            # Empty slot — skip
            off += MARKER_SIZE
            continue

        entry = _parse_marker(slot_data, off)
        if entry and not entry["empty"]:
            # Everything in the stamp area is a stamp, regardless of type byte
            entry["kind"] = "stamp"
            stamps.append(entry)
        off += MARKER_SIZE

    return beacons + stamps


def _find_markers_bruteforce(slot_data):
    """Fallback: scan entire slot for any valid marker entries."""
    markers = []
    for i in range(0, len(slot_data) - MARKER_SIZE, 4):
        entry = _parse_marker(slot_data, i)
        if entry and not entry["empty"] and abs(entry["map_x"]) > 50:
            markers.append(entry)
    return markers


def read_markers(save_path, slot):
    """Read markers from a save file slot."""
    slot_data = _extract_slot_data(save_path, slot)
    if slot_data is None:
        return []
    return find_markers(slot_data)


def _load_legacy_conv(data_dir):
    """Load dungeon-to-overworld coordinate conversion table."""
    conv_path = Path(data_dir) / "WorldMapLegacyConvParam.json"
    if not conv_path.exists():
        return {}
    with open(conv_path) as f:
        rows = json.load(f)
    conv = {}
    for r in rows:
        src_area = int(r.get("srcAreaNo", 0))
        src_gx = int(r.get("srcGridXNo", 0))
        dst_area = int(r.get("dstAreaNo", 0))
        if dst_area not in (60, 61):
            continue
        key = (src_area, src_gx)
        if key not in conv:
            conv[key] = {
                "dstArea": dst_area,
                "dstGridX": int(r.get("dstGridXNo", 0)),
                "dstGridZ": int(r.get("dstGridZNo", 0)),
                "dstPosX": float(r.get("dstPosX", 0)),
                "dstPosZ": float(r.get("dstPosZ", 0)),
                "srcPosX": float(r.get("srcPosX", 0)),
                "srcPosZ": float(r.get("srcPosZ", 0)),
            }
    return conv


def load_massedit_entries(massedit_dir, category_filter=None):
    """Load all MASSEDIT entries with their overworld coordinates."""
    legacy_conv = _load_legacy_conv(Path(massedit_dir).parent)
    pattern = re.compile(
        r"param\s+WorldMapPointParam:\s+id\s+(\d+):\s+(\w+):\s*=\s*(.+);"
    )
    entries = defaultdict(dict)
    entry_category = {}
    for filepath in sorted(Path(massedit_dir).glob("*.MASSEDIT")):
        cat = filepath.stem
        if category_filter and category_filter.lower() not in cat.lower():
            continue
        with open(filepath, "r", encoding="utf-8") as f:
            for line in f:
                m = pattern.match(line.strip())
                if m:
                    row_id = int(m.group(1))
                    field = m.group(2)
                    value = m.group(3).strip()
                    entries[row_id][field] = value
                    entry_category[row_id] = cat

    result = []
    skipped_dungeon = 0
    for row_id, fields in entries.items():
        area = int(float(fields.get("areaNo", "0")))
        gx = int(float(fields.get("gridXNo", "0")))
        gz = int(float(fields.get("gridZNo", "0")))
        px = float(fields.get("posX", "0"))
        pz = float(fields.get("posZ", "0"))
        icon = int(float(fields.get("iconId", "0")))
        dst_area = area  # which overworld layer this maps to
        if area in (60, 61):
            world_x = gx * 256 + px
            world_z = gz * 256 + pz
        else:
            conv_key = (area, gx)
            if conv_key in legacy_conv:
                c = legacy_conv[conv_key]
                dst_area = c.get("dstArea", 60)
                world_x = c["dstGridX"] * 256 + c["dstPosX"] + (px - c["srcPosX"])
                world_z = c["dstGridZ"] * 256 + c["dstPosZ"] + (pz - c["srcPosZ"])
            else:
                skipped_dungeon += 1
                continue
        result.append(
            {
                "row_id": row_id,
                "category": entry_category[row_id],
                "area": area,
                "dst_area": dst_area,
                "gridX": gx,
                "gridZ": gz,
                "posX": px,
                "posZ": pz,
                "worldX": world_x,
                "worldZ": world_z,
                "iconId": icon,
                "textId1": int(float(fields.get("textId1", "0"))),
                "textId2": int(float(fields.get("textId2", "0"))),
                "textId3": int(float(fields.get("textId3", "0"))),
                "flag1": int(float(fields.get("textDisableFlagId1", "0"))),
            }
        )
    if skipped_dungeon:
        print(f"  ({skipped_dungeon} dungeon entries without overworld mapping skipped)")
    return result


def find_nearby(marker, massedit_entries, radius, area_filter=None):
    """Find MASSEDIT entries within radius of a marker's world position.
    If area_filter is set, only match entries from that area (or dungeons mapped to it).
    If None, match area 60 and 61 (overworld grids overlap)."""
    wx, wz = map_to_world(marker["map_x"], marker["map_z"])
    nearby = []
    overworld = {60, 61}
    for entry in massedit_entries:
        if area_filter is not None:
            if entry["area"] != area_filter and entry.get("dst_area") != area_filter:
                continue
        else:
            # Accept overworld entries (60/61) and dungeons mapped to either
            ea = entry["area"]
            da = entry.get("dst_area", ea)
            if ea not in overworld and da not in overworld:
                continue
        dx = entry["worldX"] - wx
        dz = entry["worldZ"] - wz
        dist = (dx**2 + dz**2) ** 0.5
        if dist <= radius:
            nearby.append((dist, entry))
    nearby.sort(key=lambda x: x[0])
    return wx, wz, nearby


def main():
    parser = argparse.ArgumentParser(
        description="Extract map markers from Elden Ring save file"
    )
    parser.add_argument("save_file", help="Path to .err or .sl2 save file")
    parser.add_argument(
        "--slot", type=int, default=0, help="Character slot (0-9, default: 0)"
    )
    parser.add_argument(
        "--radius",
        type=float,
        default=300,
        help="Search radius in world units (default: 300)",
    )
    parser.add_argument(
        "--category", type=str, default=None, help="Filter MASSEDIT by category name"
    )
    parser.add_argument(
        "--massedit-dir", type=str, default=None, help="Path to MASSEDIT directory"
    )
    parser.add_argument(
        "--no-massedit", action="store_true", help="Skip MASSEDIT lookup, just show markers"
    )
    args = parser.parse_args()

    # Read markers
    markers = read_markers(args.save_file, args.slot)
    if not markers:
        print(f"No markers found in slot {args.slot}.")
        sys.exit(0)

    beacons = [m for m in markers if m["kind"] == "beacon"]
    stamps = [m for m in markers if m["kind"] == "stamp"]
    print(f"Found {len(markers)} marker(s) in slot {args.slot}: {len(beacons)} beacons, {len(stamps)} stamps")
    print()

    if args.no_massedit:
        for marker in markers:
            wx, wz = map_to_world(marker["map_x"], marker["map_z"])
            gx, gz, _, _ = world_to_tile(wx, wz)
            # DLC overworld grid roughly 44-53, 37-49
            area = 61 if (44 <= gx <= 53 and 37 <= gz <= 49) else 60
            print(
                f"  #{marker['index']:2d} {marker['kind']:6s} "
                f"type=0x{marker['type']:04X}  "
                f"map=({marker['map_x']:.1f}, {marker['map_z']:.1f})  "
                f"world=({wx:.1f}, {wz:.1f})  ~m{area}_{gx}_{gz}"
            )
        return

    # Find MASSEDIT directory
    massedit_dir = args.massedit_dir
    if not massedit_dir:
        script_dir = Path(__file__).parent.parent
        massedit_dir = script_dir / "data" / "massedit"
        if not massedit_dir.exists():
            massedit_dir = Path(__file__).parent / "data" / "massedit"
    if not Path(massedit_dir).exists():
        print(f"ERROR: MASSEDIT directory not found: {massedit_dir}")
        sys.exit(1)

    massedit_entries = load_massedit_entries(massedit_dir, args.category)
    print(f"Loaded {len(massedit_entries)} MASSEDIT entries (overworld + dungeon)")
    print()

    beacon_num = 0
    stamp_num = 0
    for marker in markers:
        wx, wz = map_to_world(marker["map_x"], marker["map_z"])
        gx, gz, _, _ = world_to_tile(wx, wz)
        area_guess = 61 if (44 <= gx <= 53 and 37 <= gz <= 49) else 60
        type_label = f"0x{marker['type']:04X}"

        if marker['kind'] == 'beacon':
            beacon_num += 1
            seq_label = f"BEACON {beacon_num}"
        else:
            stamp_num += 1
            seq_label = f"STAMP {stamp_num}"

        print(f"--- {seq_label} ({type_label})  map=({marker['map_x']:.1f}, {marker['map_z']:.1f})  ~m{area_guess}_{gx}_{gz}")

        # Match both area 60 and 61 (grids overlap), filter dungeon entries by dst_area
        _, _, nearby = find_nearby(marker, massedit_entries, args.radius, area_filter=None)
        if nearby:
            for dist, entry in nearby[:10]:
                area = entry["area"]
                tile = f"m{area}_{entry['gridX']}_{entry['gridZ']}"
                tid1 = entry.get("textId1", 0)
                tid2 = entry.get("textId2", 0)
                tid3 = entry.get("textId3", 0)
                flag = entry.get("flag1", 0)
                text_parts = []
                if tid1: text_parts.append(f"t1={tid1}")
                if tid2: text_parts.append(f"t2={tid2}")
                if tid3: text_parts.append(f"t3={tid3}")
                text_str = " ".join(text_parts) if text_parts else "no text"
                print(
                    f"  [{dist:4.0f}u] row={entry['row_id']:>8d} "
                    f"icon={entry['iconId']:3d} {tile:14s} "
                    f"flag={flag}  {text_str}  "
                    f"[{entry['category']}]"
                )
            if len(nearby) > 10:
                print(f"  ... and {len(nearby) - 10} more")
        print()


if __name__ == "__main__":
    main()
