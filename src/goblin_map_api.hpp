#pragma once

#include <cstdint>

// The provider side of the map API (sdk/mfg_map_api.h). Every entry point here is a no-op until a
// reader has called MfgGetMapAPI, so a game without one pays nothing.
namespace goblin::map_api {
// Injection is a one-time initialization. Publish only after every row exists.
void publish_rows() noexcept;
// Poll thread (dllmain), every poll, under __try: rebuilds the snapshot of each layer a reader is
// asking for. Not noexcept: a fault on a freed game row has to unwind out of it to that __except.
void maintain();
// Called only when the native manager creates or discards its own children.
void record_depth(uint64_t id, uint16_t depth) noexcept;
void reset_depths() noexcept;
} // namespace goblin::map_api
