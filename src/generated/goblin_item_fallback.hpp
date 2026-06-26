#pragma once

#include <cstddef>
#include <cstdint>

namespace goblin::generated
{
    // English item-name fallback. For every loot item a marker can reference (the same
    // encoded-id space as goblin_item_icons / the markers' textId1), the English name read
    // from the game's engus msgbnd at build time. setup_messages injects these into PlaceName
    // as the LOWEST-priority layer, so a marker whose item has no string in the player's
    // language shows the English name instead of "?PlaceName?". Localized strings copied from
    // the live FMG are added first and win the first-occurrence-wins dedup, so this never
    // overrides an existing localized name. id = item id + category offset (100M weapon /
    // 200M armour / 300M talisman / 400M ash / 500M goods), matching the marker textId.
    struct ItemNameFallback
    {
        int32_t id;
        const wchar_t *name;
    };

    extern const size_t ITEM_NAME_FALLBACK_COUNT;
    extern const ItemNameFallback ITEM_NAME_FALLBACK[];  // sorted ascending by id
}
