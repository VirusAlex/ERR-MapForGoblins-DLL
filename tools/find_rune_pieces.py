#!/usr/bin/env python3
"""Search EMEVD event files for Rune Piece ItemLot references."""

import struct
import csv
import json
from pathlib import Path

import config

ERR_EVENT_DIR = config.require_err_mod_dir() / "event"
CSV_PATH = Path(__file__).parent.parent / "data" / "ItemLotParam_map.csv"

def decompress_dcx(data):
    if data[:4] != b'DCX\x00':
        return data
    import zstandard
    idx = data.find(b'\x28\xB5\x2F\xFD')
    if idx >= 0:
        return zstandard.ZstdDecompressor().decompress(data[idx:], max_output_size=50*1024*1024)
    raise ValueError("Cannot decompress")

def load_itemlot_ids(csv_path):
    ids = set()
    with open(csv_path, 'r') as f:
        for row in csv.DictReader(f):
            ids.add(int(row['ID']))
    return ids

def search_binary_for_ids(data, target_ids):
    hits = {}
    for tid in target_ids:
        packed = struct.pack('<i', tid)
        count = 0
        start = 0
        while True:
            idx = data.find(packed, start)
            if idx < 0:
                break
            count += 1
            start = idx + 4
        if count > 0:
            hits[tid] = count
    return hits

def main():
    lots = load_itemlot_ids(CSV_PATH)
    print(f"Searching for {len(lots)} ItemLot IDs in EMEVD files...")

    emevd_files = sorted(ERR_EVENT_DIR.glob("*.emevd.dcx"))
    print(f"Found {len(emevd_files)} EMEVD files")

    results = {}  # id -> {file -> count}
    files_with_hits = {}

    for i, path in enumerate(emevd_files):
        if i % 100 == 0:
            print(f"  {i}/{len(emevd_files)}...")
        try:
            data = decompress_dcx(path.read_bytes())
        except:
            continue

        hits = search_binary_for_ids(data, lots)
        if hits:
            fname = path.stem.replace(".emevd", "")
            files_with_hits[fname] = hits
            for tid, count in hits.items():
                results.setdefault(tid, {})[fname] = count

    found = set(results.keys())
    print(f"\n=== Found {len(found)}/{len(lots)} IDs in EMEVD files ===")

    if files_with_hits:
        # Show by file
        for fname in sorted(files_with_hits.keys())[:30]:
            hits = files_with_hits[fname]
            print(f"\n  {fname}: {len(hits)} ItemLots")
            for tid in sorted(hits.keys())[:5]:
                print(f"    ID {tid} ({hits[tid]}x)")
            if len(hits) > 5:
                print(f"    ... and {len(hits)-5} more")

    # Summary
    not_found = lots - found
    print(f"\nFound: {len(found)}, Not found: {len(not_found)}")
    if not_found and len(not_found) < 20:
        print(f"Missing IDs: {sorted(not_found)}")

if __name__ == "__main__":
    main()
