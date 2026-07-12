// World-map hover detection (production). See goblin_maphover.hpp.
//
// FUN_14087a8e0 (v2.6.2.0) = the map dialog's per-frame "update the PlaceName panel for
// the currently-focused pin". The dialog picks the pin NEAREST the reticle within a
// radius each frame and passes it here to display its name - so this item IS the icon
// the game highlights + popups. __fastcall(rcx = panel, RDX = item pin, r8 = map area);
// item is null when nothing is focused.
//
// item = CS::WorldMapPointPinData (vtable RVA 0x2AD6688). The underlying param row is at
// *(item+0x248) (live-confirmed: row+0x30 holds our offset-encoded textId). We publish
// that row ptr; the inject layer matches it to one of our CategoryRow::p.
#include "goblin_maphover.hpp"

#include "modutils.hpp"

#include <spdlog/spdlog.h>
#include <windows.h>

#include <atomic>
#include <cstdint>

namespace
{
    using PlaceNameFn = void *(void *, void *, void *);  // __fastcall
    PlaceNameFn *o_placename = nullptr;

    uintptr_t g_pin_vt = 0;  // module_base + 0x2AD6688 (WorldMapPointPinData vtable)

    std::atomic<void *> g_hovered_row{nullptr};
    std::atomic<uint64_t> g_last_hook_ms{0};

    constexpr uintptr_t PIN_VT_RVA = 0x2AD6688;
    constexpr size_t PIN_ROW_OFF = 0x248;  // item -> underlying WorldMapPointParam row

    void *placename_detour(void *panel, void *item, void *map_area)
    {
        void *row = nullptr;
        if (item)
        {
            uintptr_t vt = *reinterpret_cast<uintptr_t *>(item);
            if (vt == g_pin_vt)
                row = *reinterpret_cast<void **>(reinterpret_cast<uint8_t *>(item) + PIN_ROW_OFF);
        }
        g_hovered_row.store(row, std::memory_order_relaxed);
        g_last_hook_ms.store(GetTickCount64(), std::memory_order_relaxed);
        return o_placename(panel, item, map_area);
    }
}  // namespace

void goblin::maphover::setup()
{
    try
    {
        g_pin_vt = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + PIN_VT_RVA;
        auto *fn = modutils::hook<PlaceNameFn>(
            {.aob = "40 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 D9 48 81 EC B0 00 00 00 "
                    "48 C7 45 B7 FE FF FF FF 48 89 9C 24 08 01 00 00 48 8B 05 ?? ?? ?? ?? "
                    "48 33 C4 48 89 45 1F 49 8B F8"},
            placename_detour, o_placename);
        spdlog::info("[maphover] hover hook armed @ 0x{:X}", reinterpret_cast<uintptr_t>(fn));
    }
    catch (const std::exception &e)
    {
        spdlog::error("[maphover] setup failed (marker hover/hide disabled): {}", e.what());
    }
}

void *goblin::maphover::hovered_row()
{
    // Heartbeat gate: the hook only fires while the world map is open. If it hasn't
    // fired very recently the map is closed - report "no hover" so a stale pin from the
    // last session's map can't be acted on.
    if (GetTickCount64() - g_last_hook_ms.load(std::memory_order_relaxed) > 300)
        return nullptr;
    return g_hovered_row.load(std::memory_order_relaxed);
}
