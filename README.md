# ELDEN RING Reforged - MapForGoblins - DLL

A DLL mod for [Elden Ring Reforged](https://www.nexusmods.com/eldenring/mods/541) (ERR) that adds ~7000 icons to the world map: weapons, armor, spells, quest items, bosses, NPCs, Rune Pieces, etc.

Unlike [Goblin Maps](https://www.nexusmods.com/eldenring/mods/3091), this mod does not modify `regulation.bin`. All map point data is injected into memory at runtime, so it won't conflict with other regulation edits.

> **Note:** Not yet whitelisted for ERR online play. Seamless Co-op has a known conflict - crashes when hosting a session. Open issue.

Collected Rune Pieces and Ember Pieces are automatically hidden on the map using real-time memory detection of the game's geometry object state.

## Features

- ~7000 map icons across 41 toggleable categories (configurable via INI)
- Map icon text injected via MsgRepository hook (English only, applied to all language slots)
- Collected Rune/Ember Piece detection: GEOF singletons for unloaded tiles + CSWorldGeomMan flags for loaded tiles
- No regulation.bin changes - no conflicts with other mods
- Addon-compatible folder structure for ERR

## Building

Requirements:
- Visual Studio 2022 (Build Tools or Community)
- CMake 3.28+
- Internet connection (CMake fetches dependencies on first configure)

```bash
build.bat              # configure + build
build.bat generate     # regenerate C++ data from MASSEDIT/FMG (requires Python)
build.bat clean        # delete build directory
```

Output: `build/Release/MapForGoblins.dll` + `MapForGoblins.ini`

## Installation

1. Copy `MapForGoblins.dll` and `MapForGoblins.ini` to your ERR `dll/offline/` directory
2. Copy `addons/MapForGoblins/menu/02_120_worldmap.gfx` to ERR `addons/MapForGoblins/menu/`
All map data is compiled into the DLL itself - no external data files needed at runtime.

## Data Pipeline

The mod's map data is generated from ERR game files through a Python pipeline:

```
MSB + regulation.bin
    │
    ├─► extract_all_items.py     → items_database.json
    ├─► extract_rune_positions.py → rune_pieces.json, ember_pieces.json
    ├─► extract_itemlot_csv.py   → ItemLotParam_map.csv
    │
    ├─► generate_all_massedit.py → MASSEDIT files
    ├─► generate_pieces_massedit.py → Rune/Ember MASSEDIT + slot mappings
    │
    └─► generate_data.py → goblin_map_data.cpp + goblin_text_data.cpp
                              │
                              └─► build.bat → MapForGoblins.dll
```

### Python Setup

```bash
pip install -r requirements.txt
cp tools/config.ini.example tools/config.ini
# Edit config.ini with paths to your ERR mod and game directories
```

See [tools/README.md](tools/README.md) for detailed script documentation.

## Project Structure

```
MapForGoblins/
├── src/                    C++ DLL source code
│   ├── generated/          Auto-generated data (from Python pipeline)
│   ├── from/               Game engine structures (params, paramdefs)
│   └── goblin/             Mod-specific headers (structs, flags, tiles)
├── tracker/                RunePieceTracker - standalone piece tracking DLL
├── data/
│   ├── massedit/           MASSEDIT files (map icon definitions)
│   ├── msg/                FMG JSON translations (14 languages)
│   └── *.json, *.csv       Extracted game data
├── tools/                  Python scripts (extraction, generation, analysis)
│   ├── lib/                Andre.SoulsFormats.dll + dependencies
│   ├── paramdefs/          Elden Ring param field definitions (XML)
│   └── fmg_patcher/        C++ tool for FMG binary patching
├── assets/                 Modified game assets (worldmap GFX)
├── docs/                   Technical documentation
│   ├── KNOWLEDGE_EN.md     Knowledge base (English)
│   ├── KNOWLEDGE_RU.md     Knowledge base (Russian)
│   └── geom_collection_tracking.md  Geom object collection detection
├── CMakeLists.txt
├── build.bat
├── MapForGoblins.ini       DLL configuration (icon category toggles)
└── requirements.txt        Python dependencies
```

## Documentation

- [Knowledge Base (EN)](docs/KNOWLEDGE_EN.md) / [База знаний (RU)](docs/KNOWLEDGE_RU.md) - DLL architecture, data formats, research notes
- [Geom Collection Tracking](docs/geom_collection_tracking.md) - how collected Rune Pieces are detected from process memory
- [Tools README](tools/README.md) - Python script documentation and usage

## Credits

This project builds on the work of many people and projects:

### Game & Mod

- **FromSoftware** - Elden Ring
- **Elden Ring Reforged** team - the overhaul mod that inspired this project. Thanks to **ivi** and the ERR Discord
- **Gacsam** - [Goblin-ERR](https://github.com/Gacsam/Goblin-ERR), the original map icons mod for ERR. MapForGoblins started as a fork of this project and reuses its map fragment logic
- **Nox** - [Goblin Maps](https://www.nexusmods.com/eldenring/mods/3091), the original Elden Ring map icons mod that started it all

### Libraries & Tools

- **vawser** - [Smithbox](https://github.com/vawser/Smithbox) / Andre.SoulsFormats.dll, the From Software file format library that powers all data extraction (bundled in `tools/lib/`)
- **mountlover** - [DSMSPortable](https://github.com/mountlover/DSMSPortable), used during early development for regulation and FMG editing
- **ThomasJClark** - [elden-ring-glorious-merchant](https://github.com/ThomasJClark/elden-ring-glorious-merchant/), reference for DLL mod architecture and param injection techniques
- **Dasaav-dsv** - [Pattern16](https://github.com/Dasaav-dsv/Pattern16), AOB pattern scanner; [libER](https://github.com/Dasaav-dsv/libER), Elden Ring C++ library (referenced during development)
- **vswarte** - [fromsoftware-rs](https://github.com/vswarte/fromsoftware-rs), From Software format implementations (referenced during development)
- **TsudaKageyu** - [MinHook](https://github.com/TsudaKageyu/minhook), API hooking framework
- **gabime** - [spdlog](https://github.com/gabime/spdlog), logging library
- **metayeti** - [mINI](https://github.com/metayeti/mINI), INI file parser

### Community

Thanks to the ERR Discord for testing and bug reports, especially **AngryPhilosopher** and **Spiswel** for early testing of the DLL version.

## License

This project is provided as-is for the Elden Ring modding community. See individual library licenses for third-party dependencies.
