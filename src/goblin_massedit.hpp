#pragma once

// Dev-only module (not in CMakeLists.txt). Parses .MASSEDIT files at runtime
// for iterating on map data without rebuilding generated arrays.

#include "from/paramdef/WORLD_MAP_POINT_PARAM_ST.hpp"
#include <cstdint>
#include <filesystem>
#include <vector>

namespace goblin::massedit
{

struct MapEntry
{
    int32_t row_id;
    from::paramdef::WORLD_MAP_POINT_PARAM_ST data;
};

/**
 * Load all .MASSEDIT files from a directory and parse them
 * into WorldMapPointParam entries.
 */
std::vector<MapEntry> load_from_directory(const std::filesystem::path &dir);

} // namespace goblin::massedit
