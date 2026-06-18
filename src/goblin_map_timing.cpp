#include "goblin_map_timing.hpp"

#include "goblin_config.hpp"
#include "modutils.hpp"

#include <spdlog/spdlog.h>

#include <atomic>
#include <mutex>
#include <vector>

#include <intrin.h> // _ReturnAddress

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// World-map open optimization (config::fastMapOpen). With ~7000 markers the game's
// per-marker widget relayout on every map open takes a long time. We wrap that
// relayout (refresh fn) and the child-list passes (ce390) at the map's per-marker
// dispatcher call site, gated by return address so other UI is left as-is, and:
//   - reuse the existing layout on RE-open (after the first build), since it is
//     unchanged;
//   - on the FIRST open, defer the relayout and run it a budget per frame so the
//     map opens smoothly (ce390 runs in-line - deferring it left the map a blank
//     frame).
// Every call site is resolved by pattern (resilient to game updates). The
// WorldMapDialog destructor is wrapped only to clear the replay queue on close.
// Full notes: docs/research_worldmap_internals.md.
namespace
{
    using Fn = void *(void *, void *, void *, void *);
    using DtorFn = void *(void *);
    Fn *o_refresh = nullptr, *o_ce390 = nullptr;
    DtorFn *o_wmd_dtor = nullptr;

    uintptr_t g_map_callsite = 0;       // ret addr of the map's per-marker refresh call
    uintptr_t g_ce_gate[3] = {0, 0, 0}; // ret addrs of the 3 ce390 calls
    std::atomic<bool> g_built{false};
    std::atomic<int> g_refresh_n{0};    // refreshes that ran; latches g_built after the first build

    // Amortize: queue the first open's relayout calls, replay a budget per frame.
    // Build, replay and close all run on the render thread; the mutex only guards a
    // concurrent reader and keeps the queue consistent.
    struct RefreshArgs { void *a, *b, *c, *d; };
    std::mutex g_amrt_mtx;
    std::vector<RefreshArgs> g_amrt_queue;
    size_t g_amrt_pos = 0;
    constexpr size_t AMRT_PER_FRAME = 500;

    void amortize_pump()
    {
        if (!o_refresh) return;
        RefreshArgs batch[AMRT_PER_FRAME];
        size_t n = 0;
        {
            std::lock_guard<std::mutex> lk(g_amrt_mtx);
            while (n < AMRT_PER_FRAME && g_amrt_pos < g_amrt_queue.size())
                batch[n++] = g_amrt_queue[g_amrt_pos++];
            if (g_amrt_pos >= g_amrt_queue.size() && !g_amrt_queue.empty())
            {
                g_amrt_queue.clear();
                g_amrt_pos = 0;
            }
        }
        for (size_t i = 0; i < n; ++i)
            o_refresh(batch[i].a, batch[i].b, batch[i].c, batch[i].d);
    }

    // Latch g_built after the first map build (a burst of build-site refreshes) so
    // later opens skip the redundant relayout. Runs every frame from on_present.
    void latch_built()
    {
        if (!g_built.load(std::memory_order_relaxed) &&
            g_refresh_n.load(std::memory_order_relaxed) > 1000)
        {
            g_built.store(true, std::memory_order_relaxed);
            spdlog::info("[fastmap] first map build complete; reopen reuses layout");
        }
    }

    void *refresh_detour(void *a, void *b, void *c, void *d)
    {
        uintptr_t ret = (uintptr_t)_ReturnAddress();
        if (ret == g_map_callsite) // the map's per-marker build call (other UI left as-is)
        {
            // RE-open: reuse the existing layout, no relayout needed (fastest).
            if (goblin::config::fastMapOpen && g_built.load(std::memory_order_relaxed))
                return nullptr;
            // FIRST open: defer + replay over frames (smooth open). The WMD destructor
            // wrapper clears the queue on close so we never replay into freed markers.
            if (goblin::config::fastMapOpen)
            {
                std::lock_guard<std::mutex> lk(g_amrt_mtx);
                g_amrt_queue.push_back({a, b, c, d});
                return nullptr;
            }
        }
        void *r = o_refresh(a, b, c, d);
        g_refresh_n.fetch_add(1, std::memory_order_relaxed); // latch counter
        return r;
    }

    void *ce390_detour(void *a, void *b, void *c, void *d)
    {
        uintptr_t ret = (uintptr_t)_ReturnAddress();
        bool map_site = (ret == g_ce_gate[0] || ret == g_ce_gate[1] || ret == g_ce_gate[2]);
        // Reuse on RE-open (structure already built); runs in-line on first open.
        if (goblin::config::fastMapOpen && g_built.load(std::memory_order_relaxed) && map_site)
            return nullptr;
        return o_ce390(a, b, c, d);
    }

    void *wmd_dtor_detour(void *self)
    {
        // Map closing: drop any pending amortize replay so we never lay out freed markers.
        {
            std::lock_guard<std::mutex> lk(g_amrt_mtx);
            g_amrt_queue.clear();
            g_amrt_pos = 0;
        }
        return o_wmd_dtor(self);
    }
}

void goblin::map_timing::on_present()
{
    amortize_pump(); // replay deferred first-open relayout, a budget per frame
    latch_built();   // enable the re-open skip after the first build
}

void goblin::map_timing::setup()
{
    if (!goblin::config::fastMapOpen) return;

    // Resolve the per-marker call sites by byte pattern (resilient to game updates):
    // the reuse/amortize gate keys on the return address of the refresh + 3 ce390
    // calls in the map's per-marker dispatcher. The call TARGETS (E8 rel32) are
    // wildcarded so a game update that moves functions doesn't break the match; the
    // return addresses are fixed offsets from the match. On a miss, g_map_callsite
    // stays 0 -> safe no-op.
    try
    {
        uintptr_t m = reinterpret_cast<uintptr_t>(modutils::scan<void>(
            {.aob = "48 8B 89 18 01 00 00 E8 ?? ?? ?? ?? 33 D2 48 8B CF E8 ?? ?? ?? ?? "
                    "BA 01 00 00 00 48 8B CF E8 ?? ?? ?? ?? BA 03 00 00 00 48 8B CF "
                    "E8 ?? ?? ?? ?? 48 8B 5C 24 30 B0 01"}));
        g_map_callsite = m + 0x0C; // ret of `call refresh`
        g_ce_gate[0] = m + 0x16;   // ret of `call ce390` (edx=0)
        g_ce_gate[1] = m + 0x23;   // ret of `call ce390` (edx=1)
        g_ce_gate[2] = m + 0x30;   // ret of `call ce390` (edx=3)
        spdlog::info("[fastmap] resolved map call sites @ 0x{:X}", m);
    }
    catch (const std::exception &e)
    {
        spdlog::warn("[fastmap] map call site not found ({}); fast map open off", e.what());
        return;
    }

    try
    {
        modutils::hook<Fn>(
            {.aob = "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B 41 20 "
                    "48 8B D9 48 8B 50 10 48 8B"},
            refresh_detour, o_refresh);
        modutils::hook<Fn>(
            {.aob = "40 53 48 83 EC 50 33 C0 89 54 24 48 4C 8D 81 C8 00 00 00 89"},
            ce390_detour, o_ce390);
        // Clear the amortize queue on map close (never replay into freed markers).
        modutils::hook<DtorFn>(
            {.aob = "48 89 4C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 30 "
                    "48 C7 45 F0 FE FF FF FF 48 89 9C 24 88 00 00 00 48 8B F1 48 8D 05 "
                    "B7 A3 16 02"},
            wmd_dtor_detour, o_wmd_dtor);
        spdlog::info("[fastmap] fast map open ready");
    }
    catch (const std::exception &e)
    {
        spdlog::warn("[fastmap] setup failed: {}", e.what());
    }
}
