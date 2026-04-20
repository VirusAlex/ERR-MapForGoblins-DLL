#include "goblin_config.hpp"
#include <spdlog/spdlog.h>

uint8_t goblin::config::loadDelay = 15;
bool goblin::config::requireMapFragments = true;
bool goblin::config::debugLogging = false;

// Equipment
bool goblin::config::showArmaments = true;
bool goblin::config::showArmour = true;
bool goblin::config::showAshesOfWar = true;
bool goblin::config::showSpirits = true;
bool goblin::config::showTalismans = true;

// Key items
bool goblin::config::showCelestialDew = true;
bool goblin::config::showCookbooks = true;
bool goblin::config::showCrystalTears = true;
bool goblin::config::showImbuedSwordKeys = true;
bool goblin::config::showLarvalTears = true;
bool goblin::config::showScadutreeFragments = true;
bool goblin::config::showGreatRunes = true;
bool goblin::config::showLostAshes = true;
bool goblin::config::showPotsNPerfumes = true;
bool goblin::config::showSeedsTears = true;
bool goblin::config::showWhetblades = true;

// Loot
bool goblin::config::showAmmo = true;
bool goblin::config::showBellBearings = true;
bool goblin::config::showConsumables = true;
bool goblin::config::showCraftingMaterials = true;
bool goblin::config::showMPFingers = true;
bool goblin::config::showMaterialNodes = true;
bool goblin::config::showReusables = true;
bool goblin::config::showSmithingStones = true;
bool goblin::config::showSmithingStonesLow = false;
bool goblin::config::showSmithingStonesRare = true;
bool goblin::config::showGoldenRunes = true;
bool goblin::config::showGoldenRunesLow = false;
bool goblin::config::showStoneswordKeys = true;
bool goblin::config::showThrowables = true;
bool goblin::config::showPrattlingPates = true;
bool goblin::config::showRuneArcs = true;
bool goblin::config::showDragonHearts = true;
bool goblin::config::showGloveworts = true;
bool goblin::config::showGreatGloveworts = true;
bool goblin::config::showRadaFruit = true;
bool goblin::config::showGestures = true;
bool goblin::config::showGreases = true;
bool goblin::config::showUtilities = true;
bool goblin::config::showStatBoosts = true;
bool goblin::config::showFortunes = true;
bool goblin::config::showHostileNPC = true;

// Magic
bool goblin::config::showIncantations = true;
bool goblin::config::showMemoryStones = true;
bool goblin::config::showPrayerbooks = true;
bool goblin::config::showSorceries = true;

// World
bool goblin::config::showBosses = true;
bool goblin::config::hideKilledBosses = false;

// Quest
bool goblin::config::showDeathroot = true;
bool goblin::config::showProgression = true;
bool goblin::config::showSeedbedCurses = true;

// Reforged
bool goblin::config::showEmberPieces = true;
bool goblin::config::showItemsAndChanges = true;
bool goblin::config::showRunePieces = true;

// World
bool goblin::config::showGraces = true;
bool goblin::config::showImpStatues = true;
bool goblin::config::showWorldMaps = true;
bool goblin::config::showPaintings = true;
bool goblin::config::showSpiritSprings = true;
bool goblin::config::showSpiritspringHawks = true;
bool goblin::config::showStakesOfMarika = true;
bool goblin::config::showSummoningPools = true;

// Bosses
bool goblin::config::showOverworldBossIcons = true;
bool goblin::config::showDungeonBossIcons = true;
bool goblin::config::showCampIcons = true;
bool goblin::config::showMerchantIcons = true;
bool goblin::config::redifyBossIcons = false;

bool goblin::config::enableMarkerDump = true;
uint32_t goblin::config::markerDumpKey = 0x78;  // VK_F9

void goblin::load_config(const std::filesystem::path &ini_path)
{
    spdlog::info("Config: {}", ini_path.string());

    mINI::INIFile file(ini_path.string());
    mINI::INIStructure ini;
    if (!file.read(ini))
    {
        spdlog::warn("Failed to read INI file, using defaults");
        return;
    }

    if (ini.has("Goblin"))
    {
        auto &cfg = ini["Goblin"];
        load_line(cfg, "load_delay", config::loadDelay);
        load_line(cfg, "require_map_fragments", config::requireMapFragments);
        load_line(cfg, "debug_logging", config::debugLogging);
    }

    if (ini.has("Equipment"))
    {
        auto &cfg = ini["Equipment"];
        load_line(cfg, "show_armaments", config::showArmaments);
        load_line(cfg, "show_armour", config::showArmour);
        load_line(cfg, "show_ashes_of_war", config::showAshesOfWar);
        load_line(cfg, "show_spirits", config::showSpirits);
        load_line(cfg, "show_talismans", config::showTalismans);
    }

    if (ini.has("Key Items"))
    {
        auto &cfg = ini["Key Items"];
        load_line(cfg, "show_celestial_dew", config::showCelestialDew);
        load_line(cfg, "show_cookbooks", config::showCookbooks);
        load_line(cfg, "show_crystal_tears", config::showCrystalTears);
        load_line(cfg, "show_imbued_sword_keys", config::showImbuedSwordKeys);
        load_line(cfg, "show_larval_tears", config::showLarvalTears);
        load_line(cfg, "show_scadutree_fragments", config::showScadutreeFragments);
        load_line(cfg, "show_great_runes", config::showGreatRunes);
        load_line(cfg, "show_lost_ashes", config::showLostAshes);
        load_line(cfg, "show_pots_n_perfumes", config::showPotsNPerfumes);
        load_line(cfg, "show_seeds_tears", config::showSeedsTears);
        load_line(cfg, "show_whetblades", config::showWhetblades);
    }

    if (ini.has("Loot"))
    {
        auto &cfg = ini["Loot"];
        load_line(cfg, "show_ammo", config::showAmmo);
        load_line(cfg, "show_bell_bearings", config::showBellBearings);
        load_line(cfg, "show_consumables", config::showConsumables);
        load_line(cfg, "show_crafting_materials", config::showCraftingMaterials);
        load_line(cfg, "show_mp_fingers", config::showMPFingers);
        load_line(cfg, "show_material_nodes", config::showMaterialNodes);
        load_line(cfg, "show_reusables", config::showReusables);
        load_line(cfg, "show_smithing_stones", config::showSmithingStones);
        load_line(cfg, "show_smithing_stones_low", config::showSmithingStonesLow);
        load_line(cfg, "show_smithing_stones_rare", config::showSmithingStonesRare);
        load_line(cfg, "show_golden_runes", config::showGoldenRunes);
        load_line(cfg, "show_golden_runes_low", config::showGoldenRunesLow);
        load_line(cfg, "show_stonesword_keys", config::showStoneswordKeys);
        load_line(cfg, "show_throwables", config::showThrowables);
        load_line(cfg, "show_prattling_pates", config::showPrattlingPates);
        load_line(cfg, "show_rune_arcs", config::showRuneArcs);
        load_line(cfg, "show_dragon_hearts", config::showDragonHearts);
        load_line(cfg, "show_gloveworts", config::showGloveworts);
        load_line(cfg, "show_great_gloveworts", config::showGreatGloveworts);
        load_line(cfg, "show_rada_fruit", config::showRadaFruit);
        load_line(cfg, "show_gestures", config::showGestures);
        load_line(cfg, "show_greases", config::showGreases);
        load_line(cfg, "show_utilities", config::showUtilities);
        load_line(cfg, "show_stat_boosts", config::showStatBoosts);
    }

    if (ini.has("Magic"))
    {
        auto &cfg = ini["Magic"];
        load_line(cfg, "show_incantations", config::showIncantations);
        load_line(cfg, "show_memory_stones", config::showMemoryStones);
        load_line(cfg, "show_prayerbooks", config::showPrayerbooks);
        load_line(cfg, "show_sorceries", config::showSorceries);
    }

    if (ini.has("World"))
    {
        auto &cfg = ini["World"];
        load_line(cfg, "show_bosses", config::showBosses);
        load_line(cfg, "hide_killed_bosses", config::hideKilledBosses);
    }

    if (ini.has("Quest"))
    {
        auto &cfg = ini["Quest"];
        load_line(cfg, "show_deathroot", config::showDeathroot);
        load_line(cfg, "show_progression", config::showProgression);
        load_line(cfg, "show_seedbed_curses", config::showSeedbedCurses);
    }

    if (ini.has("Reforged"))
    {
        auto &cfg = ini["Reforged"];
        load_line(cfg, "show_ember_pieces", config::showEmberPieces);
        load_line(cfg, "show_items_and_changes", config::showItemsAndChanges);
        load_line(cfg, "show_rune_pieces", config::showRunePieces);
        load_line(cfg, "show_fortunes", config::showFortunes);
    }

    if (ini.has("World"))
    {
        auto &cfg = ini["World"];
        load_line(cfg, "show_graces", config::showGraces);
        load_line(cfg, "show_hostile_npc", config::showHostileNPC);
        load_line(cfg, "show_imp_statues", config::showImpStatues);
        load_line(cfg, "show_world_maps", config::showWorldMaps);
        load_line(cfg, "show_paintings", config::showPaintings);
        load_line(cfg, "show_spirit_springs", config::showSpiritSprings);
        load_line(cfg, "show_spiritspring_hawks", config::showSpiritspringHawks);
        load_line(cfg, "show_stakes_of_marika", config::showStakesOfMarika);
        load_line(cfg, "show_summoning_pools", config::showSummoningPools);
    }

    if (ini.has("Bosses"))
    {
        auto &cfg = ini["Bosses"];
        load_line(cfg, "show_overworld_boss_icons", config::showOverworldBossIcons);
        load_line(cfg, "show_dungeon_boss_icons", config::showDungeonBossIcons);
        load_line(cfg, "show_camp_icons", config::showCampIcons);
        load_line(cfg, "show_merchant_icons", config::showMerchantIcons);
        load_line(cfg, "redify_boss_icons", config::redifyBossIcons);
    }

    if (ini.has("Debug"))
    {
        auto &cfg = ini["Debug"];
        load_line(cfg, "enable_marker_dump", config::enableMarkerDump);
        if (cfg.has("marker_dump_key"))
        {
            uint32_t vk = parse_vk_code(cfg["marker_dump_key"]);
            if (vk) config::markerDumpKey = vk;
            spdlog::debug("Config: marker_dump_key = 0x{:X}", config::markerDumpKey);
        }
    }
}

// Parse a human key name ("F9", "A", "Home", "0", "Space") into Win32 VK_* code.
// Returns 0 on unknown name.
uint32_t goblin::parse_vk_code(std::string name)
{
    for (auto &c : name)
        if (c >= 'a' && c <= 'z') c -= 32;  // uppercase

    if (name.empty()) return 0;

    // Function keys F1..F24
    if (name.size() >= 2 && name[0] == 'F')
    {
        try
        {
            int n = std::stoi(name.substr(1));
            if (n >= 1 && n <= 24) return 0x6F + n;  // VK_F1=0x70, so 0x6F+n
        }
        catch (...) {}
    }

    // Single letter A-Z
    if (name.size() == 1 && name[0] >= 'A' && name[0] <= 'Z')
        return static_cast<uint32_t>(name[0]);

    // Single digit 0-9
    if (name.size() == 1 && name[0] >= '0' && name[0] <= '9')
        return static_cast<uint32_t>(name[0]);

    // Named keys
    static const std::pair<const char *, uint32_t> named[] = {
        {"SPACE", 0x20}, {"ESCAPE", 0x1B}, {"ESC", 0x1B},
        {"TAB", 0x09}, {"ENTER", 0x0D}, {"RETURN", 0x0D}, {"BACKSPACE", 0x08},
        {"HOME", 0x24}, {"END", 0x23},
        {"PAGEUP", 0x21}, {"PAGEDOWN", 0x22}, {"INSERT", 0x2D}, {"DELETE", 0x2E},
        {"UP", 0x26}, {"DOWN", 0x28}, {"LEFT", 0x25}, {"RIGHT", 0x27},
    };
    for (auto &p : named)
        if (name == p.first) return p.second;
    return 0;
}

void goblin::load_line(mINI::INIMap<std::string> config, std::string lineInIni, bool &boolVariable)
{
    if (config.has(lineInIni))
    {
        boolVariable = config[lineInIni] != "false";
        spdlog::debug("Config: {} = {}", lineInIni, boolVariable);
    }
}

void goblin::load_line(mINI::INIMap<std::string> config, std::string lineInIni, uint8_t &intVariable)
{
    if (config.has(lineInIni))
    {
        int val = std::stoi(config[lineInIni]);
        if (val < 0 || val > 255)
            val = 15;
        intVariable = static_cast<uint8_t>(val);
        spdlog::debug("Config: {} = {}", lineInIni, intVariable);
    }
}
