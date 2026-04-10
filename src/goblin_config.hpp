#pragma once

#include <filesystem>
#include <mini/ini.h>

namespace goblin
{
    void load_line(mINI::INIMap<std::string> config, std::string lineInIni, bool &lineVariable);
    void load_line(mINI::INIMap<std::string> config, std::string lineInIni, uint8_t &intVariable);
    void load_config(const std::filesystem::path &ini_path);

    namespace config
    {
        extern uint8_t loadDelay;
        extern bool requireMapFragments;
        extern bool debugLogging;

        // Equipment
        extern bool showArmaments;
        extern bool showArmour;
        extern bool showAshesOfWar;
        extern bool showSpirits;
        extern bool showTalismans;

        // Key items
        extern bool showCelestialDew;
        extern bool showCookbooks;
        extern bool showCrystalTears;
        extern bool showImbuedSwordKeys;
        extern bool showLarvalTears;
        extern bool showLostAshes;
        extern bool showPotsNPerfumes;
        extern bool showSeedsTears;
        extern bool showWhetblades;

        // Loot
        extern bool showAmmo;
        extern bool showBellBearings;
        extern bool showConsumables;
        extern bool showCraftingMaterials;
        extern bool showMPFingers;
        extern bool showMaterialNodes;
        extern bool showReusables;
        extern bool showSomberScarab;
        extern bool showStoneswordKeys;
        extern bool showUniqueDrops;

        // Magic
        extern bool showIncantations;
        extern bool showMemoryStones;
        extern bool showSorceries;

        // Quest
        extern bool showDeathroot;
        extern bool showProgression;
        extern bool showSeedbedCurses;

        // Reforged
        extern bool showCampContents;
        extern bool showEmberPieces;
        extern bool showItemsAndChanges;
        extern bool showRunePieces;
        extern uint8_t collectedSlot; // 255 = auto (largest GEOF section)

        // World
        extern bool showGraces;
        extern bool showHostileNPC;
        extern bool showImpStatues;
        extern bool showPaintings;
        extern bool showSpiritSprings;
        extern bool showSpiritspringHawks;
        extern bool showSummoningPools;

        // Boss/camp/merchant settings
        extern bool showOverworldBossIcons;
        extern bool showDungeonBossIcons;
        extern bool showCampIcons;
        extern bool showMerchantIcons;
        extern bool redifyBossIcons;
    };
};
