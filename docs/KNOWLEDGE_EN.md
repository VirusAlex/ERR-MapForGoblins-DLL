# MapForGoblins - Knowledge Base

> [Русская версия](KNOWLEDGE_RU.md)

Everything learned during mod development and reverse-engineering of Elden Ring Reforged game files.
Written so anyone (human or AI agent) can quickly get up to speed.

---

## What is MapForGoblins

A DLL mod for Elden Ring Reforged (ERR). Adds ~7000 icons to the world map: weapons, armor, spells, quest items, bosses, NPCs, Rune Pieces, etc.

The key difference from a regular installer - the mod **does not touch regulation.bin**, all data is injected into memory when the DLL loads. This allows online play without EAC blocking (via Seamless Co-op / mod loader).

Current version: **v1.0.6**, ~7000 WorldMapPointParam entries (+ ~740 vanilla), 41 granular icon categories in INI. Collected Rune/Ember Pieces are automatically hidden on the map.

---

## DLL Architecture

### Modules

| File | Purpose |
|---|---|
| `dllmain.cpp` | Entry point. Logger (spdlog), config loading, mod thread startup |
| `goblin_inject.cpp` | Injecting entries into WorldMapPointParam (replacing ParamTable in memory) |
| `goblin_messages.cpp` | Hook on MsgRepositoryImp::LookupEntry for custom map text |
| `goblin_logic.cpp` | Map fragment logic - icons only appear after the map fragment is collected |
| `goblin_collected.cpp` | Detection of collected Rune/Ember Pieces: GEOF (model hash + InstanceID slot) + WGM (+0x263 bit1) |
| `goblin_config.cpp` | INI parsing (mINI), 41 category toggles + debug_logging |
| `goblin_massedit.cpp` | Runtime MASSEDIT file parser (alternative loading path from `dll/offline/massedit/`) |
| `generated/goblin_map_data.cpp` | Auto-generated array from MASSEDIT files (~7000 entries) |
| `generated/goblin_text_data.cpp` | Auto-generated text data from FMG JSON (14 languages) |
| `modutils.cpp` | AOB scanner (Pattern16), hooks (MinHook), memory utilities |
| `from/params.cpp` | Working with SoloParamRepository - searching and iterating Param tables |

### How injection works (goblin_inject.cpp)

1. Wait for params to load (`from::params::initialize()`)
2. Find `ParamResCap` for "WorldMapPointParam" in ParamList
3. Get pointer to param_file via `rescap + 0x80`
4. `VirtualAlloc` a new buffer: header (0x40) + row locators + data + type string + wrapper locators
5. Copy original rows + add ours, sort by row_id
6. Atomically swap the pointer: `file_ptr_ref = new_param_file`

ParamTable memory layout:
```
ParamResCap -> param_header (+0x78 = size, +0x80 = param_file ptr)
ParamTable (param_file):
  +0x00: param_type_offset (uint32)
  +0x0A: num_rows (uint16)
  +0x30: data_start (uint64)
  +0x40: ParamRowInfo[num_rows] -- 24 bytes each: row_id(u64) + param_offset(u64) + param_end_offset(u64)
  [data_start..]: WORLD_MAP_POINT_PARAM_ST at 256 bytes each
```

### How text works (goblin_messages.cpp)

Hook on `MsgRepositoryImp::LookupEntry` (AOB: `48 8B 3D ?? ?? ?? ?? 44 0F B6 30 48 85 FF 75`).
For IDs >= 9000000, we return our own text from a compiled C++ array.
We also build an FMG binary in memory for PlaceName - so the game sees our entries when iterating over FMG.

Language is detected via the Steam API (`SteamAPI_ISteamApps_GetCurrentGameLanguage`).

### Data generation pipeline

```
MSB files + regulation.bin
        |
        v
  extract_all_items.py  -->  items_database.json
        |
        v
  generate_all_massedit.py  -->  MASSEDIT files (data/massedit/)
        |                         + FMG JSON (data/msg/)
        v
  generate_data.py  -->  goblin_map_data.cpp + goblin_text_data.cpp
        |
        v
  CMake build  -->  MapForGoblins.dll
```

Separate pipeline for Rune/Ember Pieces:
```
MSB files (AEG099_821 / AEG099_822)
        |
        v
  extract_rune_positions.py  -->  rune_pieces.json / ember_pieces.json
        |                         (with InstanceID for GEOF slot mapping)
        v
  generate_pieces_massedit.py  -->  MASSEDIT files + _slots.json
        |                            (row_id -> geom_slot = InstanceID - 9000)
        v
  generate_data.py  -->  goblin_map_data.cpp
                          (MapEntry.geom_slot for each piece)
```

MSB parsing via Andre.SoulsFormats.dll (from Smithbox, copy in `tools/lib/`).
`MSBE.Read(string path)` via reflection - supports both base game and DLC maps.

---

## Key Structures and Formats

### WORLD_MAP_POINT_PARAM_ST (256 bytes)

A map icon entry. Key fields:

| Field | Type | Description |
|---|---|---|
| iconId | int32 | Icon ID (376 = stonesword key style, 393 = standard loot) |
| posX, posZ | float | Map coordinates (world coordinates X and Z) |
| textId1 | int32 | PlaceName text ID (ours start at 9000000+) |
| textDisableFlagId1 | int32 | Event flag - when set, the icon is hidden (item picked up) |
| eventFlagId | int32 | Display flag (map fragment) |
| areaNo | int16 | Area number (60 = overworld, 61 = DLC, 10-21 = dungeons) |
| gridXNo, gridZNo | int16 | Map tile coordinates |
| dispMask00..07 | bits | Map layer visibility masks |
| selectMinZoomStep | int32 | Minimum zoom level for display |

### ItemLotParam_map

Defines what lies at a specific "drop point". Linked to an MSB Treasure event.

- `lotItemId01..08` - item ID (goods/weapon/armor/etc)
- `getItemFlagId` - event flag set on pickup (used for textDisableFlagId1)
- Row ID encodes the map tile: `AABBCCDDEE` -> area AA, grid BB_CC

### MSB (MSBE) - Map Files

Binary level files in `map/MapStudio/`. Contain:
- **Parts** - objects in the world (Assets, Enemies, Players, DummyAssets, DummyEnemies, MapPieces, ConnectCollisions)
- Each Part has: Name, ModelName, Position (x,y,z), EntityID, EntityGroups[8], MapStudioLayer

Parsing via `pythonnet` + `Andre.SoulsFormats.dll` (from Smithbox, copy in `tools/lib/`):
```python
from pythonnet import load
load('coreclr')
import clr
asm = Assembly.LoadFrom('tools/lib/Andre.SoulsFormats.dll')
# Andre.SoulsFormats: MSBE.Read(string path) via reflection:
_msbe_read_str = _msbe_type.GetMethod('Read', ..., Array[SysType]([str_type]), None)
msb = _msbe_read_str.Invoke(None, Array[Object]([path_to_msb_dcx]))
```

Andre.SoulsFormats supports both base game and DLC maps (the DSMSPortable version crashes on DLC `WeatherOverride` region).

Each MSB Part has an **InstanceID** field - used for GEOF slot mapping:
```python
for asset in msb.Parts.Assets:
    instance_id = asset.InstanceID  # e.g. 9001
    geom_slot = instance_id - 9000  # e.g. 1
```

### EMEVD - Compiled Event Scripts

Binary event files in `event/`. Contain:
- Events with unique IDs
- Instructions with Bank:ID (e.g. 2003:66 = SetEventFlag, 2000:00 = RunEvent)
- Arguments as raw bytes (byte array), interpreted via EMEDF

Key instructions:
| Bank:ID | Name | Purpose |
|---|---|---|
| 2000:00 | RunEvent | Call a nested event with arguments |
| 2003:14 | WarpPlayer | Teleport player (NOT related to gatherables) |
| 2003:22 | BatchSetEventFlags | Bulk flag setting |
| 2003:36 | AwardItemsIncludingClients | Award items |
| 2003:66 | SetEventFlag | Set a single flag |
| 2006:04 | CreateAssetFollowingSFX | Create a visual effect |
| 2007:01 | DisplayGenericDialog | Show a dialog box |

### FMG - Text Files

From Software's binary text format. Version 2 (Elden Ring):
- Header: version(u32), fileSize(u32), unk(u32), groupCount(u32)
- Groups at 16 bytes each: firstId(i32), lastId(i32), offsetsStart(i32)
- Strings in UTF-16LE

Stored inside BND4 archives (`item_dlc02.msgbnd.dcx`), compressed with DCX (zstd).

### DCX / BND4 / BHD5

- **DCX** - compression container (zstd for ER, magic `DCX\0`, zstd magic `\x28\xB5\x2F\xFD`)
- **BND4** - file archive (MSB, FMG, etc.)
- **BHD5** - encrypted vanilla archive index (Data0-3.bdt), key for EldenRing = Game enum value 3

---

## Rune Pieces and Ember Pieces - Full Research

Rune Pieces (and Ember Pieces in DLC) are custom ERR items scattered across the world. Small yellow glowing stones. Picking one up adds a Rune Piece (800010) and Runic Trace (800011) to inventory. Each can only be picked up once per playthrough - persisted in the save.

### Identifiers

| Item | Goods ID | MSB Model | Count | Location |
|---|---|---|---|---|
| Rune Piece | 800010 | AEG099_821 | 1164 | Base game (m10, m60, etc.) |
| Ember Piece | 850010 | AEG099_822 | 314 | DLC (m20, m21, m61) |
| Runic Trace | 800011 | -- | -- | Awarded together with Rune Piece |

### Models in MSB

**AEG099_821** (Rune Piece):
- 1164 instances across all base game maps
- **96% (1028) have EntityID = 0** and empty EntityGroups
- Only 41 have an EntityID, with just 4 unique categorical values (e.g. 1042610000)
- MapStudioLayer = 0xFFFFFFFF (all layers) for all
- Dummy properties: ReferenceID=100, Unk34=-1877326030; ReferenceID=90, Unk34=1075484236

**AEG099_822** (Ember Piece):
- 314 instances in DLC maps
- Parsing DLC MSB needs Andre.SoulsFormats.dll (from Smithbox), the standard one crashes

**AEG099_510** ("anchor" objects):
- 133 instances
- Have unique EntityIDs
- Linked to EMEVD events
- Only ~50 of them are managed via event 1045632900
- NOT visual piece models - they're invisible triggers / anchor points
- Not all AEG099_821 are located near an AEG099_510

### EMEVD Chain (50 managed pieces)

```
Event 1045632900 (orchestrator, in common.emevd.dcx)
  |
  |-- RunEvent(1045630910, ...) x 50 times
       |
       Arguments:
         vals[3] = subEvent2 (collectedFlag, e.g. 1045630100)
         vals[7] = subEvent4 (EntityID of AEG099_510)
         vals[8] = lotId (ID from ItemLotParam_map)
         vals[9] = mapTile (encoded as 4 LE bytes: mXX_YY_ZZ_00)
```

Event 1045630910 (handler for a single piece):
1. Checks collectedFlag (subEvent2)
2. If not collected - creates SFX (2006:04), shows interaction dialog (2007:01)
3. On pickup - SetEventFlag(2003:66) for subEvent2, AwardItemsIncludingClients(2003:36) for lotId
4. Hides the object

### What was successfully mapped

43 positions fully linked: **lotId -> EntityID -> XYZ coordinates -> event flag**

| Source | Count | How |
|---|---|---|
| Event 1045632900 -> AEG099_510 | 36 | subEvent4 = EntityID, subEvent2 = collectedFlag |
| Direct entity_matches in MSB | 7 | EntityID matches lot ID |

Data in `data/_piece_final_map.json` and `data/_piece_complete_map.json`.

### SOLVED: Tracking Rune/Ember Pieces (v1.0.6)

Fully reverse-engineered via memory dumps.

**Two data sources:**

1. **GEOF singletons** (unloaded tiles):
   - GeomFlagSaveDataManager (RVA `0x3D69D18`) and GeomNonActiveBlockManager (RVA `0x3D69D98`)
   - Store entries ONLY for destroyed/collected objects
   - Each entry is 8 bytes: flags, geom_idx, **model_hash** (bytes 4-7)
   - Model hash `0x009A1C6D` = AEG099_821 (Rune Piece)
   - GEOF slot = `(geom_idx - 0x1194) * 2 + (flags >> 7)` = `InstanceID - 9000`

2. **CSWorldGeomMan** (loaded tiles, RVA `0x3D69BA8`) - **TAKES PRIORITY over GEOF**:
   - RB-tree of loaded blocks -> geom_ins_vector -> CSWorldGeomIns objects
   - **Combined flag**:
     - +0x263 bit 1 (mask 0x02): persistent, survives restart (32/32 checks)
     - +0x269 & 0x60: immediate after pickup, but resets on restart
   - `alive = (f263 & 0x02) && !(f269 & 0x60)` - alive only if BOTH flags agree
   - WGM data takes priority over GEOF for loaded tiles (GEOF may be stale)

**False candidate: +0x1D8** - processing state, not collected status. Flickers during streaming.

**GEOF slot mapping:**
- Slot does NOT equal the name suffix (_9000 != slot 0 on some tiles)
- Slot = `InstanceID - 9000` (InstanceID is an MSB Part field, read via SoulsFormats)
- WGM mapping: each piece is bound by `name_suffix` -> `row_id` (not by position in the vector)

**Known limitations:**
- ~3 pieces out of ~1164 are tracked via event flags (EMEVD), not GEOF - these are not detected
- Seamless Co-op hosting crashes due to VirtualAlloc'd ParamTable (old bug, unrelated to collected detection)

Details: `geom_collection_tracking.md` in the project root.

---

## Remaining Tasks

### 1. Event-flag tracked pieces (~3)
~3 pieces out of ~1164 are tracked via EMEVD event flags, not GEOF. Detecting them needs event flag reading from memory (event 1045632900 -> collectedFlag).

### 2. Seamless Co-op hosting
The VirtualAlloc'd ParamTable (9803 rows instead of 740) is incompatible with Seamless Co-op when creating a session (hosting). Options:
- Hook param lookup instead of replacing the table
- HeapAlloc instead of VirtualAlloc
- Figure out what exactly Seamless Co-op does with params during hosting

### 3. Reference Offsets

Player position:
```
WorldChrMan (RVA: base + 0x3D65F88)
  -> PlayerIns (+0x10EF8)
    -> ChrModules (+0x190)
      -> SubModule (+0xC0)
        -> WorldPosition (+0x40)  // float x, y, z
```

ERR-specific event IDs:
- Event 1045632900 - Rune Pieces orchestrator (50 RunEvent calls)
- Event 1045630910 - single piece handler with AEG099_510

---

## Dependencies and Tools

### For DLL build (C++)
- CMake 3.28+, MSVC (Visual Studio Build Tools 2022)
- MinHook (hooks), Pattern16 (AOB scanner), mINI (INI parser), spdlog (logger)

### For scripts (Python)
- `pythonnet` - calling C#/.NET from Python (for SoulsFormats)
- `Andre.SoulsFormats.dll` (from Smithbox) - From Software format parser. Copy in `tools/lib/`.
  Supports both base game and DLC MSB (unlike the DSMSPortable version)
- `pymem` - process memory reading (for dump scripts)

### Paths
All external paths are configured via `tools/config.ini` (copy from `config.ini.example`).
- ERR mod (`err_mod_dir`): folder with regulation.bin, map/, event/, msg/
- Game (`game_dir`): folder with eldenring.exe and oo2core_6_win64.dll
- Andre.SoulsFormats.dll: `tools/lib/` (in repo)
- Paramdefs XML: `tools/paramdefs/` (in repo)

---

## Output Data (data/)

### JSON Files
| File | Contents |
|---|---|
| `items_database.json` | All items from MSB Treasure + regulation.bin |
| `WorldMapPointParam.json` | Dump of vanilla WorldMapPointParam |
| `rune_pieces.json` | 1164 AEG099_821 positions with InstanceID (after dedup ~1113) |
| `ember_pieces.json` | 314 AEG099_822 positions with InstanceID |
| `new_fmg_entries.json` | New text entries for FMG |
| `comparison_report.json` | Comparison of MASSEDIT with items_database |

### Diagnostic JSON (Rune Pieces research)
| File | Contents |
|---|---|
| `_pieces_diagnostic.json` | All rune/ember lot IDs, event flags, entity_matches |
| `_emevd_findings.json` | Lot ID hits in EMEVD instructions |
| `_piece_mappings.json` | Coordinates from per-map EMEVD |
| `_piece_complete_map.json` | 43 mapped positions (lotId+flag+coords) |
| `_piece_final_map.json` | Final map with collectedFlag |
| `_piece_models.json` | Comparison of AEG099_510/821/822 |
| `_rune_pieces_821.json` | Full analysis of all AEG099_821 (properties, EntityID) |

---

## Troubleshooting History

### soulstruct doesn't work
The soulstruct library (Python) is broken on Python 3.13 - crashes on import. Workaround: use pythonnet + SoulsFormats.dll directly.

### DLC MSB files don't parse
DSMSPortable SoulsFormats.dll crashes on DLC MSB due to `WeatherOverride` region. Fix: use Andre.SoulsFormats.dll from Smithbox, which supports the DLC format.

### MSBE.Read via pythonnet
Andre.SoulsFormats supports `MSBE.Read(string path)` - reads and decompresses DCX:
```python
_msbe_read_str = _msbe_type.GetMethod('Read', ..., Array[SysType]([str_type]), None)
msb = _msbe_read_str.Invoke(None, Array[Object]([path_to_msb_dcx]))
```

### EMEVD Instruction.Index -> Instruction.ID
In SoulsFormats, the `EMEVD.Instruction` class uses `.ID` for the instruction number (not `.Index`).

### dispMask / pad2_0 confusion
In MASSEDIT, `pad2_0: = 1` corresponds to `dispMask02` (bit 2 of byte 0x18). This is the DLC map layer (area 61). For overworld (area 60), `dispMask00` is used.

### Player position - wrong offsets
Initially ChrModules+0x68 (PhysMod)+0x70 returned (0,0,0). Correct chain: ChrModules+0xC0 (SubModule)+0x40 gives real world coordinates.

---

## Tracking Rune Pieces - SOLVED (v1.0.6)

Approach: game memory dump (Python + pymem) -> byte-by-byte comparison of CSWorldGeomIns structures -> combined flag (+0x263 persistent + +0x269 immediate) -> verification across 4+ dumps.

Result: 175/178 pieces are correctly hidden (3 event-flag tracked ones are not covered).

Details: see the "SOLVED: Tracking Rune/Ember Pieces" section above and `geom_collection_tracking.md`.
