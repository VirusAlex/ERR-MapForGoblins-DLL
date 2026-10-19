#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "../sdk/mfg_map_api.h"
#include "generated_shared/goblin_map_icons.hpp"
#include "goblin_build_variants.hpp"
#include "goblin_inject.hpp"
#include "goblin_map_api.hpp"

namespace {
std::mutex depth_mutex;
std::unordered_map<uint64_t, uint16_t> depths;
bool reset_pending = false;
std::atomic_bool rows_ready{false};

bool usable(const goblin::NativeMarkerPoint &point) {
    return point.visible && point.source_icon_id >= 0 && point.gx <= 255 && point.gz <= 255 && std::isfinite(point.px) && std::isfinite(point.pz);
}

int viewPoints(uint32_t layer, const MfgMapPoint **output, uint32_t *count) noexcept {
    if (output)
        *output = nullptr;
    if (count)
        *count = 0;
    if (!output || !count || layer > 2)
        return MFG_MAP_INVALID;
    // Each calling thread owns the storage behind its borrowed point view.
    // Native refreshes and other threads cannot invalidate that view.
    thread_local std::vector<MfgMapPoint> points;
    points.clear();
    if (!rows_ready.load(std::memory_order_acquire))
        return MFG_MAP_OK;
    try {
        struct Collector {
            std::vector<MfgMapPoint> &points;
            uint32_t order = 24;
            bool overflow = false;
        } collector{points};
        // Fill this thread's view storage from the native marker enumeration.
        goblin::visit_native_markers(
            static_cast<int>(layer),
            [](const auto &point, void *context) {
                auto &sink = *static_cast<Collector *>(context);
                const uint32_t fallback = (point.original_row_id & goblin::NATIVE_HIGHLIGHT_KEY_BIT)
                                              ? 50000 + static_cast<uint32_t>(point.original_row_id & ~goblin::NATIVE_HIGHLIGHT_KEY_BIT)
                                              : sink.order++;
                if (!usable(point))
                    return;
                if (sink.points.size() == MFG_MAP_MAX_POINTS) {
                    sink.overflow = true;
                    return;
                }
                sink.points.push_back({point.original_row_id, static_cast<uint32_t>(point.source_icon_id),
                                       (uint32_t(point.area) << 24) | (uint32_t(point.gx) << 16) | (uint32_t(point.gz) << 8), point.px, point.pz, fallback, 0});
            },
            &collector);
        if (collector.overflow) {
            points.clear();
            return MFG_MAP_FAILED;
        }
        {
            // Read native child depths after releasing the layout lock.
            // Neither lock is held while the caller consumes the view.
            std::lock_guard lock(depth_mutex);
            for (auto &point: points)
                if (const auto depth = depths.find(point.id); depth != depths.end())
                    point.draw_order = depth->second;
        }
        const auto before = [](const auto &a, const auto &b) { return a.draw_order < b.draw_order; };
        if (!std::is_sorted(points.begin(), points.end(), before))
            std::stable_sort(points.begin(), points.end(), before);
        if (!points.empty())
            *output = points.data();
        *count = static_cast<uint32_t>(points.size());
        return MFG_MAP_OK;
    } catch (...) {
        points.clear();
        return MFG_MAP_FAILED;
    }
}

int getIcon(uint32_t index, MfgMapIcon *output) noexcept {
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
} // namespace

void goblin::map_api::publish_rows() noexcept { rows_ready.store(true, std::memory_order_release); }

void goblin::map_api::record_depth(uint64_t id, uint16_t depth) noexcept {
    try {
        std::lock_guard lock(depth_mutex);
        if (reset_pending) {
            depths.clear();
            reset_pending = false;
        }
        depths[id] = depth;
    } catch (...) {
        // An optional client must not affect native marker creation.
    }
}

void goblin::map_api::reset_depths() noexcept {
    std::lock_guard lock(depth_mutex);
    // Keep the last native order while the map is closed. Replace it when
    // the next generation actually starts creating children.
    reset_pending = true;
}

extern "C" __declspec(dllexport) const MfgMapAPI *MfgGetMapAPI(uint32_t version) noexcept {
    static const MfgMapAPI api{sizeof(MfgMapAPI), MFG_MAP_ABI, sizeof(MfgMapPoint), sizeof(MfgMapIcon), static_cast<uint32_t>(goblin::generated::MAP_ICON_TAG_COUNT), 0,
                               viewPoints,        getIcon};
    return goblin::variants::kNativeMarkers && version == MFG_MAP_ABI ? &api : nullptr;
}
