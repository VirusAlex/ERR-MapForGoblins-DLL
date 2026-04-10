#define WIN32_LEAN_AND_MEAN
#include <filesystem>
#include <memory>
#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <thread>
#include <windows.h>

#include "tracker.hpp"

static std::thread g_mod_thread;
static std::filesystem::path g_mod_folder;

static void setup_logger(std::filesystem::path log_file)
{
    auto logger = std::make_shared<spdlog::logger>("tracker");
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] %^[%l]%$ %v");
    logger->sinks().push_back(
        std::make_shared<spdlog::sinks::daily_file_sink_st>(log_file.string(), 0, 0, false, 5));
    logger->flush_on(spdlog::level::info);
    logger->set_level(spdlog::level::debug);

    AllocConsole();
    FILE *stream;
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
    freopen_s(&stream, "CONIN$", "r", stdin);
    SetConsoleTitleA("RunePieceTracker v0.9.1");
    logger->sinks().push_back(std::make_shared<spdlog::sinks::stdout_color_sink_st>());

    spdlog::set_default_logger(logger);
}

static void mod_main()
{
    try
    {
        spdlog::info("Waiting 10s for game init...");
        std::this_thread::sleep_for(std::chrono::seconds(10));

        tracker::initialize(g_mod_folder);
        tracker::hotkey_loop();
    }
    catch (const std::exception &e)
    {
        spdlog::error("Fatal error: {}", e.what());
    }
}

bool WINAPI DllMain(HINSTANCE dll_instance, unsigned int fdw_reason, void *lpv_reserved)
{
    if (fdw_reason == DLL_PROCESS_ATTACH)
    {
        wchar_t dll_filename[MAX_PATH] = {0};
        GetModuleFileNameW(dll_instance, dll_filename, MAX_PATH);
        g_mod_folder = std::filesystem::path(dll_filename).parent_path();

        setup_logger(g_mod_folder / "logs" / "RunePieceTracker.log");

        spdlog::info("═══════════════════════════════════════════════════════════");
        spdlog::info("  RunePieceTracker v" PROJECT_VERSION);
        spdlog::info("  Piece DB + Position tracking + Pickup matching");
        spdlog::info("═══════════════════════════════════════════════════════════");

        g_mod_thread = std::thread(mod_main);
    }
    else if (fdw_reason == DLL_PROCESS_DETACH && lpv_reserved != nullptr)
    {
        try
        {
            g_mod_thread.join();
        }
        catch (...)
        {
        }
        spdlog::shutdown();
    }
    return true;
}
