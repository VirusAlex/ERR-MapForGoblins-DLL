#define WIN32_LEAN_AND_MEAN
#include <filesystem>
#include <memory>
#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <thread>
#include <windows.h>

#include "from/params.hpp"
#include "modutils.hpp"

#include "goblin_collected.hpp"
#include "goblin_config.hpp"
#include "goblin_inject.hpp"
#include "goblin_logic.hpp"
#include "goblin_markers.hpp"
#include "goblin_messages.hpp"

#include "version.h"

static std::thread mod_thread;

// SEH wrapper — catches access violations from refresh() during multiplayer transitions
static int safe_refresh_seh()
{
    __try
    {
        return goblin::collected::refresh();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

// ── SEH-guarded init-phase wrappers ──
// MSVC's /EHsc disallows __try in functions that contain C++ objects with
// destructors, so each init step goes through a plain C-style adapter +
// a shared invoker. If any step access-violates (e.g. another mod shifted
// the game's memory map mid-init), we log and continue — losing that
// feature is better than the DLL crashing the entire game.

using InitFn = void (*)();

static bool seh_invoke_void(InitFn fn)
{
    __try { fn(); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void init_modutils()         { modutils::initialize(); }
static void init_from_params()      { from::params::initialize(); }
static void init_collected()        { goblin::collected::initialize(); }
static void init_inject_entries()   { goblin::inject_map_entries(); }
static void init_apply_map_logic()  { goblin::apply_map_logic(); }
static void init_setup_messages()   { goblin::setup_messages(); }

static void safe_init_step(InitFn fn, const char *name)
{
    if (!seh_invoke_void(fn))
        spdlog::error("SEH exception in init step '{}' — feature may be degraded", name);
}

static void setup_logger(std::filesystem::path log_file)
{
    auto logger = std::make_shared<spdlog::logger>("mapforgoblins");
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] %^[%l]%$ %v");
    logger->sinks().push_back(
        std::make_shared<spdlog::sinks::daily_file_sink_st>(log_file.string(), 0, 0, false, 5));
    logger->flush_on(spdlog::level::info);

#if _DEBUG
    AllocConsole();
    FILE *stream;
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
    freopen_s(&stream, "CONIN$", "r", stdin);
    logger->sinks().push_back(std::make_shared<spdlog::sinks::stdout_color_sink_st>());
    logger->set_level(spdlog::level::trace);
#endif

    spdlog::set_default_logger(logger);
}

static std::filesystem::path g_mod_folder;

static void setup_mod()
{
    safe_init_step(&init_modutils,    "modutils::initialize");
    safe_init_step(&init_from_params, "from::params::initialize");

    spdlog::info("Waiting {}s for game init...", goblin::config::loadDelay);
    std::this_thread::sleep_for(std::chrono::seconds(goblin::config::loadDelay));

    safe_init_step(&init_collected,       "collected::initialize");
    safe_init_step(&init_inject_entries,  "inject_map_entries");
    safe_init_step(&init_apply_map_logic, "apply_map_logic");
    safe_init_step(&init_setup_messages,  "setup_messages");

    try
    {
        modutils::enable_hooks();
    }
    catch (const std::exception &e)
    {
        spdlog::error("enable_hooks() FAILED: {}", e.what());
    }

    spdlog::info("Initialization complete");

    if (GetModuleHandleA("ersc.dll"))
        spdlog::info("Seamless Co-op detected (ersc.dll)");

    if (goblin::config::enableMarkerDump)
    {
        goblin::markers::set_output_path(g_mod_folder / "logs" / "MapForGoblins_markers.log");
        std::thread(goblin::markers::hotkey_loop).detach();
        spdlog::info("Marker dump hotkey: VK 0x{:X}", goblin::config::markerDumpKey);
    }

    bool first_read = true;
    auto start = std::chrono::steady_clock::now();
    while (true)
    {
        // Fast polling (100ms) for first 30 seconds to catch NonActive GEOF data
        // before it transitions to WGM. Then slow down to 2 seconds.
        auto elapsed = std::chrono::steady_clock::now() - start;
        bool fast_phase = elapsed < std::chrono::seconds(30);
        std::this_thread::sleep_for(fast_phase ? std::chrono::milliseconds(100) : std::chrono::seconds(2));

        try
        {
            int newly = safe_refresh_seh();
            if (first_read && newly > 0)
            {
                spdlog::info("Initial state: {} pieces hidden",
                             goblin::collected::collected_count());
                first_read = false;
            }
        }
        catch (...)
        {
        }
    }
}

bool WINAPI DllMain(HINSTANCE dll_instance, unsigned int fdw_reason, void *lpv_reserved)
{
    if (fdw_reason == DLL_PROCESS_ATTACH)
    {
        wchar_t dll_filename[MAX_PATH] = {0};
        GetModuleFileNameW(dll_instance, dll_filename, MAX_PATH);
        auto folder = std::filesystem::path(dll_filename).parent_path();
        g_mod_folder = folder;

        setup_logger(folder / "logs" / "MapForGoblins.log");

        spdlog::info("Map For Goblins DLL v{}", PROJECT_VERSION);
        goblin::load_config(folder / "MapForGoblins.ini");

        if (goblin::config::debugLogging)
            spdlog::default_logger()->set_level(spdlog::level::debug);

        mod_thread = std::thread([]()
                                 {
            try
            {
                setup_mod();
            }
            catch (std::runtime_error const &e)
            {
                spdlog::error("Error initializing mod: {}", e.what());
                modutils::deinitialize();
                spdlog::shutdown();
            } });
    }
    else if (fdw_reason == DLL_PROCESS_DETACH && lpv_reserved != nullptr)
    {
        try
        {
            mod_thread.join();
            modutils::deinitialize();
        }
        catch (std::runtime_error const &e)
        {
            spdlog::error("Error deinitializing: {}", e.what());
        }
        spdlog::shutdown();
    }
    return true;
}
