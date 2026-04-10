#pragma once

#include <cstdint>

namespace goblin::collected
{
    /// Build tile-to-row lookup tables from map data. No memory reads yet.
    void initialize();

    /// Re-read GEOF/WGM from memory. Returns delta (newly hidden count).
    int refresh();

    bool is_row_collected(uint64_t row_id);

    void register_param_ptr(uint64_t row_id, void *param_data);

    int collected_count();
    int skipped_count();
};
