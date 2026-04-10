# MapForGoblins - Tools

Python scripts for data extraction, code generation, and debugging.
Scripts prefixed with `_` are research/diagnostic tools. The rest form the build pipeline.

## Setup

```bash
pip install -r ../requirements.txt
cp config.ini.example config.ini
# Edit config.ini with your local paths
```

Dependencies:
- **Python 3.10+** with `pythonnet`, `pymem`
- **Andre.SoulsFormats.dll** + deps - bundled in `lib/` (from Smithbox, supports base game + DLC)
- **Paramdex XMLs** - bundled in `paramdefs/` (Elden Ring param field definitions)
- **oo2core_6_win64.dll** - ships with Elden Ring, resolved from `game_dir` in config.ini

---

## Core Pipeline

Build order: `extract_all_items.py` → `generate_all_massedit.py` → `generate_data.py` → `build.bat`

### extract_all_items.py - Extract item database
Parses regulation.bin (ItemLotParam, EquipParam) and MSB Treasure events to build a
complete item database with positions, names, event flags, and categories.
No arguments. Reads ERR mod files via config.ini.
Output: `data/items_database.json`

### extract_rune_positions.py - Extract Rune/Ember Piece positions
Scans all MSB files for AEG099_821 (Rune) and AEG099_822 (Ember) assets. Extracts
coordinates, model name, and InstanceID (critical for GEOF slot mapping).
No arguments. Reads ERR mod MapStudio via config.ini.
Output: `data/rune_pieces.json`, `data/ember_pieces.json`

### extract_itemlot_csv.py - Extract ItemLotParam from regulation
Reads regulation.bin and exports ItemLotParam_map rows matching Rune/Ember Piece
goods IDs (800010, 850010) to CSV. Replaces the need for SmithBox export.
Args: `[path/to/regulation.bin]` (optional, defaults to config.ini).
Output: `data/ItemLotParam_map.csv`

### generate_all_massedit.py - Generate MASSEDIT from item database
Converts items_database.json into MASSEDIT files for each item category. Preserves
existing manually-edited entries, assigns row IDs, and generates FMG text entries.
No arguments.
Output: `data/massedit/*.MASSEDIT`, `data/new_fmg_entries.json`

### generate_pieces_massedit.py - Generate Rune/Ember Piece MASSEDIT
Creates MASSEDIT entries from rune/ember JSON data, matches event flags from
ItemLotParam_map.csv, and saves slot mapping for the DLL's GEOF detection.
No arguments.
Output: `data/massedit/Reforged - Rune Pieces.MASSEDIT`, `*_slots.json`

### generate_data.py - Generate C++ source from MASSEDIT
Reads all MASSEDIT files + FMG JSONs and generates C++ arrays compiled into the DLL.
Maps MASSEDIT categories to C++ enums, embeds geom_slot from slot JSONs.
No arguments.
Output: `src/generated/goblin_map_data.cpp`, `src/generated/goblin_text_data.cpp`

### compare_massedit.py - Validate MASSEDIT vs item database
Cross-references current MASSEDIT files with items_database.json. Reports matched,
missing, obsolete, and position-mismatched entries.
No arguments.
Output: console report, `data/comparison_report.json`

---

## Save File Analysis

### rune_piece_tracker.py - Count collected pieces from save
Reads an .err/.sl2 save file and parses the GEOM section to count Rune Pieces
per map tile by (geom_idx, instance_hash) pairs.
Args: `<save_file>`.
Output: console report with per-tile collected/total counts.

### rune_piece_map.py - Build piece map from save + coordinates
Combines save file GEOF/GEOM data with rune_pieces.json to produce a map of all
Rune Pieces with their collected/uncollected status. Supports dungeon-to-world
coordinate mapping.
Args: `<save_file> --json <rune_pieces.json> [--out map.json] [--html map.html]`.
Output: JSON map and optional HTML visualization.

### _diff_saves_bytes.py - Binary diff of two saves
Simple byte-level diff. Shows changed regions with hex context, detects single-bit
changes and interprets uint32 deltas.
Args: `<before.err> <after.err>`.
Output: console diff report.

### _diff_saves_slots.py - Structural diff of two saves
Parses BND4 container, extracts character slots (0x280000 bytes each), and diffs at
the slot level. Knows about EventFlags section layout.
Args: `<before.err> <after.err> [--slot N]`.
Output: console structural diff.

### _diff_saves_triple.py - Triple-test comparator
Compares 2-4 save files to isolate bits unique to each pickup action. Designed for
research: pick piece A, pick piece B, pick both - find which bits are unique to each.
Args: `<base.err> <test1.err> [test2.err] [test3.err]`.
Output: console analysis + diff_analysis.txt.

### _inspect_save.py - Save file structure inspector
Hex dumps the header, detects BND4 structure, searches for patterns (USER_DATA, SAVE),
and analyzes slot boundaries.
Args: `<save_file.err>`.
Output: console structural analysis.

---

## GEOF Analysis (Save Files)

### _parse_geof.py - Parse GEOF/GEOM sections
Parses GEOF and GEOM sections from save files, counts collected Rune Pieces per tile
by matching geom_idx range (0x1194-0x11A6) and flags.
Args: `<file.err> [file2.err ...]` or `<directory>` (processes all .err).
Output: console per-tile piece counts.

### _diff_geof_overview.py - Diff GEOF between save pairs
Compares GEOF entries between two saves to find newly added/removed entries. Shows
which pieces were collected between saves.
Args: `<before.err> <after.err>` or `<directory>` (pairs consecutive .err files).
Output: console diff with added/removed entries per tile.

### _diff_geof_tile.py - Detailed GEOF diff for one tile
Shows ALL GEOF entries on a specific tile across multiple saves. First file is baseline,
subsequent files are compared against it. Useful for understanding per-tile slot mapping.
Args: `<save1.err> <save2.err> [save3.err ...] [--tile m60_33_45_00]`.
Output: console per-entry comparison on target tile.

### _count_pieces.py - Count pieces from save files
Counts collected Rune Pieces using (tile, instance_hash) pairs from GEOF sections.
Simpler output than _parse_geof.py.
Args: `<save.err> [save2.err ...]`.
Output: console count per tile.

### _find_partial_tiles.py - Find partially collected tiles
Cross-references GEOF data from saves with rune_pieces.json to find tiles where some
but not all pieces have been collected. Auto-discovers save from %APPDATA%/EldenRing.
No arguments.
Output: console report of partially collected tiles.

---

## Live Game Memory (requires game running)

These scripts attach to `eldenring.exe` via pymem and read memory structures in real time.

### compare_collected.py - Memory vs save comparison
Compares what the DLL sees in memory (GEOF singletons) with what the save file
contains. Identifies discrepancies between live game state and persisted state.
No arguments (auto-discovers save from %APPDATA%).
Output: console discrepancy report.

### _check_all_slots.py - Audit GEOF slots for all tiles
Checks GEOF entries for every known Rune Piece tile. Shows which tiles are in GEOF,
their 821-hash entries, slot assignments, and WGM (loaded) status.
No arguments.
Output: console per-tile GEOF/WGM audit.

### _check_tile_geof.py - Check GEOF for one tile
Examines GEOF entries for a specific tile from live game memory.
Args: `<tile_name>` (e.g. `m60_39_40_00`).
Output: console GEOF slot assignments.

### _dump_aeg099.py - Dump AEG099 objects on a tile
Lists all AEG099_* geometry objects loaded on a tile, showing vector index to
MSB name mapping. Used to understand geom_idx slot ordering.
Args: `<tile_name>`.
Output: console object list.

### _dump_tile_geom.py - Dump all GeomIns on a tile
Lists ALL geometry instances (not just AEG099) on a specific tile with their
vector indices and names.
Args: `<tile_name>`.
Output: console geometry instance list.

### _find_slot_field.py - Find GEOF slot field in GeomIns
Reads candidate fields from AEG099_821 GeomIns objects to determine which memory
offset stores the GEOF slot number. Research tool for reverse-engineering.
Args: `<tile_name>`.
Output: console field analysis.

### _dump_game_memory.py - Full memory structure dump
Full dump of the game's geometry management memory: GEOF singletons, WGM
loaded tiles, BlockData, CSWorldGeomIns fields. Outputs timestamped dump directory.
Args: `[label]` (optional tag for the dump folder).
Output: `dumps/dump_YYYYMMDD_HHMMSS_label/` with binary dumps and text reports.

---

## EMEVD / Event Research

Scripts that analyze EMEVD event scripts to understand how Rune Piece collection works.

### find_rune_pieces.py - Search EMEVD for Rune Piece ItemLots
Searches EMEVD binary files for int32 values matching Rune/Ember Piece ItemLot IDs
from ItemLotParam_map.csv.
No arguments.
Output: console search results.

### _parse_emevd.py - Search EMEVD for specific instructions
Searches events for instructions 2003:04 (AwardItemLot), 2003:36, 2003:69 referencing
Rune/Ember Piece lot IDs.
No arguments.
Output: console instruction search results.

### _deep_emevd.py - Deep event analysis
Detailed analysis of the main Rune Piece handler event (1045630910). Decodes RunEvent
arguments, finds sub-events, traces arg[9] as map tile encoding.
No arguments.
Output: console event breakdown.

### _find_piece_coords.py - Map event flags to coordinates
Traces event → RunEvent → EntityID → MSB position chain to build coordinate-to-flag
mapping for Rune/Ember Pieces.
No arguments.
Output: console coordinate/flag mapping.

### _find_entity_groups.py - Search MSB EntityGroups
Searches MSB files for entities whose EntityID or EntityGroups match Rune/Ember Piece
EMEVD sub-event ID ranges (1045630100-1045633200).
No arguments.
Output: console entity matches.

### _find_remaining_flags.py - Find flags for uncovered positions
Targeted search for event flags of AEG099_510 positions not covered by the main
event handler. Scans per-map EMEVD files.
No arguments.
Output: console flag candidates.

### _piece_flag_map.py - Final piece-to-flag mapping
Full mapping combining AEG099_510 index, EMEVD scan, and entity matching.
Key finding: subEvent2 (1045630100+n) = per-position collected flag.
No arguments.
Output: `data/_piece_final_map.json`.

### _match_pieces.py - Cross-reference pieces across data sources
Matches ItemLotParam entries with MSB Assets by EntityID or coordinate proximity to
MASSEDIT entries.
No arguments.
Output: console matching results.

---

## FMG / Text Utilities

### add_fmg_entries.py - Add entries to PlaceName FMG
Adds "Rune Piece" / "Ember Piece" text entries to the binary PlaceName_dlc01.fmg
file (FMG v2 format). Rebuilds the binary with proper group structure.
No arguments.
Output: modified `PlaceName_dlc01.fmg`.

### debug_fmg.py - Dump FMG structure
Displays header, groups, and entries of a PlaceName FMG file for debugging.
No arguments.
Output: console FMG structure dump.

### find_free_ids.py - Find available PlaceName IDs
Scans game FMG files to show which PlaceName ID ranges are free for custom entries.
No arguments (requires oo2core).
Output: console ID range report.

### find_textid.py - Search for a text ID in FMG files
Searches item_dlc02.msgbnd.dcx for specific PlaceName text IDs.
No arguments (requires oo2core).
Output: console search results.

### check_textid.py - Verify specific text IDs
Checks a fixed set of IDs (10500000-10500005) in PlaceName_dlc01.fmg.
No arguments.
Output: console text content.

### get_fmg_ids.py - List BND4 file entries
Lists file_id → filename mappings inside item_dlc02.msgbnd.dcx.
No arguments (requires oo2core).
Output: console file listing.

### list_fmgs.py - List PlaceName FMGs
Lists PlaceName.fmg files in the game's BND4 archives with entry counts.
No arguments (requires oo2core).
Output: console FMG metadata.

### verify_patched.py - Verify patched FMG text
Checks that specific text IDs (10600001, 10600002, 10500002) are present and correct
in the patched item_dlc02.msgbnd.dcx.
No arguments (requires oo2core).
Output: console verification results.

---

## File Comparison Utilities

### compare_bnd.py - Compare BND4 structures
Compares BND4 headers, file entries, data alignment and content of 2+ DCX files.
Shows per-entry diffs with hex context.
Args: `<file_a.dcx> <file_b.dcx> [file_c.dcx ...]` (requires oo2core).
Output: console structural comparison.

### compare_dcx.py - Dump DCX headers
Dumps raw DCX header fields (magic, sizes, compression method, offsets) for one or
more files.
Args: `<file1.dcx> [file2.dcx ...]`.
Output: console header dump.

### compare_fmg_detail.py - Compare PlaceName FMG entries
Extracts and compares FMG text entries between two DCX files. Shows IDs only in A/B,
text differences for common IDs.
Args: `<file_a.dcx> <file_b.dcx>` (requires oo2core).
Output: console text diff.

### check_align.py - Check BND4 data alignment
Verifies that all file entries in a BND4 archive are 16-byte aligned. Reports
misaligned entries.
Args: `<file1.dcx> [file2.dcx ...]` (requires oo2core).
Output: console alignment check.

---

## Other Research Tools

### extract_world_map_param.py - Export WorldMapPointParam
Extracts WorldMapPointParam from regulation.bin to CSV and JSON for comparison with
MASSEDIT data.
No arguments.
Output: `data/WorldMapPointParam.csv`, `data/WorldMapPointParam.json`.

### _search_regulation.py - Search all regulation params
Searches every param table in regulation.bin for references to Rune/Ember Piece goods
IDs (800010, 850010) and model strings.
No arguments.
Output: console search results.

### _check_pieces.py - Quick piece diagnostic
Finds Rune/Ember Pieces in ItemLotParam_map and checks associated event flags. Quick
alternative to the full extract_all_items.py pipeline.
No arguments.
Output: console diagnostic.

### _analyze_821.py - Deep AEG099_821 analysis
Examines all properties of AEG099_821 assets: EntityID, EntityGroups, UnkStruct, etc.
Finds that 96% have EntityID=0.
No arguments.
Output: console analysis, `data/_rune_pieces_821.json`.

### _find_821_822.py - Compare 821/822/510 assets
Searches MSB files for AEG099_821, AEG099_822, and AEG099_510 assets and compares
their distribution and relationships.
No arguments.
Output: console comparison.

### _decode_pieces.py - Decode EMEVD piece arguments
Decodes arg[9] format in RunEvent calls as 4-byte mapTile encoding. Dumps handler
event structure and cross-references with MASSEDIT.
No arguments.
Output: console decode analysis.

### _export_pieces_bin.py - Export pieces to binary
Converts rune_pieces.json to a compact binary format (RPDB header + [x, y, z, tile_id]
records). Experimental format, not used by the DLL.
No arguments.
Output: `data/pieces.bin`.

### _verify_positions.py - Verify piece positions
Quick validation of piece coordinate database: finds nearest pieces to a test position,
reports distances.
No arguments.
Output: console verification.
