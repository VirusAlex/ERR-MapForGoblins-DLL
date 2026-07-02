#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "goblin_map_data.hpp"

// Per-region collection-progress aggregation for the overlay's Progress tab.
//
// Groups every baked MAP_ENTRIES marker into one of ~22 coarse world regions
// (the map-fragment partition in goblin/goblin_map_tiles.hpp, with the E/W and
// N/E/W splits merged back into a single named region), and counts collected vs
// total per category using the live collected-tracking state. Region names are
// localized at runtime via goblin::lookup_text (English fallback baked in).
//
// The aggregation is O(MAP_ENTRY_COUNT) and only re-runs when the collected
// count changes (see rebuild_if_stale), never per frame.
namespace goblin::progress
{
    // Number of Category enum values (contiguous 0..N from goblin_map_data.hpp).
    constexpr int kCategoryCount =
        static_cast<int>(generated::Category::WorldInteractables) + 1;

    struct CatCount
    {
        int collected = 0;
        int total = 0;
    };

    struct RegionProgress
    {
        int32_t place_name_id = 0;  // PlaceName id used for the localized name (0 = Other)
        std::string name;           // resolved UTF-8 name (localized, else English fallback)
        int collected = 0;          // region-wide collected / total (all categories)
        int total = 0;
        CatCount cats[kCategoryCount];
    };

    // Rebuild the cached aggregation from MAP_ENTRIES + live collected state.
    void rebuild();

    // Rebuild at most every ~0.5s (pass a monotonically increasing seconds
    // clock, e.g. ImGui::GetTime()), or immediately if nothing is built yet.
    // Time-throttled rather than keyed on the GEOF collected count, because
    // flag-based loot/boss pickups don't change that count. Cheap to call every
    // frame from the tab; the aggregation only re-runs on the throttle tick.
    void rebuild_if_stale(double now_seconds);

    // Cached regions, sorted by name (the "Other" bucket sorts last). Empty
    // until rebuild()/rebuild_if_stale() has run at least once.
    const std::vector<RegionProgress> &snapshot();

    // The region grouping key (a PlaceName id, or -1 for the "Other" bucket) for
    // a marker's baked tile. Same logic the tab groups by; used by inject to tag
    // each CategoryRow so map "focus" can isolate one category IN one region.
    int32_t region_place_id(const from::paramdef::WORLD_MAP_POINT_PARAM_ST &data);
}
