#pragma once

#include <cstdint>
#include <unordered_map>

namespace goblin::kindling
{
    /// Build slot table from MAP_ENTRIES (rows in WorldKindlingSpirits category).
    /// Decodes per-row kindling slot 1..5 + SFX entity ID from MAP_ENTRY metadata.
    /// Safe to call before remap_row_ids - works on original row IDs.
    void initialize();

    /// Remap row IDs after dynamic ID assignment in inject.
    void remap_row_ids(const std::unordered_map<uint64_t, uint64_t> &old_to_new);

    /// Re-read SFX-region state from RAM. Returns delta vs previous tick
    /// (positive = newly hidden, negative = newly revealed).
    /// Idempotent if state hasn't changed.
    int refresh();

    /// True if the row is currently considered collected (hidden).
    bool is_row_collected(uint64_t row_id);

    /// Register the param row pointer so refresh() can flip areaNo in-place.
    /// Same trick as goblin::collected - write 0x20 = 99 to hide, restore original.
    void register_param_ptr(uint64_t row_id, void *param_data);

    int collected_count();
};
