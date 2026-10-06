# ELDEN RING Map For Goblins - DLL

<p align="center">
  <a href="https://github.com/VirusAlex/ERR-MapForGoblins-DLL/releases/latest"><img src="https://img.shields.io/github/v/release/VirusAlex/ERR-MapForGoblins-DLL?label=release" alt="Latest release"></a>
  <a href="https://github.com/VirusAlex/ERR-MapForGoblins-DLL/releases"><img src="https://img.shields.io/github/downloads/VirusAlex/ERR-MapForGoblins-DLL/total" alt="Downloads"></a>
  <a href="https://www.nexusmods.com/eldenring/mods/10062"><img src="https://img.shields.io/badge/Nexus%20Mods-MapForGoblins-da8e35?logo=nexusmods&logoColor=white" alt="Nexus Mods"></a>
  <a href="https://discord.gg/JvTMwPCygB"><img src="https://img.shields.io/badge/Discord-Elden%20Ring%20DLL%20Mods-5865F2?logo=discord&logoColor=white" alt="Discord"></a>
  <a href="LICENSE.txt"><img src="https://img.shields.io/badge/license-MIT--style-blue" alt="License"></a>
</p>

DLL version of the [Map for Goblins](https://www.nexusmods.com/eldenring/mods/3091) mod. Thousands of loot and world icons on the in-game map, and **no regulation.bin changes**.

> **OFFLINE ONLY.** This mod is unofficial: it is not affiliated with the ERR team, the Convergence Team or any other overhaul authors, and they don't support it. Compatible with [Seamless Co-op](https://www.nexusmods.com/eldenring/mods/510).

**Download:** [Nexus Mods](https://www.nexusmods.com/eldenring/mods/10062) · **Questions and bug reports:** [Discord server](https://discord.gg/JvTMwPCygB)

This mod supports many overhauls, pick the one for your game:

- **Vanilla version**, for the base game + Shadow of the Erdtree (not required). ~7400 icons. Also works with the [Item & Enemy Randomizer](https://www.nexusmods.com/eldenring/mods/428)
- **ERR version**, for [ERR](https://www.nexusmods.com/eldenring/mods/541). ~9700 icons, including the ERR-specific stuff (Rune Pieces, Ember Pieces, Kindling Spirits, unique loot etc)
- **Convergence version**, for [The Convergence](https://www.nexusmods.com/eldenring/mods/3419) (3.x, me3), ~8700 icons. Includes the new/relocated loot, new bosses and reworked zones
- **ERTE version**, for [ERTE](https://www.nexusmods.com/eldenring/mods/2747), ~8800 icons
- **Golden Age version**, for the [Elden Ring Golden Age](https://afdian.com/a/moke16864) overhaul (me3), ~7600 icons
- **ELDEN VINS version**, for the [ELDEN VINS](https://www.nexusmods.com/eldenring/mods/4709) overhaul (me3), ~8000 icons
- **Elden Ring Reborn version**, for the [Elden Ring Reborn](https://www.nexusmods.com/eldenring/mods/2202) overhaul, ~7600 icons
- **Graceborne version**, for the [Graceborne](https://www.nexusmods.com/eldenring/mods/5207) Bloodborne-inspired overhaul (me3), ~7400 icons
- **DM Throne version**, for the [Dark Moon: Throne](https://www.patreon.com/cw/puyuan) overhaul (me3), ~7500 icons

## In-game mod menu

<p align="center">
  <img src="assets/screenshots/mod-menu.jpg" alt="The mod menu: the native in-game menu and the ImGui overlay" width="880">
</p>

Press **F10** (keyboard) or **Y+R3** (controller) to open the mod menu inside the game. There you can:

- Turn any icon category on or off (or use **Show all** / **Hide all**)
- Track your completion on the **Progress** page: collected/total per region (grouped into The Lands Between, the Underground and Shadow of the Erdtree), and click a category to highlight only its uncollected markers right on the map
- Hide individual markers you don't care about, and bring them back, from the **Hidden** page
- Search any known item by its name from the **Search** page
- Flip the master "Show map icons" switch
- Change the mod's settings - map fragment requirement, killed bosses, spoiler-free mode etc
- Copy a marker dump for bug reports

**Toggling a category takes effect immediately** - its icons appear or disappear without closing the map. Mouse, keyboard and controller supported. By default the menu is drawn by the game itself; `menu_render_mode = imgui` in the ini switches to the older overlay window instead.

## Main features

- Icon **categories** that can be turned on/off individually: weapons, armour, talismans, spells, key items, smithing stones, gloveworts, cookbooks, bell bearings, gathering nodes, bosses, graces, stakes of Marika, spirit springs, imp statues, paintings, summoning pools, map fragments, gestures, NPC invaders, strong enemies, puzzles etc
- Icons in a region only show up after you've found that region's map fragment (can be disabled)
- Things you've already collected disappear from the map: including gathered **material nodes** (like Arteria Leaf), Rune/Ember Pieces etc. Defeated bosses either hide or get a checkmark (configurable)
- **Progress tracking**: the Progress page shows how much of each category you've collected per region, with small caves/catacombs folded into their region
- **Items search**: find and highlight any item by name (merchant stock is not included)
- **Hide markers** you don't want: hover a marker on the world map and press **Delete** (or **RB** on a controller). Hidden markers persist and are saved per character
- Hovering a marker shows a **small tooltip** with how far above or below you it is
- **Focus** on your current location: markers of the map you are on are drawn bigger and brighter, markers of another map smaller and dimmer. Configurable in the mod menu
- **Live marker de-overlapping**: overlapping icons are spread apart using only the markers visible at that moment, so a category you turn off frees its space
- Every marker shows its location name, down to sub-areas (like Siofra Aqueduct / Siofra River). The mod uses the game's own strings, so everything is **localised** out of the box; the mod's own menu is translated into 9 languages (English, Chinese (Simplified/Traditional), Korean, Russian, German, French, Spanish, Vietnamese)
- The INI maintains itself: if there's no file, it gets created; new options after an update are added automatically
- **Item & Enemy Randomizer support** (vanilla build, on by default): loot markers read the loaded regulation at startup, so each one shows the item that's *actually* there - right name, right icon - and disappears when you pick up the real light point. Works with any seed, no per-seed setup
- **Spoiler-free mode** (optional, `anonymous_loot` in the ini): every loot marker shows a plain "?" icon and a generic label instead of the real item - made for blind / randomizer runs

### How it works

Unlike [Map for Goblins](https://www.nexusmods.com/eldenring/mods/3091), this mod does not touch `regulation.bin` or the world map `.gfx`: all map points and icon frames are injected into memory at runtime, so it doesn't conflict with other regulation edits. Marker text redirects to the game's own FMG entries (goods/weapon/armour/place names by id) through a MsgRepository hook. Collected pieces and nodes are detected live from the game's geometry-object state (GEOF singletons for unloaded tiles, CSWorldGeomMan flags for loaded ones).

## Building

Requirements:
- Visual Studio 2022 (Build Tools or Community)
- CMake 3.28+
- Internet connection (CMake fetches dependencies on first configure)

All nine profiles in parallel (the shared art/i18n step runs once, then each profile's
pipeline + compile):

```bash
py tools/build_all.py snapshot                      # every profile
py tools/build_all.py snapshot --profiles vanilla   # a subset, comma-separated
```

One profile through `build.bat` (run it from PowerShell):

```bash
build.bat              # configure + build
build.bat snapshot     # full data pipeline + build + package into releases/pre-release-<profile>/
build.bat release      # same as snapshot, but a non-pre version + bumps the patch version
build.bat generate     # run the data pipeline only (no DLL build)
build.bat clean        # delete the profile's build directory
```

`build.bat` builds the ERR profile by default. Append `--vanilla`, `--convergence3`,
`--erte`, `--goldenage`, `--vins`, `--reborn`, `--graceborne` or `--throne` to build
another profile (own data/source/build/package dirs; see `tools/config.ini.example` for
the required paths). The overhaul profiles stage a merged overlay-over-vanilla source view
first, since those overhauls ship a partial ModEngine overlay. `--convergence2`
(The Convergence 2.x) still exists but is unpublished and not part of the default set.

Output: `builds/build[-<profile>]/Release/MapForGoblins.dll`; the ini is written by
`mfg_inigen` at packaging time, and the DLL also creates/repairs it on first run.

## Installation

Grab a packaged release from [Nexus Mods](https://www.nexusmods.com/eldenring/mods/10062) - it has step-by-step instructions for every build (ERR; vanilla via ModEngine2/me3; the overhauls - Convergence, ERTE, Golden Age, ELDEN VINS, Reborn, Graceborne, Dark Moon: Throne - via their bundled ModEngine2 / Mod Engine 3).

The mod is a single DLL (no gfx or extra files) - manual install of the ERR build:
1. Copy `MapForGoblins.dll` and `MapForGoblins.ini` to your ERR `dll/offline/` directory.
All map data is compiled into the DLL itself - no external data files needed at runtime.

## Data Pipeline

Each profile's map data is generated from that game/mod's own files (params, MSB, EMEVD,
FMG) through a Python pipeline orchestrated by `tools/build_pipeline.py` (37 stages,
hash-based incremental cache; ERR-only stages such as the Rune/Ember Pieces skip
themselves on the other profiles):

```
regulation.bin + MSB + EMEVD + FMG  (+ committed inputs/)
    │
    ├─► extract_goods_categories, extract_tutorial_codex, extract_placename_dump,
    │   extract_english_fallback, extract_itemlot_csv       → category / text tables
    ├─► extract_rune_positions, finalize_pieces (ERR)       → Rune/Ember Piece positions
    ├─► build_entity_index, scan_emevd_awards, extract_all_items, build_grace_index,
    │   enrich_fallback_with_emevd                          → items database + entity/award index
    │
    ├─► generate_boss_list, generate_relocating_boss_fix, generate_loot, generate_pieces
    ├─► scan_all_gathering_nodes, scan_gathering_node_flags, generate_material_nodes
    ├─► generate_graces, generate_summoning_pools, generate_kindling_spirits,
    │   generate_spirit_springs, generate_imp_statues, generate_stakes,
    │   extract_seal_puzzles, generate_seal_puzzles, generate_hero_tomb_statues,
    │   generate_paintings, generate_maps, generate_gestures, generate_hostile_npcs,
    │   generate_strong_enemies                             → data/<profile>/rows_generated/*.rows
    │
    ├─► generate_data   → goblin_map_blob_data.cpp (one deflated table) + goblin_legacy_conv.hpp
    ├─► generate_region_map, generate_geof_models, generate_location_overrides
    │
    └─► CMake / MSBuild → MapForGoblins.dll
```

The profile-independent art and text (logo, map and overlay icons, menu icon tags, i18n
bundles) are generated once into `src/generated_shared/`.

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
│   ├── generated/          ERR baked data (regenerated by the pipeline, gitignored)
│   ├── generated_<profile>/  The same for every other profile (gitignored)
│   ├── generated_shared/   Profile-independent art + i18n (gitignored)
│   ├── from/               Game engine structures (params, paramdefs)
│   ├── goblin/             Mod-specific headers (structs, flags, tiles)
│   ├── sc2/                In-swapchain backend of the optional ImGui overlay
│   └── vendor_miniz/       miniz build config
├── inputs/                 Committed pipeline inputs nothing regenerates (unprojectable tiles,
│   └── <profile>/          invader overrides, model aliases, enemy names i18n; per-profile extras)
├── data/                   Pipeline workspace, gitignored: everything here is regenerated
│   └── <profile>/          Per-profile workspace (err, vanilla, convergence3, ...)
│       ├── rows_generated/ Generated marker rows (binary, one file per category)
│       └── *.json, *.csv   Extracted game data (items, entity index, EMEVD map, ...)
├── i18n/                   The mod's own UI strings, one JSON per language
├── tools/                  Python scripts (extraction, generation, build orchestration, analysis)
│   ├── lib/                Andre.SoulsFormats.dll + dependencies
│   └── paramdefs/          Elden Ring param field definitions (XML)
├── assets/                 Icon art (map_icons/custom/), badges, logo, per-build README_*.txt
│                           shipped in the packages, Nexus page text
├── docs/                   Technical documentation and research notes
│   ├── KNOWLEDGE_EN.md     Knowledge base (English)
│   ├── KNOWLEDGE_RU.md     Knowledge base (Russian)
│   └── geom_collection_tracking.md  Geom object collection detection
├── sdk/                    Menu add-on API header (preview; the host is not compiled into current builds)
├── cmake/                  Build-identity stamping
├── builds/                 CMake build trees, one per profile (gitignored)
├── releases/               Packaged snapshots and releases (gitignored)
├── CMakeLists.txt
├── build.bat
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
- **Elden Ring Golden Age** team - [Elden Ring Golden Age](https://afdian.com/a/moke16864), the overhaul the Golden Age build targets
- **mayk** - [ELDEN VINS](https://www.nexusmods.com/eldenring/mods/4709), the overhaul the ELDEN VINS build targets
- **H A L C Y O N** - [Elden Ring Reborn](https://www.nexusmods.com/eldenring/mods/2202), the overhaul the Reborn build targets
- **Noctis** - [Graceborne](https://www.nexusmods.com/eldenring/mods/5207), the Bloodborne-inspired overhaul the Graceborne build targets
- **Dark Moon: Throne** team - [Dark Moon: Throne](https://www.patreon.com/cw/puyuan), the overhaul the Throne build targets

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

### Contributors

- **[yun-wulian](https://github.com/yun-wulian)** - Chinese (Simplified & Traditional) overlay localization and an in-menu multi-controller gamepad fix.
- **[yeousherang](https://github.com/yeousherang)** - Korean overlay localization.

### Community

Thanks to the ERR Discord for testing and bug reports, especially **AngryPhilosopher** and **Spiswel** for early testing of the DLL version, and **darksucklet** for help debugging the geom-object (collected-piece) tracking.

## License

MIT-style, see [LICENSE.txt](LICENSE.txt) - includes the original [Goblin-ERR](https://github.com/Gacsam/Goblin-ERR) notice (this project started as its fork) and the bundled third-party licenses (Pattern16, MinHook, HDE64, mINI, spdlog, Dear ImGui).
