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
    bool debugLogging = false;        // key debug_logging: verbose diagnostics. LOGGING ONLY - it must never
                                      // arm behaviour. Anything functional gets its own key below.
                                      // native menu (un-localized, for us, not for players).
    // (icon/resource injection is unconditional - it IS how icons render without a gfx; no ini toggle.)

    bool locationEmphasis = true;
    float locationEmphasisOwnScale = 1.05f;
    float mapPanelOffsetPercent = 100.0f;
    float locationEmphasisOtherScale = 0.95f;
    float locationEmphasisOtherFade = 0.75f;
    float locationEmphasisOtherCool = 0.90f;

    bool showArmaments = true, showArmour = true, showAshesOfWar = true,
         showSpirits = true, showTalismans = true;

    bool showCelestialDew = true, showCookbooks = true, showCrystalTears = true,
         showGreatRunes = true, showImbuedSwordKeys = true, showLarvalTears = true,
         showLostAshes = true, showPotsNPerfumes = true, showScadutreeFragments = true,
         showReveredSpiritAshes = true, showSpectralSteedRegalia = true, showSeedsTears = true,
         showWhetblades = true;

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

    bool showBosses = true, showGraces = true, showHostileNPC = true, showStrongEnemies = true,
         showImpStatues = true, showPaintings = true, showSpiritSprings = true,
         showSpiritspringHawks = true, showStakesOfMarika = true,
         showSummoningPools = true, showKindlingSpirits = true,
         showInteractables = true, showWorldMaps = true, hideKilledBosses = false;

    // World Map fragment markers ignore require_map_fragments (default ON): a map
    // fragment marker is only useful BEFORE you own that fragment, but the gate would
    // hide it until you do - so you'd never see where to find one. Independent of
    // show_world_maps (the category on/off).
    bool worldMapsIgnoreFragments = true;

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

    std::string uiLanguage = "auto"; // key ui_language (was overlay_ui_language for two versions)
    std::string menuRenderMode = "native"; // native | imgui (| dev, undocumented)
    float fontScale = 1.0f;  // overlay text size multiplier (live io.FontGlobalScale)
    bool menuEnabled = true; // key menu_enabled (was enable_menu, was enable_overlay)
    float overlayOpacity = 1.0f;                 // overlay menu panel opacity (window bg alpha)
    // Menu window geometry. overlayWinX = the window's CENTER x as a fraction of screen
    // width (0.5 = horizontally centered); overlayWinY = the window's TOP y as a fraction
    // of screen height (0 = flush to the top). Stored as fractions so the position stays
    // sensible after a resolution/aspect change. W/H are pixels.
    float overlayWinX = 0.5f, overlayWinY = 0.03f, overlayWinW = 560.0f, overlayWinH = 680.0f;
    bool enableMarkerDump = false;
    uint32_t markerDumpKey = 0x78; // VK_F9
    bool enableManualHide = true;
    bool searchHideCollected = true;
    uint32_t hideMarkerKey = 0x2E; // VK_DELETE
    uint16_t hideMarkerGamepad = 0;      // unbound; RB (the old default) is the map's own
                                         // tab-switch button, so it fired on every layer switch
    bool enableHoverInfo = true;
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
                B("location_emphasis", locationEmphasis, "true",
                  "Tell apart the markers of the place you are IN from the ones that only\n"
                  "look nearby: a dungeon sits under the overworld, so its icons land on the\n"
                  "same spot of the map as the surface ones. Markers of your own map draw\n"
                  "bigger, markers of any other map draw smaller. Nothing is ever hidden."),
                                IniEntry{"map_panel_offset_percent", IniType::Float, &cfg::mapPanelOffsetPercent, "100",
                         "Horizontal position of the marker tooltip and the focus banner on the map "
                         "screen. 100 = the corner they were authored for, 0 = the centre of the map "
                         "area, above 100 = further left, below 0 = right of centre. Only needed if "
                         "the panels sit wrong on your display; ultrawide setups have been reported. "
                         "Takes effect immediately, no need to reopen the map."},
                IniEntry{"location_emphasis_own_scale", IniType::Float, &cfg::locationEmphasisOwnScale, "1.05",
                         "Size of the markers that belong to the map you are standing in (1.0 = unchanged).", false, nullptr},
                IniEntry{"location_emphasis_other_scale", IniType::Float, &cfg::locationEmphasisOtherScale, "0.95",
                         "Size of the markers that belong to any other map (1.0 = unchanged).", false, nullptr},
                IniEntry{"location_emphasis_other_fade", IniType::Float, &cfg::locationEmphasisOtherFade, "0.75",
                         "How much the markers of another map fade (1.0 = no fade, 0.2 = faintest).\n"
                         "They stay fully visible and clickable - only quieter than the ones around you.", false, nullptr},
                IniEntry{"location_emphasis_other_cool", IniType::Float, &cfg::locationEmphasisOtherCool, "0.90",
                         "How far the markers of another map are pushed cold, on top of the fade\n"
                         "(0.0 = colour untouched, 1.0 = strongest). Red and green drop, blue holds.", false, nullptr},
                // fast_map_open / native_self_detach / native_viewport_window used to live here as
                // BETA toggles. They are compile-time variants now - see goblin_build_variants.hpp.
            }},

            {"Equipment", nullptr, false, {
                B("show_armaments", showArmaments, "true", "Weapons, shields, bows, staves, etc."),
                B("show_armour", showArmour, "true", "Armor pieces (helms, chest, gauntlets, legs)"),
                B("show_ashes_of_war", showAshesOfWar, "true", "Ashes of War (weapon skills)"),
                B("show_spirits", showSpirits, "true", "Spirit Ashes (summons)"),
                B("show_talismans", showTalismans, "true", "Talismans"),
            }},

            {"Key Items", nullptr, false, {
                B("show_celestial_dew", showCelestialDew, "true", "Celestial Dew (for Absolution at the Church of Vows)"),
                B("show_cookbooks", showCookbooks, "true", "Cookbooks (crafting recipes)"),
                B("show_crystal_tears", showCrystalTears, "true", "Crystal Tears (for Flask of Wondrous Physick)"),
                B("show_great_runes", showGreatRunes, "true", "Great Runes (dropped by story bosses)"),
                B("show_imbued_sword_keys", showImbuedSwordKeys, "true", "Imbued Sword Keys (Four Belfries)"),
                B("show_larval_tears", showLarvalTears, "true", "Larval Tears (respec items)"),
                B("show_lost_ashes", showLostAshes, "true", "Lost Ashes of War"),
                B("show_pots_n_perfumes", showPotsNPerfumes, "true", "Cracked Pots, Ritual Pots, Perfume Bottles"),
                B("show_scadutree_fragments", showScadutreeFragments, "true", "Scadutree Fragments (DLC blessing upgrade)"),
                B("show_revered_spirit_ashes", showReveredSpiritAshes, "true", "Revered Spirit Ashes (DLC spirit ash blessing upgrade)"),
                B("show_spectral_steed_regalia", showSpectralSteedRegalia, "true", "Spectral Steed Regalia (Torrent's caparisons; they need the Tarnished Pack)"),
                B("show_seeds_tears", showSeedsTears, "true", "Golden Seeds, Sacred Tears"),
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
                BE("show_ember_pieces", showEmberPieces, "true", "Ember Piece locations"),
                BE("show_items_and_changes", showItemsAndChanges, "true", "Added items: Oracle Effigy/Remedy, Starlight Tokens, Sealed Curios"),
                BE("show_fortunes", showFortunes, "true", "Fortune trinkets (12 types)"),
                BE("show_rune_pieces", showRunePieces, "true", "Rune Piece locations"),
            }},

            {"World", nullptr, false, {
                B("show_bosses", showBosses, "true", "Boss markers (field bosses, dungeon bosses)"),
                B("show_graces", showGraces, "true", "Sites of Grace"),
                B("show_hostile_npc", showHostileNPC, "true", "Hostile NPC invader locations"),
                B("show_strong_enemies", showStrongEnemies, "true",
                  "Strong enemies that stay dead once killed (scarabs, field mini-bosses, some NPCs)"),
                B("show_imp_statues", showImpStatues, "true", "Imp Statue (Stonesword Key fog gate) locations"),
                B("show_paintings", showPaintings, "true", "Painting locations"),
                B("show_spirit_springs", showSpiritSprings, "true", "Spirit Spring (horse jump) locations"),
                BE("show_spiritspring_hawks", showSpiritspringHawks, "true", "Spiritspring Hawk locations"),
                B("show_stakes_of_marika", showStakesOfMarika, "true", "Stakes of Marika (respawn points)"),
                B("show_summoning_pools", showSummoningPools, "true", "Summoning Pool (Martyr Effigy) locations"),
                BE("show_kindling_spirits", showKindlingSpirits, "true",
                   "Kindling Spirits in Misty Forest - collect all 5 between rests for\nthe Kindling Spirit incantation. Markers hide once you have the incantation."),
                B("show_interactables", showInteractables, "true",
                  "Interactive world objects & puzzles: blue seal puzzles (unlock hidden\ncellars), light-flame interacts (Sellia chalices, Snow Town statues, Siofra\nRiver lanterns), and Hero's Tomb direction statues."),
                B("show_world_maps", showWorldMaps, "true", "World Map fragment locations"),
                B("show_world_maps_ignore_fragments", worldMapsIgnoreFragments, "true",
                  "Always show World Map fragment markers, even with require_map_fragments\non (otherwise you could never see where a map fragment is until you own it)."),
                B("hide_killed_bosses", hideKilledBosses, "false", "Hide boss/invader/hawk markers after defeat (false = show green checkmark instead)"),
            }},

            {"ERR Markers",
             "This section applies this mod's display rules to ERR's OWN pre-placed map\n"
             "markers (camps, merchants, bosses, dungeon entrances) - NOT the icons this\n"
             "mod adds - so both icon sets follow the same visibility logic\n"
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

            {"Menu & Hotkeys",
             "The mod's MENU and the key/button that opens it. toggle_key (keyboard) /\n"
             "toggle_gamepad_combo (gamepad) OPEN the menu when menu_enabled is on, or toggle\n"
             "ALL map icons on/off when it is off. Which menu opens is menu_render_mode.\n"
             "Key names: F1-F24, A-Z, 0-9, Space, Escape, Tab, Enter, Backspace, Home, End,\n"
             "PageUp, PageDown, Insert, Delete, arrows.",
             false, {
                IniEntry{"menu_enabled", IniType::Bool, &cfg::menuEnabled, "true",
                         "The mod's menu on toggle_key / toggle_gamepad_combo. With this off those\n"
                         "controls switch ALL map icons on/off instead, and no menu opens - which is\n"
                         "also what to reach for if a DX overlay conflict (Steam overlay / RTSS /\n"
                         "GeForce Experience) or a driver issue makes the game unstable with the\n"
                         "overlay menu.",
                         // Renamed twice: enable_overlay in every released build, then enable_menu
                         // for a few hours here. Both names still find their value.
                         false, "enable_menu,enable_overlay"},

                IniEntry{"menu_render_mode", IniType::Text, &cfg::menuRenderMode, "native",
                         "WHICH MENU the mod puts on toggle_key / toggle_gamepad_combo." "\n"
                         "native (default) - the in-game menu the game draws itself; the overlay is" "\n"
                         "not created at all." "\n"
                         "imgui - the separate overlay window; the in-game menu is not injected (the" "\n"
                         "map panels keep working, they have their own switch)." "\n"
                         "Editable HERE ONLY: neither menu offers it, because it decides which menu" "\n"
                         "exists. Change needs a game restart.",
                         false, "overlay_render_mode", true},
                IniEntry{"ui_language", IniType::Language, &cfg::uiLanguage, "auto",
                         "Language of everything this mod writes: the in-game menu, the overlay menu,\n"
                         "the map tooltip and this file's own comments. auto follows the Steam game\n"
                         "language; unrecognized languages fall back to English.\n"
                         "Values: auto, english, schinese, tchinese, korean, russian, german, french,\n"
                         "spanish, vietnamese.\n"
                         "Item and place names on the markers come from the game itself and always\n"
                         "stay in the game's language, because the game loads only that one. The\n"
                         "in-game menu is drawn with the GAME's font: Chinese or Korean picked in a\n"
                         "European copy of the game shows as blank boxes there, while the overlay\n"
                         "carries its own font and renders them. Enemy names on the markers, and that\n"
                         "overlay font, are prepared while the game loads, so they follow a change\n"
                         "only after a restart.",
                         false, "overlay_ui_language"},
                IniEntry{"overlay_font_scale", IniType::Float, &cfg::fontScale, "1.0",
                         "Overlay menu text size multiplier (1.0 = default). Raise on 4K / high-DPI\nscreens if the menu text is too small. Also adjustable live from the slider at\nthe top of the overlay's Settings tab.", false, nullptr},
                IniEntry{"overlay_opacity", IniType::Float, &cfg::overlayOpacity, "1.0",
                         "Overlay menu panel opacity, 0.3 to 1.0 (1.0 = solid). Lower it to see more\nof the map behind the menu. Also adjustable live from a slider in the Settings tab.", false, nullptr},
                // Overlay menu window geometry - auto-managed (saved when you move/resize the
                // menu and close it, restored on open). X = the window CENTER as a fraction of
                // screen width (0.5 = centered); Y = the window TOP as a fraction of screen
                // height (0 = flush top). Fractions keep the position sensible after a
                // resolution/aspect change; W/H are in pixels (clamped to fit the screen).
                IniEntry{"overlay_window_x", IniType::Float, &cfg::overlayWinX, "0.5", "Overlay menu horizontal CENTER, fraction of screen width (0.5 = centered). Auto-saved.", false, nullptr},
                IniEntry{"overlay_window_y", IniType::Float, &cfg::overlayWinY, "0.03", "Overlay menu TOP edge, fraction of screen height (0.0 = top). Auto-saved.", false, nullptr},
                IniEntry{"overlay_window_w", IniType::Float, &cfg::overlayWinW, "560", "Overlay menu window width in pixels (auto-saved, clamped to screen).", false, nullptr},
                IniEntry{"overlay_window_h", IniType::Float, &cfg::overlayWinH, "680", "Overlay menu window height in pixels (auto-saved, clamped to screen).", false, nullptr},
                B("enable_toggle_hotkey", enableToggleHotkey, "true",
                  "Let toggle_key / toggle_gamepad_combo switch ALL map icons on/off while\n"
                  "menu_enabled is off. (With the menu on, those controls open it instead.)"),
                IniEntry{"toggle_key", IniType::VkKey, &cfg::toggleInjectionKey, "F10",
                         "Keyboard key: OPENS the mod's menu when menu_enabled is on, or toggles ALL\n"
                         "map icons on/off when it is off. Default: F10.", false, "toggle_injection_key"},
                IniEntry{"toggle_gamepad_combo", IniType::GamepadMask, &cfg::toggleGamepadMask, "Y+R3",
                         "Gamepad combo, same role as toggle_key (opens the menu, or toggles all\n"
                         "icons when menu_enabled is off). Tokens joined with '+': A,B,X,Y,LB,RB,\n"
                         "L3/LSTICK,R3/RSTICK,BACK/SELECT/VIEW,START/MENU,UP/DOWN/LEFT/RIGHT.\n"
                         "Default: Y+R3.", false, nullptr},
                // Marker-interaction options (shown at the bottom of the Settings tab, not Debug):
                B("enable_manual_hide", enableManualHide, "true",
                  "Let you hide individual markers: hover a marker on the world map and press\nhide_marker_key to hide it. Hidden markers persist across sessions; un-hide them\nfrom the in-game menu (Hidden markers section)."),
                IniEntry{"hide_marker_key", IniType::VkKey, &cfg::hideMarkerKey, "Delete",
                         "Key that hides the map marker currently under the cursor. Default: Delete.", false, nullptr},
                IniEntry{"hide_marker_gamepad", IniType::GamepadMask, &cfg::hideMarkerGamepad, "none",
                         "Gamepad button that hides the map marker under the cursor (same as\nhide_marker_key). Tokens joined with '+'. Unbound by default: every free\nmap button is taken by the game (RB, the old default, is the map's own\ntab-switch button, so it fired on every layer switch). Default: none.", false, nullptr},
                B("search_hide_collected", searchHideCollected, "true",
                  "Item search (menu / overlay): list only markers not yet collected. Off = list\n"
                  "everything, collected ones tagged. Default: true."),
                B("hover_info", enableHoverInfo, "true",
                  "Show a small passive panel (top-left) while the world map is open and the\ncursor is over a marker: the marker's name and its height relative to you\n(\"N units above/below\"). Never captures input."),
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

const std::vector<const char *> &goblin::ini_retired_keys()
{
    // Retired 2026-07-27/28. The three map-open levers became compile-time build variants
    // (goblin_build_variants.hpp) because nobody tuned them and a mod should work on install;
    // native_markers was folded into the same variant switch so only one marker mechanism ships.
    static const std::vector<const char *> keys = {
        "fast_map_open",
        "fast_map_reopen",  // the even older name of the same lever
        "native_self_detach",
        "native_viewport_window",
        "native_markers",
    };
    return keys;
}

// -- which menu is on the hotkey ---------------------------------------------------------------
// One ini value decides it, and every gate asks here instead of comparing the string again:
//   native - the game draws the menu; the overlay is never created
//   imgui  - the overlay window; nothing of the in-game menu is injected
//   dev    - both, the overlay on the hotkey and the in-game menu on F8. Undocumented on purpose:
//            it exists for working on the two side by side, not as a user-facing choice.
// Decided ONCE per session, from the ini as DllMain loaded it (dllmain.cpp seals it right after that
// load). Which menu exists is fixed at startup anyway - the overlay is created or not - and the
// overlay reloads the ini on every open, where apply_defaults() leaves this key at "native" for the
// length of the file read. Measured 2026-09-11 in imgui mode: F10 over the map landed in that window,
// the in-game menu's key check saw native mode, and the native settings screen opened together with
// the overlay. Re-reading the string there was also an unsynchronized read of a string being rewritten.
goblin::config::MenuMode goblin::config::menu_mode()
{
    static const MenuMode mode = []
    {
        const std::string &v = menuRenderMode;
        if (v == "imgui")
            return MenuMode::ImGui;
        if (v == "dev")
            return MenuMode::Dev;
        // Anything else - including every legacy drawing mode (surface / layered / swapchain*) - reads
        // as the default. load_config() rewrites such a value so the file says what is in force.
        return MenuMode::Native;
    }();
    return mode;
}

bool goblin::config::native_menu_enabled() { return menu_mode() != MenuMode::ImGui; }
bool goblin::config::overlay_menu_enabled() { return menu_mode() != MenuMode::Native; }

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

void goblin::emit_ini(std::ostream &out, bool include_err_only, const IniValueResolver &resolve,
                      i18n::Language language)
{
    emit_comment(out, i18n::tr(i18n::TextId::IniHeader, language));
    for (auto const &sec : ini_schema())
    {
        if (sec.err_only && !include_err_only) continue;
        out << "\n";
        emit_comment(out, i18n::section_comment(sec.name, sec.comment, language));
        out << "[" << sec.name << "]\n";
        for (auto const &e : sec.entries)
        {
            if (e.err_only && !include_err_only) continue;
            emit_comment(out, i18n::entry_comment(e.key, e.comment, language));
            std::string val;
            if (!(resolve && resolve(sec.name, e, val))) val = e.def;
            out << e.key << " = " << val << "\n";
        }
    }
}
