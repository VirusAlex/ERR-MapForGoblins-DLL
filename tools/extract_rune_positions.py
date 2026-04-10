#!/usr/bin/env python3
"""
Extract all AEG099_821 (Rune Piece) positions from Elden Ring MSB files.
Uses SoulsFormats.dll via pythonnet to read BDT archives and parse MSBs.
"""

import json
import os
import sys
from pathlib import Path

import config

# Setup .NET runtime
from pythonnet import load
load('coreclr')
import clr
from System.Reflection import Assembly

# Load SoulsFormats
dll_path = str(config.SOULSFORMATS_DLL)
asm = Assembly.LoadFrom(dll_path)
clr.AddReference(dll_path)

import SoulsFormats
from System import Array, Type as SysType, Object
from System.Reflection import BindingFlags

# Andre.SoulsFormats: MSBE.Read(string path) reads and decompresses DCX
_msbe_type = asm.GetType('SoulsFormats.MSBE')
_str_type = SysType.GetType('System.String')
_msbe_read_str = _msbe_type.GetMethod('Read',
    BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str_type]), None)

ERR_MAP_DIR = config.require_err_mod_dir() / 'map' / 'MapStudio'
OUTPUT_RUNE = config.DATA_DIR / 'rune_pieces.json'
OUTPUT_EMBER = config.DATA_DIR / 'ember_pieces.json'

MODEL_NAMES = {"AEG099_821", "AEG099_822"}


def parse_msb_file(msb_path, map_name):
    results = []
    try:
        msb = _msbe_read_str.Invoke(None, Array[Object]([str(msb_path)]))
    except Exception as e:
        return results

    try:
        for asset in msb.Parts.Assets:
            name = str(asset.Name) if asset.Name else ""
            model = str(asset.ModelName) if asset.ModelName else ""
            if any(mn in model or mn in name for mn in MODEL_NAMES):
                pos = asset.Position
                entity_id = asset.EntityID if hasattr(asset, 'EntityID') else -1
                instance_id = asset.InstanceID if hasattr(asset, 'InstanceID') else -1
                results.append({
                    'map': map_name,
                    'name': name,
                    'model': model,
                    'x': float(pos.X),
                    'y': float(pos.Y),
                    'z': float(pos.Z),
                    'entity_id': int(entity_id),
                    'instance_id': int(instance_id),
                })
    except Exception as e:
        print(f"  Error reading assets from {map_name}: {e}")

    return results


def scan_loose_msb_files():
    results = []
    msb_files = sorted(ERR_MAP_DIR.glob("*.msb.dcx"))
    print(f"Scanning {len(msb_files)} ERR MSB files...")

    for f in msb_files:
        try:
            map_name = f.stem.replace('.msb', '')
            r = parse_msb_file(f, map_name)
            if r:
                results.extend(r)
                print(f"  {map_name}: {len(r)} pieces")
        except Exception as e:
            pass

    return results


def main():
    all_results = scan_loose_msb_files()
    print(f"\nTotal found: {len(all_results)} pieces")

    seen = set()
    unique = []
    for r in all_results:
        key = (r['map'], r['name'])
        if key not in seen:
            seen.add(key)
            unique.append(r)

    # Sort per tile by name suffix to match g_tile_to_rows ordering (_9000, _9001, ...)
    def name_suffix(p):
        n = p.get('name', '')
        parts = n.rsplit('_', 1)
        return int(parts[-1]) if len(parts) == 2 and parts[-1].isdigit() else 0

    rune = sorted([r for r in unique if 'AEG099_821' in r.get('model', '')],
                   key=lambda p: (p['map'], name_suffix(p)))
    ember = sorted([r for r in unique if 'AEG099_822' in r.get('model', '')],
                    key=lambda p: (p['map'], name_suffix(p)))
    print(f"\nTotal unique: {len(rune)} Rune + {len(ember)} Ember = {len(unique)} pieces")

    OUTPUT_RUNE.parent.mkdir(parents=True, exist_ok=True)
    with open(OUTPUT_RUNE, 'w') as f:
        json.dump(rune, f, indent=2)
    print(f"Saved rune: {OUTPUT_RUNE}")

    OUTPUT_EMBER.parent.mkdir(parents=True, exist_ok=True)
    with open(OUTPUT_EMBER, 'w') as f:
        json.dump(ember, f, indent=2)
    print(f"Saved ember: {OUTPUT_EMBER}")


if __name__ == "__main__":
    main()
