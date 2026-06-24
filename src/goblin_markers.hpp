#pragma once
#include <filesystem>
#include <string>

namespace goblin::markers
{
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
