#pragma once

#include <cstdint>

namespace goblin::map_api {
// Injection is a one-time initialization. Publish only after every row exists.
void publish_rows() noexcept;
// Called only when the native manager creates or discards its own children.
void record_depth(uint64_t id, uint16_t depth) noexcept;
void reset_depths() noexcept;
} // namespace goblin::map_api
