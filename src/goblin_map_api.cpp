#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>

#include "../sdk/mfg_map_api.h"
#include "generated_shared/goblin_map_icons.hpp"
#include "goblin_build_variants.hpp"
#include "goblin_collected.hpp" // read_player_map_id(): is a character in the world
#include "goblin_config.hpp"
#include "goblin_inject.hpp"
#include "goblin_map_api.hpp"

// The map API (sdk/mfg_map_api.h): another module reads the markers MapForGoblins draws.
//
// A reader asks as often as it draws - every frame, often from the game's render thread - so the
// answer must not be computed on the reader's call. One snapshot per layer is built HERE, on the
// poll thread (maintain, every 100 ms), and handed out as a shared immutable buffer: view_points is a
// pointer copy under a short lock, whatever the reader's frame rate. Building one costs a full
// visibility + de-overlap pass (~2 ms, the same pass the open world map runs ~5x a second), done
// from a private layout (goblin::api_marker_points) so it neither waits for the map nor disturbs it.
//
// When it is rebuilt: only for a layer a reader asked for in the last kReaderIdleMs; as soon as what
// should be on screen changed (goblin::visibility_epoch: a pickup, a toggle, a focus, a manual hide,
// a world-state flip), but not more often than kMinGapMs; and at least every kMaxAgeMs, because a
// loot pickup or a boss kill that only sets an event flag changes the answer without telling anyone.
// Nothing is built, and nothing else of this file runs, until a reader has asked for the table.
namespace
{
using Clock = std::chrono::steady_clock;

constexpr int64_t kReaderIdleMs = 3000;
constexpr int64_t kMinGapMs = 250;
constexpr int64_t kMaxAgeMs = 1000;

int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

struct Snapshot
{
    std::vector<MfgMapPoint> points;
    int64_t built_ms = 0;
    uint32_t epoch = 0;
    bool overflow = false;  // more points than MFG_MAP_MAX_POINTS: reported as MFG_MAP_FAILED
};

std::atomic_bool g_rows_ready{false};
std::atomic_bool g_reader{false};               // MfgGetMapAPI has handed the table out
std::atomic<int64_t> g_asked_ms[3] = {0, 0, 0}; // the last view_points per layer, 0 = never

std::mutex g_pub_mutex;
std::shared_ptr<const Snapshot> g_pub[3];
// Keeps the calling thread's borrowed view of a layer alive until its next view_points of that layer
// (the header's contract) - one per layer, so reading two layers in one frame keeps both valid.
thread_local std::shared_ptr<const Snapshot> t_view[3];
// Replaced snapshots, held by the poll thread until no reader holds them, so the last reference (and
// the free of up to a megabyte) never drops on a reader's thread - often the game's render thread.
std::vector<std::shared_ptr<const Snapshot>> g_retired;
// What the builds cost, for the debug-log summary. Poll thread only.
struct BuildCost
{
    int64_t builds, sum_us, max_us, since_ms;
} g_cost = {0, 0, 0, 0};

// Native child depths, recorded while the map's icon factory builds its children - so a reader can
// stack its icons the way the map does. Only kept once a reader exists.
std::mutex g_depth_mutex;
std::unordered_map<uint64_t, uint16_t> g_depths;
uint16_t g_depth_top = 0;  // the deepest of them
bool g_reset_pending = false;

std::shared_ptr<const Snapshot> published(uint32_t layer)
{
    std::lock_guard lock(g_pub_mutex);
    return g_pub[layer];
}

int view_points(uint32_t layer, const MfgMapPoint **output, uint32_t *count) noexcept
{
    if (output)
        *output = nullptr;
    if (count)
        *count = 0;
    if (!output || !count || layer > 2)
        return MFG_MAP_INVALID;
    g_asked_ms[layer].store(now_ms(), std::memory_order_relaxed);
    // This thread's previous view of the layer is released here, the new one held until the next call.
    auto &view = t_view[layer];
    view = published(layer);
    if (!view)
        return MFG_MAP_OK;  // not built yet: the next poll (<= 100 ms) builds it
    if (view->overflow)
        return MFG_MAP_FAILED;
    if (!view->points.empty())
    {
        *output = view->points.data();
        *count = static_cast<uint32_t>(view->points.size());
    }
    return MFG_MAP_OK;
}

int get_icon(uint32_t index, MfgMapIcon *output) noexcept
{
    if (!output)
        return MFG_MAP_INVALID;
    *output = {};
    if (index >= static_cast<uint32_t>(goblin::generated::MAP_ICON_TAG_COUNT))
        return MFG_MAP_INVALID;
    const auto &icon = goblin::generated::MAP_ICON_TAGS[index];
    if (icon.srcIconId < 0)
        return MFG_MAP_INVALID;
    *output = {icon.tag, icon.tagLen, static_cast<uint32_t>(icon.srcIconId), icon.matrix, icon.matrixLen, 0};
    return MFG_MAP_OK;
}

std::shared_ptr<const Snapshot> build(uint32_t layer, int64_t now, uint32_t epoch)
{
    static std::vector<goblin::NativeMarkerPoint> points;  // poll thread only
    goblin::api_marker_points(static_cast<int>(layer), points);
    auto snap = std::make_shared<Snapshot>();
    snap->built_ms = now;
    snap->epoch = epoch;
    snap->points.reserve(points.size());
    for (const auto &p : points)
    {
        if (p.source_icon_id < 0 || p.gx > 255 || p.gz > 255 || !std::isfinite(p.px) || !std::isfinite(p.pz))
            continue;
        snap->points.push_back({p.original_row_id, static_cast<uint32_t>(p.source_icon_id),
                                (uint32_t(p.area) << 24) | (uint32_t(p.gx) << 16) | (uint32_t(p.gz) << 8), p.px,
                                p.pz, 0, 0});
    }
    if (snap->points.size() > MFG_MAP_MAX_POINTS)
    {
        snap->points = {};
        snap->overflow = true;
        return snap;
    }
    {
        // Children the map built carry their real depth. Before it has built any, the order is the row
        // order in the normal band and the pool index in the ring band. A point it has not built YET
        // (a badge of a boss killed since the map was opened) goes above every known child - where the
        // map itself appends the child it creates for it next time.
        std::lock_guard lock(g_depth_mutex);
        uint32_t next = g_depths.empty() ? 24 : uint32_t(g_depth_top) + 1;
        for (auto &point : snap->points)
        {
            if (const auto depth = g_depths.find(point.id); depth != g_depths.end())
                point.draw_order = depth->second;
            else if (point.id & goblin::NATIVE_HIGHLIGHT_KEY_BIT)
                point.draw_order = 50000 + static_cast<uint32_t>(point.id & 0xFFFF);
            else
                point.draw_order = next++;
        }
    }
    const auto before = [](const MfgMapPoint &a, const MfgMapPoint &b) { return a.draw_order < b.draw_order; };
    if (!std::is_sorted(snap->points.begin(), snap->points.end(), before))
        std::stable_sort(snap->points.begin(), snap->points.end(), before);
    return snap;
}
} // namespace

void goblin::map_api::publish_rows() noexcept
{
    g_rows_ready.store(true, std::memory_order_release);
}

void goblin::map_api::maintain()
{
    if (!g_reader.load(std::memory_order_acquire) || !g_rows_ready.load(std::memory_order_acquire))
        return;
    try
    {
        // use_count() == 1: only this list still holds it, and an unpublished snapshot cannot be
        // picked up again.
        std::erase_if(g_retired, [](const auto &s) { return s.use_count() == 1; });
        const int64_t now = now_ms();
        const uint32_t epoch = goblin::visibility_epoch();
        int player = -1;  // asked once, and only when a layer is due
        for (uint32_t layer = 0; layer < 3; ++layer)
        {
            const int64_t asked = g_asked_ms[layer].load(std::memory_order_relaxed);
            if (asked == 0 || now - asked > kReaderIdleMs)
                continue;
            auto current = published(layer);
            if (current)
            {
                const int64_t age = now - current->built_ms;
                if (age < kMinGapMs || (current->epoch == epoch && age < kMaxAgeMs))
                    continue;
            }
            // Built only while a character stands in the world. The marker rows live in the param file
            // the GAME owns, and on the way out of the game it is freed under this thread; at the
            // title and through loading screens there is nothing to show anyway. The last snapshot
            // stays published meanwhile. The race left over is the caller's __try.
            if (player < 0)
            {
                uint32_t map_id = 0;
                player = goblin::collected::read_player_map_id(map_id) ? 1 : 0;
            }
            if (!player)
                break;
            const auto t0 = Clock::now();
            auto snap = build(layer, now, epoch);
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count();
            const size_t n = snap->points.size();
            const bool overflow = snap->overflow;
            {
                std::lock_guard lock(g_pub_mutex);
                g_pub[layer] = std::move(snap);
            }
            if (current)
                g_retired.push_back(std::move(current));
            // The first build per layer says what it cost; under debug logging a 2-minute summary too.
            static bool s_said[3] = {false, false, false};
            if (!s_said[layer])
            {
                s_said[layer] = true;
                spdlog::info("[mapapi] layer {}: {} point(s) built in {} us{}", layer, n, us,
                             overflow ? " - over the API's point cap, reported as a failure" : "");
            }
            g_cost.builds++;
            g_cost.sum_us += us;
            g_cost.max_us = std::max<int64_t>(g_cost.max_us, us);
        }
        if (goblin::config::debugLogging && g_cost.builds && now - g_cost.since_ms >= 120000)
        {
            spdlog::info("[mapapi] {} s: {} build(s), avg {} us, worst {} us", (now - g_cost.since_ms) / 1000,
                         g_cost.builds, g_cost.sum_us / g_cost.builds, g_cost.max_us);
            g_cost = {0, 0, 0, now};
        }
        else if (!g_cost.since_ms)
            g_cost.since_ms = now;
    }
    catch (...)
    {
        // A C++ throw (an allocation) must not reach the poll loop; the next poll tries again. A fault
        // reading the game's rows is not a C++ exception - dllmain's __try takes that one.
    }
}

void goblin::map_api::record_depth(uint64_t id, uint16_t depth) noexcept
{
    if (!g_reader.load(std::memory_order_relaxed))
        return;
    try
    {
        std::lock_guard lock(g_depth_mutex);
        if (g_reset_pending)
        {
            g_depths.clear();
            g_depth_top = 0;
            g_reset_pending = false;
        }
        g_depths[id] = depth;
        g_depth_top = std::max(g_depth_top, depth);
    }
    catch (...)
    {
        // An optional reader must not affect native marker creation.
    }
}

void goblin::map_api::reset_depths() noexcept
{
    if (!g_reader.load(std::memory_order_relaxed))
        return;
    std::lock_guard lock(g_depth_mutex);
    // Keep the last native order while the map is closed. Replace it when
    // the next generation actually starts creating children.
    g_reset_pending = true;
}

extern "C" __declspec(dllexport) const MfgMapAPI *MfgGetMapAPI(uint32_t version) noexcept
{
    static const MfgMapAPI api{sizeof(MfgMapAPI),
                               MFG_MAP_ABI,
                               sizeof(MfgMapPoint),
                               sizeof(MfgMapIcon),
                               static_cast<uint32_t>(goblin::generated::MAP_ICON_TAG_COUNT),
                               0,
                               view_points,
                               get_icon};
    if (!goblin::variants::kNativeMarkers || version != MFG_MAP_ABI)
        return nullptr;
    if (!g_reader.exchange(true))
        spdlog::info("[mapapi] a reader asked for the map API (ABI {})", version);
    return &api;
}
