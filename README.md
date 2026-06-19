# ELDEN RING Map For Goblins - DLL

<p align="center">
  <a href="https://github.com/VirusAlex/ERR-MapForGoblins-DLL/releases/latest"><img src="https://img.shields.io/github/v/release/VirusAlex/ERR-MapForGoblins-DLL?label=release" alt="Latest release"></a>
  <a href="https://github.com/VirusAlex/ERR-MapForGoblins-DLL/releases"><img src="https://img.shields.io/github/downloads/VirusAlex/ERR-MapForGoblins-DLL/total" alt="Downloads"></a>
  <a href="https://www.nexusmods.com/eldenring/mods/10062"><img src="https://img.shields.io/badge/Nexus%20Mods-MapForGoblins-da8e35?logo=nexusmods&logoColor=white" alt="Nexus Mods"></a>
  <a href="https://discord.gg/JvTMwPCygB"><img src="https://img.shields.io/badge/Discord-Elden%20Ring%20DLL%20Mods-5865F2?logo=discord&logoColor=white" alt="Discord"></a>
  <a href="LICENSE.txt"><img src="https://img.shields.io/badge/license-MIT--style-blue" alt="License"></a>
</p>

A DLL mod for Elden Ring that adds thousands of icons to the world map: weapons, armor, spells, quest items, bosses, NPCs, gathering nodes, etc. Four builds: one for [ERR](https://www.nexusmods.com/eldenring/mods/541) (~9000 icons, including ERR-specific content like Rune Pieces), one for the vanilla game + Shadow of the Erdtree (~6700 icons), one for [The Convergence](https://www.nexusmods.com/eldenring/mods/3419) (~7200 icons generated from the overhaul's own data), and one for [ERTE](https://www.nexusmods.com/eldenring/mods/2747) (~7600 icons generated from the overhaul's own data).

**Download:** [Nexus Mods](https://www.nexusmods.com/eldenring/mods/10062) · **Community:** [Elden Ring - DLL Mods Discord](https://discord.gg/JvTMwPCygB)

<p align="center">
  <img src="assets/screenshots/mod-menu.png" alt="The in-game mod menu (F10 / Y+R3): toggle any icon category live" width="440">
  <br>
  <em>In-game mod menu (F10 / Y+R3): toggle any icon category live, no ini editing.</em>
</p>

Unlike [Map for Goblins](https://www.nexusmods.com/eldenring/mods/3091), this mod does not modify `regulation.bin`. All map point data is injected into memory at runtime, so it won't conflict with other regulation edits.

> **Note:** OFFLINE only. This is an unofficial mod, not affiliated with the ERR team, the Convergence Team, or the ERTE author.

Collected Rune Pieces, Ember Pieces and gathering nodes are automatically hidden on the map using real-time memory detection of the game's geometry object state.

## Features

- In-game mod menu (Dear ImGui on a DX12 Present hook): open with **F10** (keyboard) or **Y+R3** (gamepad) to toggle any icon category, flip the master switch, use Show all / Hide all, and copy a marker dump - no ini editing or alt-tab. Mouse/keyboard/controller input; category toggles take effect right away on the open map (some options apply on the next map open)
- ~9000 map icons across 60+ toggleable categories (configurable via INI or the overlay)
- Map text sourced from existing in-game FMG entries (all 14 languages) via a MsgRepository hook - each marker redirects to a goods/weapon/armour/etc. name by ID, so translations come for free
- Collected Rune/Ember Piece detection: GEOF singletons for unloaded tiles + CSWorldGeomMan flags for loaded tiles
- [Item & Enemy Randomizer](https://www.nexusmods.com/eldenring/mods/428) support (vanilla build, on by default): loot markers read the loaded `ItemLotParam` from live memory at startup, so each shows the item actually placed by your seed (name + icon) and hides on the real light-point pickup - seed-agnostic, no per-seed data
- Spoiler-free mode (`anonymous_loot` INI option): every loot marker shows a gray "?" icon and a generic localized label instead of the real item, for blind / randomizer runs
- No regulation.bin changes - no conflicts with other mods
- Addon-compatible folder structure for ERR

## Building

Requirements:
- Visual Studio 2022 (Build Tools or Community)
- CMake 3.28+
- Internet connection (CMake fetches dependencies on first configure)

```bash
build.bat              # configure + build
build.bat snapshot     # run the full data pipeline + build + package into pre-release/
build.bat release      # same as snapshot, but non-pre version + bumps patch version
build.bat generate     # run the data pipeline only (no DLL build)
build.bat clean        # delete build directory
```

Every command builds the ERR profile by default. Append `--vanilla`,
`--convergence`, or `--erte` to build the other profiles (own data/source/build/
package dirs; see `tools/config.ini.example` for the required paths). The
Convergence and ERTE profiles stage a merged overlay-over-vanilla source view
first, since those overhauls ship a partial ModEngine overlay.

Output: `build/Release/MapForGoblins.dll` + `MapForGoblins.ini`

## Installation

Grab a packaged release from [Nexus Mods](https://www.nexusmods.com/eldenring/mods/10062) - it has step-by-step instructions for all four builds (ERR; vanilla via ModEngine2/me3; The Convergence via its bundled ModEngine2; ERTE via Mod Engine 3).

Manual install of the ERR build:
1. Copy `MapForGoblins.dll` and `MapForGoblins.ini` to your ERR `dll/offline/` directory
2. Copy `addons/MapForGoblins/menu/02_120_worldmap.gfx` to ERR `addons/MapForGoblins/menu/`
All map data is compiled into the DLL itself - no external data files needed at runtime.

## Data Pipeline

The mod's map data is generated from ERR game files through a Python pipeline
orchestrated by `tools/build_pipeline.py` (18 stages, hash-based incremental cache):

```
MSB + regulation.bin + EMEVD
    │
    ├─► extract_all_items.py        → items_database.json
    ├─► build_entity_index.py       → msb_entity_index.json
    ├─► scan_emevd_awards.py        → emevd_lot_mapping.json
    ├─► enrich_fallback_with_emevd.py (upgrades unmatched records in-place)
    │
    ├─► generate_loot_massedit.py   → 50+ Loot/Equipment/Key/Quest/Magic MASSEDIT
    ├─► generate_pieces_massedit.py → Rune/Ember MASSEDIT + slot mappings
    ├─► generate_material_nodes.py, generate_graces.py, generate_summoning_pools.py,
    │   generate_spirit_springs.py, generate_imp_statues.py, generate_stakes.py,
    │   generate_paintings.py, generate_maps.py, generate_gestures.py,
    │   generate_hostile_npcs.py    → world-infrastructure MASSEDIT
    │
    └─► generate_data.py → goblin_map_data.cpp + goblin_legacy_conv.hpp
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
│   ├── massedit_generated/ MASSEDIT files (auto-generated map icon definitions)
│   └── *.json, *.csv       Extracted game data (items, entity index, EMEVD map, ...)
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
- **Elden Ring Reforged** team - the overhaul mod that inspired this project. Thanks to [**ividyon**](https://github.com/ividyon) and the ERR Discord
- **Gacsam** - [Goblin-ERR](https://github.com/Gacsam/Goblin-ERR), the original map icons mod for ERR. MapForGoblins started as a fork of this project and reuses its map fragment logic
- **Harmonixer** - [Map for Goblins](https://www.nexusmods.com/eldenring/mods/3091), the original Elden Ring map icons mod that started it all
- **Convergence Team** - [The Convergence](https://www.nexusmods.com/eldenring/mods/3419), the overhaul the Convergence build targets
- **ERTE author** - [ERTE](https://www.nexusmods.com/eldenring/mods/2747), the overhaul the ERTE build targets

### Libraries & Tools

- **vawser** - [Smithbox](https://github.com/vawser/Smithbox) / Andre.SoulsFormats.dll, the From Software file format library that powers all data extraction (bundled in `tools/lib/`)
- **mountlover** - [DSMSPortable](https://github.com/mountlover/DSMSPortable), used during early development for regulation and FMG editing
- **ThomasJClark** - [elden-ring-glorious-merchant](https://github.com/ThomasJClark/elden-ring-glorious-merchant/), reference for DLL mod architecture and param injection techniques
- **Dasaav-dsv** - [Pattern16](https://github.com/Dasaav-dsv/Pattern16), AOB pattern scanner; [libER](https://github.com/Dasaav-dsv/libER), Elden Ring C++ library (referenced during development)
- **vswarte** - [fromsoftware-rs](https://github.com/vswarte/fromsoftware-rs), From Software format implementations (referenced during development)
- **TsudaKageyu** - [MinHook](https://github.com/TsudaKageyu/minhook), API hooking framework
- **gabime** - [spdlog](https://github.com/gabime/spdlog), logging library
- **metayeti** - [mINI](https://github.com/metayeti/mINI), INI file parser
- **ocornut** - [Dear ImGui](https://github.com/ocornut/imgui), the in-game config overlay UI
- **[Claude Code](https://claude.com/claude-code)** (Anthropic) - heavy lifting on the data-extraction pipeline automation and on reverse-engineering the game's in-memory geom-object state (the collected-piece detection research)

### Community

Thanks to the ERR Discord for testing and bug reports, especially **AngryPhilosopher** and **Spiswel** for early testing of the DLL version.

## License

MIT-style, see [LICENSE.txt](LICENSE.txt) - includes the original [Goblin-ERR](https://github.com/Gacsam/Goblin-ERR) notice (this project started as its fork) and the bundled third-party licenses (Pattern16, MinHook, HDE64, mINI, spdlog, Dear ImGui).
