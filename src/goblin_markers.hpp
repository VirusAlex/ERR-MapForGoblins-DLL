#pragma once
#include <filesystem>
#include <string>

namespace goblin::markers
{
    // Configure output file path (called once at DLL init).
    void set_output_path(std::filesystem::path path);

    // Poll hotkey and trigger dump. Runs forever in a worker thread.
    void hotkey_loop();

    // Run a marker dump now and return it as text (for the in-game overlay's
    // Tools tab). SEH-guarded; returns a short note instead of throwing.
    std::string dump_to_string();
}
