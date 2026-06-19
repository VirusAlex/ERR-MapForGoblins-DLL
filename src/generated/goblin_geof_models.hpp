#pragma once

#include <cstddef>
#include <cstdint>

namespace goblin::generated
{

// Rows whose gather part NAME differs from its ACTUAL model (ERR substituted the asset,
// e.g. part AEG099_753_9000 instantiates DLC model AEG463_860). GEOF save entries carry
// the ACTUAL model's hash - collected-tracking must bucket these rows by it.
struct GeofModelOverride
{
    uint64_t row_id;    // ORIGINAL (pre-remap) row id, matches MAP_ENTRIES[].row_id
    uint32_t model_id;  // actual model id (10463860 = AEG463_860) as seen in GEOF entries
};

extern const GeofModelOverride GEOF_MODEL_OVERRIDES[];  // sorted by row_id
extern const size_t GEOF_MODEL_OVERRIDE_COUNT;

} // namespace goblin::generated
