#pragma once

#include <cstddef>
#include <cstdint>

namespace goblin::generated
{

// Hybrid sub-area loot-location names (PRIMARY): rows whose location differs from the baked
// baseline under the hybrid model (MSB volume containment + height-aware nearest anchor in 3D).
// The DLL OVERWRITES textId<slot> with this value at injection time; rows absent from this
// table keep their baked baseline (the tile/grace fallback).
//   slot = the marker's LOCATION slot - textId2 for plain loot, textId3 for enemy-drop loot
//          (textId1=item / textId2=enemy / textId3=location). slot 0 = no baseline location
//          line; the DLL adds one (textId2 if free, else textId3).
struct LocationAlt
{
    uint64_t row_id;   // ORIGINAL (pre-remap) row id, matches MAP_ENTRIES[].row_id
    uint8_t  slot;     // location textId slot to overwrite (2/3...), 0 = add a new line
    int32_t  textId2;  // hybrid PlaceName id (may be a COMPOSED id, see below)
};

// Duplicate-named sub-zones (e.g. the two Hallowhorn Grounds) get a synthetic PlaceName id
// whose text the DLL composes at runtime from the game's own FMG strings: "<sub> (<super>)".
// Localization-safe: both parts are vanilla PlaceName ids looked up in the live FMG.
struct LocationCompose
{
    int32_t id;        // synthetic PlaceName id (999000001+)
    int32_t subId;     // vanilla PlaceName id of the sub-zone name
    int32_t superId;   // vanilla PlaceName id of the disambiguating super-zone
};

extern const LocationAlt LOCATION_ALT[];  // sorted by row_id
extern const size_t LOCATION_ALT_COUNT;
extern const LocationCompose LOCATION_COMPOSE[];
extern const size_t LOCATION_COMPOSE_COUNT;

} // namespace goblin::generated
