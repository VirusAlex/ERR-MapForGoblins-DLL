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
bool goblin::config::showSomberScarab = true;
bool goblin::config::showStoneswordKeys = true;
bool goblin::config::showUniqueDrops = true;

// Magic
bool goblin::config::showIncantations = true;
bool goblin::config::showMemoryStones = true;
bool goblin::config::showSorceries = true;

// Quest
bool goblin::config::showDeathroot = true;
bool goblin::config::showProgression = true;
bool goblin::config::showSeedbedCurses = true;

// Reforged
bool goblin::config::showCampContents = true;
bool goblin::config::showEmberPieces = true;
bool goblin::config::showItemsAndChanges = true;
bool goblin::config::showRunePieces = true;
uint8_t goblin::config::collectedSlot = 255;

// World
bool goblin::config::showGraces = true;
bool goblin::config::showHostileNPC = true;
bool goblin::config::showImpStatues = true;
bool goblin::config::showPaintings = true;
bool goblin::config::showSpiritSprings = true;
bool goblin::config::showSpiritspringHawks = true;
bool goblin::config::showSummoningPools = true;

// Bosses
bool goblin::config::showOverworldBossIcons = true;
bool goblin::config::showDungeonBossIcons = true;
bool goblin::config::showCampIcons = true;
bool goblin::config::showMerchantIcons = true;
bool goblin::config::redifyBossIcons = false;

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
        load_line(cfg, "show_somber_scarab", config::showSomberScarab);
        load_line(cfg, "show_stonesword_keys", config::showStoneswordKeys);
        load_line(cfg, "show_unique_drops", config::showUniqueDrops);
    }

    if (ini.has("Magic"))
    {
        auto &cfg = ini["Magic"];
        load_line(cfg, "show_incantations", config::showIncantations);
        load_line(cfg, "show_memory_stones", config::showMemoryStones);
        load_line(cfg, "show_sorceries", config::showSorceries);
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
        load_line(cfg, "show_camp_contents", config::showCampContents);
        load_line(cfg, "show_ember_pieces", config::showEmberPieces);
        load_line(cfg, "show_items_and_changes", config::showItemsAndChanges);
        load_line(cfg, "show_rune_pieces", config::showRunePieces);
        load_line(cfg, "collected_slot", config::collectedSlot);
    }

    if (ini.has("World"))
    {
        auto &cfg = ini["World"];
        load_line(cfg, "show_graces", config::showGraces);
        load_line(cfg, "show_hostile_npc", config::showHostileNPC);
        load_line(cfg, "show_imp_statues", config::showImpStatues);
        load_line(cfg, "show_paintings", config::showPaintings);
        load_line(cfg, "show_spirit_springs", config::showSpiritSprings);
        load_line(cfg, "show_spiritspring_hawks", config::showSpiritspringHawks);
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
