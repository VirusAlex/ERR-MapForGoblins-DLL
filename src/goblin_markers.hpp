#pragma once
#include <filesystem>
#include <string>

// Forward-declare only (see goblin_inject.hpp for why we avoid pulling the
// profile-scoped goblin_map_data.hpp into headers).
namespace goblin::generated { enum class Category : uint8_t; }

namespace goblin::markers
{
    // Human-readable English display name for a marker category (e.g.
    // "Loot - Smithing Stones"). Used by the overlay's Progress tab.
    const char *category_name(generated::Category c);

    // Configure output file path (called once at DLL init).
    void set_output_path(std::filesystem::path path);

    // Poll hotkey and trigger dump. Runs forever in a worker thread.
    void hotkey_loop();

    // Which marker kinds a dump should include. The hotkey-driven log file
    // always writes DUMP_ALL (both together); the overlay can request just one
    // kind to keep its on-screen text box readable.
    enum DumpSel { DUMP_ALL = 0, DUMP_BEACONS = 1, DUMP_STAMPS = 2 };

    // Run a marker dump now and return it as text (for the in-game overlay's
    // Debug tab). SEH-guarded; returns a short note instead of throwing.
    std::string dump_to_string(DumpSel sel = DUMP_ALL);
}
