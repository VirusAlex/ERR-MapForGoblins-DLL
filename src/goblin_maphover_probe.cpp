// Temporary investigation probe (test builds only, not shipped).
//
// Hooks FUN_14087a8e0 (v2.6.2.0) = the world-map dialog's per-frame "update the
// PlaceName panel for the currently-focused pin" routine. The dialog picks the pin
// NEAREST the reticle within a radius each frame (FUN_1409dbf80) and passes it here
// to display its name - so the item we receive IS the icon the game shows the popup
// for. __fastcall(rcx = PlaceName panel, RDX = item pin, r8 = map area); item may be
// null when nothing is focused.
//
// item = CS::WorldMapPointPinData (vtable RVA 0x2AD6688). Non-virtual identity fields
// (no vcall needed): +0x0c valid byte, +0x10/+0x14/+0x18 pos X/Z/Y (map space, same
// frame the mod projects), +0x5c iconId (== our injected iconId). We only READ + call
// the original; behaviour is unchanged. Logging is throttled to identity changes.
#include "goblin_maphover_probe.hpp"

#include "modutils.hpp"

#include <spdlog/spdlog.h>
#include <windows.h>

#include <cmath>
#include <cstdint>

namespace
{
    using PlaceNameFn = void *(void *, void *, void *);  // __fastcall
    PlaceNameFn *o_placename = nullptr;

    uintptr_t g_base = 0;
    constexpr uintptr_t PIN_VT_RVA = 0x2AD6688;  // CS::WorldMapPointPinData vtable

    // throttle: only log when the focused pin identity changes
    uintptr_t last_vt = 0;
    int last_icon = -0x7fffffff;
    float last_x = -1e9f, last_z = -1e9f;

    // read an int from item+off, SEH-guarded (per-frame, must never crash the game)
    int safe_i32(uint8_t *base, size_t off)
    {
        int v = -1;
        __try { v = *reinterpret_cast<int *>(base + off); } __except (EXCEPTION_EXECUTE_HANDLER) { v = -1; }
        return v;
    }
    uintptr_t safe_ptr(uint8_t *base, size_t off)
    {
        uintptr_t v = 0;
        __try { v = *reinterpret_cast<uintptr_t *>(base + off); } __except (EXCEPTION_EXECUTE_HANDLER) { v = 0; }
        return v;
    }
    // find the first offset in [lo,hi) holding an int in the marker textId band 500M..600M
    int find_textid(uint8_t *base, size_t lo, size_t hi, size_t &at)
    {
        for (size_t o = lo; o + 4 <= hi; o += 4)
        {
            int v = safe_i32(base, o);
            if (v >= 500000000 && v < 600000000) { at = o; return v; }
        }
        at = 0; return 0;
    }

    void *placename_detour(void *panel, void *item, void *map_area)
    {
        if (item)
        {
            auto *b = reinterpret_cast<uint8_t *>(item);
            uintptr_t vt = *reinterpret_cast<uintptr_t *>(b);
            uintptr_t rva = g_base ? (vt - g_base) : vt;
            int icon = safe_i32(b, 0x5c);
            float px = *reinterpret_cast<float *>(b + 0x10);
            float pz = *reinterpret_cast<float *>(b + 0x14);
            uint8_t valid = *(b + 0x0c);

            if (vt != last_vt || icon != last_icon ||
                std::fabs(px - last_x) > 0.5f || std::fabs(pz - last_z) > 0.5f)
            {
                last_vt = vt; last_icon = icon; last_x = px; last_z = pz;
                // scan the item itself for a textId, and follow the embedded param row (item+0x248)
                size_t iat = 0, pat = 0;
                int item_tid = find_textid(b, 0x40, 0x260, iat);
                int lines = safe_i32(b, 0x230);
                uintptr_t prow = safe_ptr(b, 0x248);
                int row_tid = 0; size_t rat = 0; int row_icon = -1;
                if (prow > 0x10000)
                {
                    auto *rb = reinterpret_cast<uint8_t *>(prow);
                    row_tid = find_textid(rb, 0x00, 0x120, rat);
                    // WORLD_MAP_POINT_PARAM_ST.iconId is a u16 near the row head
                    row_icon = safe_i32(rb, 0x00) & 0xffff;
                }
                spdlog::info("[maphover] pin vt_rva=0x{:X}{} icon@5c={} valid={} pos=({:.1f},{:.1f}) "
                             "lines={} item_tid={}@0x{:X} prow=0x{:X} row_tid={}@0x{:X} rowhead_u16={}",
                             rva, (rva == PIN_VT_RVA ? " <MapPointPin>" : ""),
                             icon, static_cast<int>(valid), px, pz,
                             lines, item_tid, iat, prow, row_tid, rat, row_icon);
            }
        }
        return o_placename(panel, item, map_area);
    }
}  // namespace

void goblin::maphover_probe::setup()
{
    try
    {
        g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        auto *fn = modutils::hook<PlaceNameFn>(
            {.aob = "40 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 D9 48 81 EC B0 00 00 00 "
                    "48 C7 45 B7 FE FF FF FF 48 89 9C 24 08 01 00 00 48 8B 05 ?? ?? ?? ?? "
                    "48 33 C4 48 89 45 1F 49 8B F8"},
            placename_detour, o_placename);
        spdlog::info("[maphover] probe hook armed @ 0x{:X}", reinterpret_cast<uintptr_t>(fn));
    }
    catch (const std::exception &e)
    {
        spdlog::error("[maphover] probe setup failed: {}", e.what());
    }
}
