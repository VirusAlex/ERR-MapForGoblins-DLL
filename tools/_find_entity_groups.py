#!/usr/bin/env python3
"""
Search MSB files for entities with EntityGroups matching the sub-event IDs
from the Rune/Ember Piece EMEVD events (1045630100-1045633200).
Also search for entities with EntityID in range 10000-10999.
"""
import sys, io, struct, json
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
from pathlib import Path
import config
from pythonnet import load
load('coreclr')
import clr
from System.Reflection import Assembly
from System import Array, Type as SysType, Object

asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
clr.AddReference(str(config.SOULSFORMATS_DLL))
import SoulsFormats

_byte_arr_type = SysType.GetType('System.Byte[]')
_msbe_cls = asm.GetType('SoulsFormats.MSBE')
_msbe_read = _msbe_cls.BaseType.GetMethod('Read', Array[SysType]([_byte_arr_type]))

MAP_DIR = config.require_err_mod_dir() / 'map' / 'MapStudio'
DATA_DIR = config.DATA_DIR

diag = json.load(open(DATA_DIR / '_pieces_diagnostic.json', 'r', encoding='utf-8'))
RUNE_LOTS = {int(k): v for k, v in diag['rune_lots'].items()}
EMBER_LOTS = {int(k): v for k, v in diag['ember_lots'].items()}
ALL_LOTS = {**RUNE_LOTS, **EMBER_LOTS}

# Target entity group ranges
# From EMEVD: subEvent1=1045631100-1045631147, subEvent2=1045630100-1045630147,
# subEvent3=1045632100-1045632147, subEvent4=1045633100-1045633175
# Also: lotBase1=10100-10572, lotBase2=10101-10572
TARGET_RANGES = [
    (1045630100, 1045630200, 'subEvent2'),
    (1045631100, 1045631200, 'subEvent1'),
    (1045632100, 1045632200, 'subEvent3'),
    (1045633100, 1045633200, 'subEvent4'),
    (10000, 10999, 'entityBase'),
]

# Target tiles from EMEVD decode
TARGET_TILES = [
    'm60_42_36_00', 'm60_46_38_00', 'm60_43_31_00', 'm10_00_00_00',
    'm14_00_00_00', 'm60_39_39_00', 'm60_43_34_00', 'm60_37_46_00',
    'm60_35_42_00', 'm12_02_00_00', 'm12_01_00_00', 'm12_03_00_00',
    'm60_35_51_00', 'm60_38_51_00', 'm15_00_00_00', 'm60_43_52_00',
    'm60_39_54_00', 'm60_38_54_00', 'm16_00_00_00', 'm60_46_40_00',
    'm60_49_38_00', 'm60_51_36_00', 'm60_51_43_00', 'm11_00_00_00',
    'm11_05_00_00', 'm35_00_00_00',
    'm60_49_53_00', 'm60_51_57_00', 'm60_48_57_00', 'm60_51_53_00',
    'm13_00_00_00', 'm60_54_55_00', 'm12_05_00_00',
    'm61_45_42_00', 'm61_48_41_00', 'm61_49_39_00', 'm61_46_45_00',
    'm61_51_45_00', 'm61_45_47_00', 'm20_00_00_00', 'm61_48_44_00',
    'm61_47_35_00', 'm21_00_00_00', 'm61_49_48_00', 'm28_00_00_00',
    'm61_54_39_00',
]

print("=" * 70)
print("Searching target tiles for EntityGroups / EntityIDs in piece ranges")
print("=" * 70)

found_entities = []
tiles_searched = 0

for tile in TARGET_TILES:
    msb_path = MAP_DIR / f'{tile}.msb.dcx'
    if not msb_path.exists():
        msb_path = MAP_DIR / f'{tile}.msb'
    if not msb_path.exists():
        print(f"  [SKIP] {tile}: MSB not found")
        continue
    
    try:
        raw = SoulsFormats.DCX.Decompress(str(msb_path))
        msb = _msbe_read.Invoke(None, Array[Object]([raw]))
    except Exception as ex:
        print(f"  [ERR] {tile}: {ex}")
        continue
    
    tiles_searched += 1
    tile_found = 0
    
    for parts_type in ['Assets', 'DummyAssets', 'Enemies', 'DummyEnemies', 
                        'MapPieces', 'Players', 'Collisions', 'ConnectCollisions']:
        parts = getattr(msb.Parts, parts_type, None)
        if parts is None:
            continue
        for p in parts:
            name = str(p.Name)
            eid = int(p.EntityID) if hasattr(p, 'EntityID') else -1
            model = str(p.ModelName) if hasattr(p, 'ModelName') else '?'
            pos = (float(p.Position.X), float(p.Position.Y), float(p.Position.Z))
            
            # Check EntityID against ranges
            for rmin, rmax, rname in TARGET_RANGES:
                if rmin <= eid <= rmax:
                    print(f"  MATCH EntityID: {tile} {parts_type}/{name} "
                          f"model={model} eid={eid} ({rname}) "
                          f"pos=({pos[0]:.1f}, {pos[1]:.1f}, {pos[2]:.1f})")
                    found_entities.append({
                        'tile': tile, 'partType': parts_type, 'name': name,
                        'model': model, 'entityId': eid, 'matchType': f'EntityID:{rname}',
                        'x': pos[0], 'y': pos[1], 'z': pos[2],
                    })
                    tile_found += 1
            
            # Check EntityGroups
            if hasattr(p, 'EntityGroups'):
                for eg in p.EntityGroups:
                    eg = int(eg)
                    if eg <= 0:
                        continue
                    for rmin, rmax, rname in TARGET_RANGES:
                        if rmin <= eg <= rmax:
                            print(f"  MATCH EntityGroup: {tile} {parts_type}/{name} "
                                  f"model={model} eid={eid} group={eg} ({rname}) "
                                  f"pos=({pos[0]:.1f}, {pos[1]:.1f}, {pos[2]:.1f})")
                            found_entities.append({
                                'tile': tile, 'partType': parts_type, 'name': name,
                                'model': model, 'entityId': eid, 'entityGroup': eg,
                                'matchType': f'EntityGroup:{rname}',
                                'x': pos[0], 'y': pos[1], 'z': pos[2],
                            })
                            tile_found += 1
    
    if tile_found == 0:
        # No matches - let's look at what EntityGroups exist in this tile
        all_groups = set()
        all_eids = set()
        for parts_type in ['Assets', 'DummyAssets', 'Enemies', 'DummyEnemies']:
            parts = getattr(msb.Parts, parts_type, None)
            if parts is None:
                continue
            for p in parts:
                eid = int(p.EntityID) if hasattr(p, 'EntityID') else -1
                if eid > 0:
                    all_eids.add(eid)
                if hasattr(p, 'EntityGroups'):
                    for eg in p.EntityGroups:
                        eg = int(eg)
                        if eg > 0:
                            all_groups.add(eg)

print(f"\nSearched {tiles_searched} tiles")
print(f"Found {len(found_entities)} matching entities")

# ========================
# Now search ALL MSBs (not just target tiles) for the entity group ranges
# ========================
print("\n" + "=" * 70)
print("Searching ALL MSB files for entity groups in piece ranges")
print("=" * 70)

msb_files = sorted(MAP_DIR.glob('*.msb.dcx'))
total_found = 0

for msb_path in msb_files:
    tile = msb_path.name.split('.')[0]
    
    try:
        raw = SoulsFormats.DCX.Decompress(str(msb_path))
        msb = _msbe_read.Invoke(None, Array[Object]([raw]))
    except:
        continue
    
    for parts_type in ['Assets', 'DummyAssets', 'Enemies', 'DummyEnemies']:
        parts = getattr(msb.Parts, parts_type, None)
        if parts is None:
            continue
        for p in parts:
            eid = int(p.EntityID) if hasattr(p, 'EntityID') else -1
            
            # Check EntityID
            for rmin, rmax, rname in TARGET_RANGES:
                if rmin <= eid <= rmax:
                    name = str(p.Name)
                    model = str(p.ModelName) if hasattr(p, 'ModelName') else '?'
                    pos = (float(p.Position.X), float(p.Position.Y), float(p.Position.Z))
                    print(f"  {tile} {parts_type}/{name} model={model} eid={eid} ({rname}) "
                          f"pos=({pos[0]:.1f}, {pos[1]:.1f}, {pos[2]:.1f})")
                    total_found += 1
            
            # Check EntityGroups
            if hasattr(p, 'EntityGroups'):
                for eg in p.EntityGroups:
                    eg = int(eg)
                    if eg <= 0:
                        continue
                    for rmin, rmax, rname in TARGET_RANGES:
                        if rmin <= eg <= rmax:
                            name = str(p.Name)
                            model = str(p.ModelName) if hasattr(p, 'ModelName') else '?'
                            pos = (float(p.Position.X), float(p.Position.Y), float(p.Position.Z))
                            egroups = [int(x) for x in p.EntityGroups if int(x) > 0]
                            print(f"  {tile} {parts_type}/{name} model={model} eid={eid} "
                                  f"group={eg} ({rname}) groups_all={egroups[:8]} "
                                  f"pos=({pos[0]:.1f}, {pos[1]:.1f}, {pos[2]:.1f})")
                            total_found += 1

print(f"\nTotal matches across all MSBs: {total_found}")

# ========================
# Also search for lot IDs (10100-10572 range) as EntityIDs
# ========================
print("\n" + "=" * 70)
print("Searching ALL MSBs for lot base IDs as EntityIDs")
print("=" * 70)

lot_bases = set()
for p_data in json.load(open(DATA_DIR / '_piece_mappings.json', 'r'))[:50]:
    for v in p_data.get('vals', []):
        if 10000 <= v <= 11000:
            lot_bases.add(v)

print(f"Searching for lot bases: {sorted(lot_bases)[:20]}...")

lot_base_matches = 0
for msb_path in msb_files:
    tile = msb_path.name.split('.')[0]
    try:
        raw = SoulsFormats.DCX.Decompress(str(msb_path))
        msb = _msbe_read.Invoke(None, Array[Object]([raw]))
    except:
        continue
    
    for parts_type in ['Assets', 'DummyAssets', 'Enemies', 'DummyEnemies']:
        parts = getattr(msb.Parts, parts_type, None)
        if parts is None:
            continue
        for p in parts:
            eid = int(p.EntityID) if hasattr(p, 'EntityID') else -1
            if eid in lot_bases:
                name = str(p.Name)
                model = str(p.ModelName) if hasattr(p, 'ModelName') else '?'
                pos = (float(p.Position.X), float(p.Position.Y), float(p.Position.Z))
                print(f"  {tile} {parts_type}/{name} model={model} eid={eid} "
                      f"pos=({pos[0]:.1f}, {pos[1]:.1f}, {pos[2]:.1f})")
                lot_base_matches += 1
            
            if hasattr(p, 'EntityGroups'):
                for eg in p.EntityGroups:
                    eg = int(eg)
                    if eg in lot_bases:
                        name = str(p.Name)
                        model = str(p.ModelName) if hasattr(p, 'ModelName') else '?'
                        pos = (float(p.Position.X), float(p.Position.Y), float(p.Position.Z))
                        print(f"  {tile} {parts_type}/{name} model={model} eid={eid} "
                              f"group={eg} pos=({pos[0]:.1f}, {pos[1]:.1f}, {pos[2]:.1f})")
                        lot_base_matches += 1

print(f"\nLot base matches: {lot_base_matches}")
print("\nDone!")
