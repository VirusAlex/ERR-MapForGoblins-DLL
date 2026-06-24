// AUTHORITATIVE ini layout - see goblin_config_schema.hpp.
// Dependency-free (no spdlog / mINI) so tools/mfg_inigen can link just this.

#include "goblin_config_schema.hpp"
#include "goblin_config.hpp"

#include <cstring>

// ── config variable definitions ─────────────────────────────────────────
// Initial values are seeded from the schema defaults by apply_defaults() at
// load time; the literals here are just a sane fallback.
namespace goblin::config
{
    bool requireMapFragments = true;
    bool debugLogging = false;        // key debug_logging: verbose diagnostics; also gates the dev-only
                                      // worldmap SpriteDef/dict dumps + RM2::Execute trace in goblin_gfx_probe.
    bool fastMapOpen = true;          // key fast_map_open: skip redundant relayout on re-open + amortize the first open
    // (icon/resource injection is unconditional - it IS how icons render without a gfx; no ini toggle.)

    bool showArmaments = true, showArmour = true, showAshesOfWar = true,
         showSpirits = true, showTalismans = true;

    bool showCelestialDew = true, showCookbooks = true, showCrystalTears = true,
         showGreatRunes = true, showImbuedSwordKeys = true, showLarvalTears = true,
         showLostAshes = true, showPotsNPerfumes = true, showScadutreeFragments = true,
         showSeedsTears = true, showWhetblades = true;

    bool showAmmo = true, showBellBearings = true, showMerchantBellBearings = true,
         showConsumables = true, showGreases = true, showUtilities = true,
         showStatBoosts = true, showCraftingMaterials = true, showGloveworts = true,
         showGoldenRunes = true, showGoldenRunesLow = true, showGreatGloveworts = true,
         showMaterialNodes = true, showMPFingers = true, showPrattlingPates = true,
         showGestures = true, showReusables = true,
         showSmithingStones = true, showSmithingStonesLow = true,
         showSmithingStonesRare = true, showStoneswordKeys = true,
         showThrowables = true, showRuneArcs = true, showDragonHearts = true;

    bool showIncantations = true, showMemoryStones = true, showPrayerbooks = true,
         showSorceries = true;

    bool showDeathroot = true, showProgression = true, showSeedbedCurses = true;

    bool showEmberPieces = true, showItemsAndChanges = true, showFortunes = true,
         showRunePieces = true;

    bool showBosses = true, showGraces = true, showHostileNPC = true,
         showImpStatues = true, showPaintings = true, showSpiritSprings = true,
         showSpiritspringHawks = true, showStakesOfMarika = true,
         showSummoningPools = true, showKindlingSpirits = true,
         showInteractables = true, showWorldMaps = true, hideKilledBosses = false;

    // Live-loot / randomizer-compat options default ON for the plain VANILLA
    // build only (that's what Item/Enemy Randomizer players use) and OFF for ERR
    // and Convergence (opt-in - they add memory/CPU with no benefit without a
    // regulation mod). MFG_PROFILE_VANILLA is set by CMake for the vanilla bake
    // only (MFG_VANILLA covers vanilla AND convergence, so it can't be used here).
#ifdef MFG_PROFILE_VANILLA
    bool liveLootFlags = true, liveLootLabels = true, liveLootIcons = true;
#else
    bool liveLootFlags = false, liveLootLabels = false, liveLootIcons = false;
#endif
    bool anonymousLoot = false;  // opt-in spoiler-free mode (all profiles default off)

    bool patchOverworldBossIcons = true, patchDungeonBossIcons = true,
         patchCampIcons = true, patchMerchantIcons = true,
         hideDungeonIconsOnClear = false;

    bool enableOverlay = true;
    bool enableMarkerDump = false;
    uint32_t markerDumpKey = 0x78; // VK_F9
    bool enableToggleHotkey = true;
    uint32_t toggleInjectionKey = 0x79; // VK_F10
    uint16_t toggleGamepadMask = 0x8000 | 0x0080; // Y + R3
}

// ── schema ───────────────────────────────────────────────────────────────
namespace
{
    using goblin::IniEntry;
    using goblin::IniSection;
    using goblin::IniType;
    namespace cfg = goblin::config;

    constexpr bool ERR = true; // err-only marker for readability

    // Default string for the live-loot options - "true" only in the vanilla
    // bake (see the var defs above), "false" elsewhere. Must match the compiled
    // bool defaults so the generated ini and the DLL agree.
#ifdef MFG_PROFILE_VANILLA
#define MFG_LL_DEF "true"
#else
#define MFG_LL_DEF "false"
#endif

    // helper macros to keep the table compact
#define B(k, var, def, cmt) IniEntry{k, IniType::Bool, &cfg::var, def, cmt, false, nullptr}
#define BE(k, var, def, cmt) IniEntry{k, IniType::Bool, &cfg::var, def, cmt, true, nullptr}

    std::vector<IniSection> build_schema()
    {
        return {
            {"Goblin", nullptr, false, {
                B("require_map_fragments", requireMapFragments, "true",
                  "Require map fragment discovery before showing icons in that area"),
                IniEntry{"fast_map_open", IniType::Bool, &cfg::fastMapOpen, "true",
                         "BETA: makes the world map open faster when many icons are shown.\nTurn off if the map glitches or crashes.",
                         false, "fast_map_reopen"},
            }},

            {"Equipment", nullptr, false, {
                B("show_armaments", showArmaments, "true", "Weapons, shields, bows, staves, etc."),
                B("show_armour", showArmour, "true", "Armor pieces (helms, chest, gauntlets, legs)"),
                B("show_ashes_of_war", showAshesOfWar, "true", "Ashes of War (weapon skills)"),
                B("show_spirits", showSpirits, "true", "Spirit Ashes (summons)"),
                B("show_talismans", showTalismans, "true", "Talismans"),
            }},

            {"Key Items", nullptr, false, {
                B("show_celestial_dew", showCelestialDew, "true", "Celestial Dew (for reversing Spirit Ashes upgrades)"),
                B("show_cookbooks", showCookbooks, "true", "Cookbooks (crafting recipes)"),
                B("show_crystal_tears", showCrystalTears, "true", "Crystal Tears (for Flask of Wondrous Physick)"),
                B("show_great_runes", showGreatRunes, "true", "Great Runes (dropped by story bosses)"),
                B("show_imbued_sword_keys", showImbuedSwordKeys, "true", "Imbued Sword Keys (Four Belfries)"),
                B("show_larval_tears", showLarvalTears, "true", "Larval Tears (respec items)"),
                B("show_lost_ashes", showLostAshes, "true", "Lost Ashes of War"),
                B("show_pots_n_perfumes", showPotsNPerfumes, "true", "Cracked Pots, Ritual Pots, Perfume Bottles"),
                B("show_scadutree_fragments", showScadutreeFragments, "true", "Scadutree Fragments (DLC blessing upgrade)"),
                B("show_seeds_tears", showSeedsTears, "true", "Golden Seeds, Sacred Tears, Revered Spirit Ashes"),
                B("show_whetblades", showWhetblades, "true", "Whetblades (weapon infusion types)"),
            }},

            {"Loot", nullptr, false, {
                B("show_ammo", showAmmo, "true", "Arrows, bolts, greatarrows, greatbolts"),
                B("show_bell_bearings", showBellBearings, "true", "Bell Bearings from treasures/chests/quest rewards"),
                B("show_merchant_bell_bearings", showMerchantBellBearings, "true",
                  "Bell Bearings dropped by killing merchants (Kale, Patches, Gostoc,\nnomadic merchants, etc.)"),
                B("show_consumables", showConsumables, "true", "Healing/buff consumables (boluses, cured meats, livers)"),
                B("show_greases", showGreases, "true", "Weapon greases"),
                B("show_utilities", showUtilities, "true", "Utility items (rainbow stone, glowstone, soap, soft cotton)"),
                B("show_stat_boosts", showStatBoosts, "true", "Stat-up items (Starlight Shards, Sacrificial Twig, Blessing of Marika)"),
                B("show_crafting_materials", showCraftingMaterials, "true", "Crafting materials (flowers, bones, bugs, etc.)"),
                B("show_gloveworts", showGloveworts, "true", "Gloveworts (Grave/Ghost [1-9]) - Spirit Ash upgrade materials"),
                B("show_golden_runes", showGoldenRunes, "true", "Golden Runes [4000+], Hero's/Numen's/Lord's/Shadow Realm Runes"),
                B("show_golden_runes_low", showGoldenRunesLow, "true", "Golden Runes [200-3000], Broken Runes"),
                B("show_great_gloveworts", showGreatGloveworts, "true", "Great Gloveworts (Great Grave, Great Ghost)"),
                B("show_material_nodes", showMaterialNodes, "true", "One-time gathering nodes (Erdleaf Flower, Trina's Lily, etc.)"),
                B("show_mp_fingers", showMPFingers, "true", "Multiplayer items (Furlcalling/Wizened Fingers, Recusant/Bloody Finger)"),
                B("show_prattling_pates", showPrattlingPates, "true", "Prattling Pates"),
                B("show_gestures", showGestures, "true", "Gestures"),
                B("show_reusables", showReusables, "true", "Reusable tools (Mimic Veil, Margit's Shackle, etc.)"),
                B("show_smithing_stones", showSmithingStones, "true", "Smithing Stones [7-8], Somber [7-9], Scadushards"),
                B("show_smithing_stones_low", showSmithingStonesLow, "true", "Smithing Stones [1-6], Somber [1-6]"),
                B("show_smithing_stones_rare", showSmithingStonesRare, "true", "Ancient Dragon Smithing Stones (rare, endgame)"),
                B("show_stonesword_keys", showStoneswordKeys, "true", "Stonesword Keys"),
                B("show_throwables", showThrowables, "true", "Throwable items (darts, daggers, stones, chakrams, warming stones)"),
                B("show_rune_arcs", showRuneArcs, "true", "Rune Arcs (buffs for active Great Rune)"),
                B("show_dragon_hearts", showDragonHearts, "true", "Dragon Hearts (for Dragon Communion incantations)"),
            }},

            {"Magic", nullptr, false, {
                B("show_incantations", showIncantations, "true", "Incantation locations"),
                B("show_memory_stones", showMemoryStones, "true", "Memory Stone locations (extra spell slots)"),
                B("show_prayerbooks", showPrayerbooks, "true", "Prayerbooks and Scrolls (unlock spells at vendors)"),
                B("show_sorceries", showSorceries, "true", "Sorcery locations"),
            }},

            {"Quest", nullptr, false, {
                B("show_deathroot", showDeathroot, "true", "Deathroot locations (for Gurranq)"),
                B("show_progression", showProgression, "true", "Quest progression items (medallions, keys, Needles, quest-specific goods)"),
                B("show_seedbed_curses", showSeedbedCurses, "true", "Seedbed Curse locations (for Dung Eater quest)"),
            }},

            {"Reforged",
             "Elden Ring Reforged-only content. Absent from the vanilla build.",
             ERR, {
                BE("show_ember_pieces", showEmberPieces, "true", "ERR Ember Piece locations"),
                BE("show_items_and_changes", showItemsAndChanges, "true", "ERR-added items: Oracle Effigy/Remedy, Starlight Tokens, Sealed Curios"),
                BE("show_fortunes", showFortunes, "true", "ERR Fortune trinkets (12 types)"),
                BE("show_rune_pieces", showRunePieces, "true", "ERR Rune Piece locations"),
            }},

            {"World", nullptr, false, {
                B("show_bosses", showBosses, "true", "Boss markers (field bosses, dungeon bosses)"),
                B("show_graces", showGraces, "true", "Sites of Grace"),
                B("show_hostile_npc", showHostileNPC, "true", "Hostile NPC invader locations"),
                B("show_imp_statues", showImpStatues, "true", "Imp Statue (Stonesword Key fog gate) locations"),
                B("show_paintings", showPaintings, "true", "Painting locations"),
                B("show_spirit_springs", showSpiritSprings, "true", "Spirit Spring (horse jump) locations"),
                BE("show_spiritspring_hawks", showSpiritspringHawks, "true", "Spiritspring Hawk locations"),
                B("show_stakes_of_marika", showStakesOfMarika, "true", "Stakes of Marika (respawn points)"),
                B("show_summoning_pools", showSummoningPools, "true", "Summoning Pool (Martyr Effigy) locations"),
                BE("show_kindling_spirits", showKindlingSpirits, "true",
                   "ERR Kindling Spirits in Misty Forest - collect all 5 between rests for\nthe Kindling Spirit incantation. Markers hide once you have the incantation."),
                B("show_interactables", showInteractables, "true",
                  "Interactive world objects & puzzles: blue seal puzzles (unlock hidden\ncellars), light-flame interacts (Sellia chalices, Snow Town statues, Siofra\nRiver lanterns), and Hero's Tomb direction statues."),
                B("show_world_maps", showWorldMaps, "true", "World Map fragment locations"),
                B("hide_killed_bosses", hideKilledBosses, "false", "Hide boss/invader/hawk markers after defeat (false = show green checkmark instead)"),
            }},

            {"ERR Markers",
             "This section applies this mod's display rules to ERR's OWN pre-placed map\n"
             "markers (camps, merchants, bosses, dungeon entrances) - NOT the icons this\n"
             "mod injects - so both icon sets follow the same visibility logic\n"
             "(map-fragment discovery, hide on clear). Disable a toggle to leave that\n"
             "marker group exactly as ERR ships it. Our own boss markers are\n"
             "[World] show_bosses and independent of this section.",
             ERR, {
                BE("patch_overworld_boss_icons", patchOverworldBossIcons, "true",
                   "Apply this mod's map-fragment discovery rule to ERR's overworld\nfield-boss markers."),
                BE("patch_dungeon_boss_icons", patchDungeonBossIcons, "true",
                   "Apply this mod's map-fragment discovery rule to ERR's dungeon/cave\nentrance markers. Required for hide_dungeon_icons_on_clear below."),
                BE("patch_camp_icons", patchCampIcons, "true",
                   "Apply this mod's map-fragment discovery rule to ERR's enemy camp markers."),
                BE("patch_merchant_icons", patchMerchantIcons, "true",
                   "Apply this mod's map-fragment discovery rule to ERR's merchant markers."),
                BE("hide_dungeon_icons_on_clear", hideDungeonIconsOnClear, "false",
                   "When patching dungeon entrances, hide the marker once the boss inside is\ndefeated. Requires patch_dungeon_boss_icons."),
            }},

            {"Compatibility",
             "Options for running alongside other mods that change item placement.",
             false, {
                B("live_loot_flags", liveLootFlags, MFG_LL_DEF,
                  "Hide loot markers using the pickup flag from the loaded regulation, so they\ndisappear correctly under the Item/Enemy Randomizer or other regulation mods."),
                B("live_loot_labels", liveLootLabels, MFG_LL_DEF,
                  "Relabel each loot marker with the item its lot currently gives, so names match\nthe randomizer. Uses more memory (copies item names into the map's name table)."),
                B("live_loot_icons", liveLootIcons, MFG_LL_DEF,
                  "Give each loot marker the icon and category of the item its lot currently\ngives, so icons and show_* toggles match the randomizer."),
                B("anonymous_loot", anonymousLoot, "false",
                  "Spoiler-free: every loot marker shows a gray \"?\" and a generic label instead\nof the real item. Overrides live_loot_labels/icons; markers still hide on pickup."),
            }},

            {"Overlay & Hotkeys",
             "The in-game config overlay and the key/button that opens it. toggle_key\n(keyboard) / toggle_gamepad_combo (gamepad) OPEN the overlay when enable_overlay\nis on, or toggle ALL map icons on/off when it's off. Key names: F1-F24, A-Z,\n0-9, Space, Escape, Tab, Enter, Backspace, Home, End, PageUp, PageDown, Insert,\nDelete, arrows.",
             false, {
                B("enable_overlay", enableOverlay, "true",
                  "In-game config overlay (Dear ImGui) opened with the toggle key below.\nSet false if a DX-hook conflict (Steam overlay/RTSS/GeForce Experience) or a\nGPU driver issue makes the game unstable."),
                B("enable_toggle_hotkey", enableToggleHotkey, "true",
                  "Enable toggle_key / toggle_gamepad_combo to switch ALL map icons on/off when\nthe overlay is DISABLED. (When the overlay is enabled, they open it instead.)"),
                IniEntry{"toggle_key", IniType::VkKey, &cfg::toggleInjectionKey, "F10",
                         "Keyboard toggle: OPENS the config overlay when enable_overlay is on, or\ntoggles ALL map icons on/off when the overlay is disabled. Default: F10.", false, "toggle_injection_key"},
                IniEntry{"toggle_gamepad_combo", IniType::GamepadMask, &cfg::toggleGamepadMask, "Y+R3",
                         "Gamepad toggle, same role as toggle_key (opens the overlay, or toggles all\nicons if the overlay is disabled). Tokens joined with '+': A,B,X,Y,LB,RB,\nL3/LSTICK,R3/RSTICK,BACK/SELECT/VIEW,START/MENU,UP/DOWN/LEFT/RIGHT. Default: Y+R3.", false, nullptr},
            }},

            {"Debug",
             "Diagnostics, shown on the overlay's Debug tab.",
             false, {
                B("debug_logging", debugLogging, "false",
                  "Enable verbose debug logging (memory addresses, param details, FMG internals)"),
                B("enable_marker_dump", enableMarkerDump, "false", "Master switch for the marker dump hotkey"),
                IniEntry{"marker_dump_key", IniType::VkKey, &cfg::markerDumpKey, "F9",
                         "Key to dump decoded markers to logs/MapForGoblins_markers.log. Default: F9.", false, nullptr},
            }},
        };
    }

#undef B
#undef BE
}

const std::vector<goblin::IniSection> &goblin::ini_schema()
{
    static const std::vector<IniSection> schema = build_schema();
    return schema;
}

static void emit_comment(std::ostream &out, const char *c)
{
    if (!c) return;
    const char *s = c;
    while (*s)
    {
        const char *nl = std::strchr(s, '\n');
        if (nl)
        {
            out << "; ";
            out.write(s, nl - s);
            out << "\n";
            s = nl + 1;
        }
        else
        {
            out << "; " << s << "\n";
            break;
        }
    }
}

void goblin::emit_ini(std::ostream &out, bool include_err_only, const IniValueResolver &resolve)
{
    out << "; MapForGoblins configuration. Auto-generated from the in-code schema;\n"
        << "; the DLL re-syncs this file on launch (adds new keys, comments out removed ones).\n";
    for (auto const &sec : ini_schema())
    {
        if (sec.err_only && !include_err_only) continue;
        out << "\n";
        emit_comment(out, sec.comment);
        out << "[" << sec.name << "]\n";
        for (auto const &e : sec.entries)
        {
            if (e.err_only && !include_err_only) continue;
            emit_comment(out, e.comment);
            std::string val;
            if (!(resolve && resolve(sec.name, e, val))) val = e.def;
            out << e.key << " = " << val << "\n";
        }
    }
}
