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
    LootReusables,
    LootSomberScarab,
    LootStoneswordKeys,
    LootUniqueDrops,
    MagicIncantations,
    MagicMemoryStones,
    MagicSorceries,
    QuestDeathroot,
    QuestProgression,
    QuestSeedbedCurses,
    ReforgedCampContents,
    ReforgedEmberPieces,
    ReforgedItemsAndChanges,
    ReforgedRunePieces,
    WorldGraces,
    WorldHostileNPC,
    WorldImpStatues,
    WorldPaintings,
    WorldSpiritSprings,
    WorldSpiritspringHawks,
    WorldSummoningPools,
};

struct MapEntry
{
    uint64_t row_id;
    from::paramdef::WORLD_MAP_POINT_PARAM_ST data;
    Category category;
    int16_t geom_slot;    // GEOF slot = InstanceID - 9000; -1 if N/A
    int16_t name_suffix;  // e.g. 9003 from "AEG099_821_9003"; -1 if N/A
};

extern const MapEntry MAP_ENTRIES[];
extern const size_t MAP_ENTRY_COUNT;

} // namespace goblin::generated
