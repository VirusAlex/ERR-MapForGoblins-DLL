#pragma once

#include "../from/paramdef/WORLD_MAP_POINT_PARAM_ST.hpp"
#include <cstddef>
#include <cstdint>

namespace goblin::generated
{

enum class Category : uint8_t
{
    EquipArmaments,
    EquipArmour,
    EquipAshesOfWar,
    EquipSpirits,
    EquipTalismans,
    KeyCelestialDew,
    KeyCookbooks,
    KeyCrystalTears,
    KeyImbuedSwordKeys,
    KeyLarvalTears,
    KeyScadutreeFragments,
    KeyGreatRunes,
    KeyLostAshes,
    KeyPotsNPerfumes,
    KeySeedsTears,
    KeyWhetblades,
    LootAmmo,
    LootBellBearings,
    LootConsumables,
    LootCraftingMaterials,
    LootMPFingers,
    LootMaterialNodes,
    LootMerchantBellBearings,
    LootReusables,
    LootSmithingStones,
    LootSmithingStonesLow,
    LootSmithingStonesRare,
    LootGoldenRunes,
    LootGoldenRunesLow,
    LootStoneswordKeys,
    LootThrowables,
    LootPrattlingPates,
    LootRuneArcs,
    LootDragonHearts,
    LootGloveworts,
    LootGreatGloveworts,
    LootGestures,
    LootGreases,
    LootUtilities,
    LootStatBoosts,
    ReforgedFortunes,
    WorldHostileNPC,
    MagicIncantations,
    MagicMemoryStones,
    MagicPrayerbooks,
    MagicSorceries,
    WorldBosses,
    QuestDeathroot,
    QuestProgression,
    QuestSeedbedCurses,
    ReforgedEmberPieces,
    ReforgedItemsAndChanges,
    ReforgedRunePieces,
    WorldGraces,
    WorldImpStatues,
    WorldMaps,
    WorldPaintings,
    WorldSpiritSprings,
    WorldSpiritspringHawks,
    WorldStakesOfMarika,
    WorldSummoningPools,
    WorldKindlingSpirits,
    WorldInteractables,
};

struct MapEntry
{
    uint64_t row_id;
    from::paramdef::WORLD_MAP_POINT_PARAM_ST data;
    Category category;
    int16_t geom_slot;    // GEOF slot = InstanceID - 9000; -1 if N/A
    int16_t name_suffix;  // e.g. 9003 from "AEG099_821_9003"; -1 if N/A
    const char *object_name;  // full MSB object name e.g. "AEG099_821_9003"; nullptr if N/A
    // Source item-lot linkage for live-loot mode (read getItemFlagId/item from
    // memory at runtime → randomizer-compatible). 0/0 = not lot-backed.
    uint32_t lotId;       // ItemLotParam row id (0 = none)
    uint8_t  lotType;     // 0 = none, 1 = ItemLotParam_map (pref), 2 = ItemLotParam_enemy (pref)
};

extern const MapEntry MAP_ENTRIES[];
extern const size_t MAP_ENTRY_COUNT;

} // namespace goblin::generated
