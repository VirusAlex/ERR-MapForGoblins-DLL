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
#include "goblin_kindling.hpp"
#include "goblin_logic.hpp"
#include "goblin_markers.hpp"
#include "goblin_messages.hpp"
#include "goblin_overlay.hpp"
#include "goblin_map_timing.hpp"
#include "goblin_gfx_probe.hpp"
#include "goblin_maphover.hpp"

#include "version.h"

static std::thread mod_thread;

// SEH wrapper - catches access violations from refresh() during multiplayer transitions
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

static int safe_kindling_refresh_seh()
{
    __try
    {
        return goblin::kindling::refresh();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

static void safe_flag_or_pairs_seh()
{
    __try
    {
        goblin::apply_flag_or_pairs();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

static void safe_apply_category_visibility_seh()
{
    __try
    {
        goblin::apply_category_visibility();
        goblin::apply_focus_highlight();  // keep focus glow/labels in sync as the collected set changes
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

static void safe_gfx_tick_seh()
{
    __try
    {
        goblin::gfx_probe::tick(); // charId collision self-heal + (debug_logging) diagnostics
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

// ── SEH-guarded init-phase wrappers ──
// MSVC's /EHsc disallows __try in functions that contain C++ objects with
// destructors, so each init step goes through a plain C-style adapter +
// a shared invoker. If any step access-violates (e.g. another mod shifted
// the game's memory map mid-init), we log and continue - losing that
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
static void init_kindling()         { goblin::kindling::initialize(); }
static void init_inject_entries()   { goblin::inject_map_entries(); }
static void init_apply_map_logic()  { goblin::apply_map_logic(); goblin::apply_worldmap_fragment_bypass(); }
static void init_tutorial_popup()   { goblin::inject_tutorial_popup_rows(); }
static void init_setup_messages()   { goblin::setup_messages(); }
static void init_live_loot()        { goblin::refresh_loot_from_itemlot(); }
static void init_overlay()          { goblin::overlay::setup(); }
static void init_map_timing()       { goblin::map_timing::setup(); }
static void init_gfx_probe()        { goblin::gfx_probe::setup(); }
static void init_maphover()         { goblin::maphover::setup(); }

static void safe_init_step(InitFn fn, const char *name)
{
    if (!seh_invoke_void(fn))
        spdlog::error("SEH exception in init step '{}' - feature may be degraded", name);
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

// Manual per-marker hide: on hide_marker_key, hide/unhide the marker under the map
// cursor (goblin::maphover::hovered_row() -> our WorldMapPointParam row). Applies live
// and persists. Runs on its own thread (like the other hotkeys).
static void manual_hide_hotkey_loop()
{
    bool prev = false;
    while (true)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        if (!goblin::config::enableManualHide) { prev = false; continue; }
        bool down = goblin::overlay::key_down(static_cast<int>(goblin::config::hideMarkerKey));
        if (down && !prev)
        {
            void *row = goblin::maphover::hovered_row();
            if (row)
            {
                goblin::ManualHideResult res = goblin::toggle_hovered_marker(row);
                if (res.matched)
                {
                    goblin::reapply_live_settings();
                    goblin::persist_manual_hidden();
                    spdlog::info("[hide] marker textId={} -> {} ({} hidden total)",
                                 res.textId, res.now_hidden ? "HIDDEN" : "shown",
                                 goblin::manual_hidden_count());
                }
            }
        }
        prev = down;
    }
}

static void setup_mod()
{
    safe_init_step(&init_modutils,    "modutils::initialize");

    // Arm + ENABLE the icon-injection hooks FIRST - before the params wait and before any other work. They
    // are passive (they fire when the worldmap movie loads its DefineSprite-171) and depend only on the
    // loaded exe, not on the game's params/world. The worldmap movie CANNOT be re-injected after it loads
    // (its bitmap-register load-context is transient), so these hooks must be live before the player first
    // opens the world map. Arming them at the earliest possible moment - not behind from::params or any
    // delay - makes icon injection robust to WHEN the DLL itself gets injected, as long as that is before
    // the first map open (always true for a process-start loader).
    safe_init_step(&init_gfx_probe, "gfx_probe::setup");
    try { modutils::enable_hooks(); }  // apply just the gfx_probe hooks queued so far (MH_ApplyQueued)
    catch (const std::exception &e) { spdlog::error("handler setup (gfx) FAILED: {}", e.what()); }

    // Blocks (polls internally) until the game's param tables are fully loaded. THIS is the real
    // "wait for game init" - no fixed startup sleep is used (a sleep would also push the hook-arming
    // above past the worldmap movie load on fast Proton boots, which breaks icons).
    safe_init_step(&init_from_params, "from::params::initialize");

    // Load the user's manually-hidden marker set BEFORE the first visibility apply,
    // so hidden markers are already hidden on the first map open.
    if (goblin::config::enableManualHide)
    {
        goblin::set_hidden_file(g_mod_folder / "MapForGoblins_hidden.txt");
        goblin::load_manual_hidden(g_mod_folder / "MapForGoblins_hidden.txt");
    }

    safe_init_step(&init_collected,       "collected::initialize");
    safe_init_step(&init_kindling,        "kindling::initialize");
    safe_init_step(&init_inject_entries,  "add_map_entries");
    safe_init_step(&init_apply_map_logic, "apply_map_logic");
    // setup_messages MUST precede inject_tutorial_popup_rows: it allocates the
    // dynamic codex-toast FMG ids (goblin::g_toast_fmg_id) that the popup rows
    // point their textId at. (It also builds the PlaceName textId remap used by
    // the marker rows injected above.)
    safe_init_step(&init_setup_messages,  "setup_messages");
    safe_init_step(&init_tutorial_popup,  "add_tutorial_rows");
    safe_init_step(&init_live_loot,       "refresh_loot_from_itemlot");
    safe_init_step(&init_overlay,         "overlay::setup");
    safe_init_step(&init_map_timing,      "map_timing::setup");
    safe_init_step(&init_maphover,        "maphover::setup");  // marker hover detection (hide/overlay)

    try
    {
        modutils::enable_hooks();  // apply the remaining hooks
    }
    catch (const std::exception &e)
    {
        spdlog::error("handler setup FAILED: {}", e.what());
    }

    spdlog::info("Initialization complete");

    if (goblin::config::enableMarkerDump)
    {
        goblin::markers::set_output_path(g_mod_folder / "logs" / "MapForGoblins_markers.log");
        std::thread(goblin::markers::hotkey_loop).detach();
        spdlog::info("Marker dump hotkey: VK 0x{:X}", goblin::config::markerDumpKey);
    }

    if (goblin::config::enableToggleHotkey)
    {
        std::thread(goblin::toggle_hotkey_loop).detach();
        spdlog::info("Icon toggle hotkey: VK 0x{:X}", goblin::config::toggleInjectionKey);
    }

    if (goblin::config::enableManualHide)
    {
        std::thread(manual_hide_hotkey_loop).detach();
        spdlog::info("Manual marker-hide hotkey: VK 0x{:X}", goblin::config::hideMarkerKey);
    }

    // The watcher is the single owner of the WorldMapPointParam state - it
    // applies the master-off flag (set by the toggle hotkey OR the overlay's
    // "Show map icons" checkbox). Run it whenever EITHER path can set that flag,
    // so the overlay's master switch works even if the toggle hotkey is disabled.
    if (goblin::config::enableToggleHotkey || goblin::config::enableOverlay)
    {
        std::thread(goblin::menu_auto_toggle_loop).detach();
        spdlog::info("Icon-state watcher started (icons EXPANDED always; master show/hide via hotkey or overlay)");
    }

    bool first_read = true;
    int prev_collected = -1, prev_kindling = -1;
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

        try
        {
            safe_kindling_refresh_seh();
        }
        catch (...)
        {
        }

        try
        {
            safe_gfx_tick_seh(); // icon collision self-heal + diagnostics (overlay-independent)
        }
        catch (...)
        {
        }

        try
        {
            safe_flag_or_pairs_seh();
        }
        catch (...)
        {
        }

        // When the collected set changes, refresh the live visibility gate so
        // collected pieces/nodes/kindling hide (and revealed ones reappear) on
        // the OPEN map without a reopen. Category-toggle changes come in via the
        // overlay (reapply_live_settings); this covers in-world collection.
        int cc = goblin::collected::collected_count();
        int kc = goblin::kindling::collected_count();
        if (cc != prev_collected || kc != prev_kindling)
        {
            prev_collected = cc;
            prev_kindling = kc;
            safe_apply_category_visibility_seh();
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

        spdlog::info("Map For Goblins DLL v{} [{}] ({})", PROJECT_VERSION, BUILD_NAME, GIT_HASH);
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
                spdlog::error("mod init failed: {}", e.what());
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
            spdlog::error("teardown failed: {}", e.what());
        }
        spdlog::shutdown();
    }
    return true;
}
