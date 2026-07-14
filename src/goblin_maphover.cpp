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

    // vtable addresses, resolved by AOB from each vtable's ctor lea in setup() (were
    // hardcoded RVAs 0x2AD6688 / 0x2B2CB08 - both move on game updates). 0 if the AOB
    // missed (the corresponding gate then never matches -> feature stays disabled).
    uintptr_t g_pin_vt = 0;      // CS::WorldMapPointPinData vtable
    uintptr_t g_maparea_vt = 0;  // CS::WorldMapArea vtable

    std::atomic<void *> g_hovered_row{nullptr};
    std::atomic<void *> g_dialog{nullptr};
    std::atomic<void *> g_map_owner{nullptr};  // buildMarkers `this` (r15); its +0x398 map = current-layer pins
    std::atomic<uint64_t> g_last_hook_ms{0};

    // buildMarkers (0x140A82A80): builds the CURRENT map layer's pins into [owner+0x398]
    // (visibility-filtered, fold already applied). We capture `owner` so the overlay can
    // read that live current-layer pin set. __fastcall(rcx=owner, rdx=layer-context).
    using BuildFn = void *(void *, void *, void *, void *);
    BuildFn *o_build = nullptr;

    std::atomic<void *> g_dialog_data{nullptr};  // buildMarkers param_2 (dialogData)

    void *build_detour(void *owner, void *ctx, void *a, void *b)
    {
        g_map_owner.store(owner, std::memory_order_relaxed);
        // ctx = dialogData. The displayed map id lives at *(int*)(dialogData+8) and its top
        // byte is the area (60=overworld, 12=underground, 61=DLC) - it updates live on layer
        // switch (FUN_1401b9390). We keep the pointer and read it live in map_layer().
        g_dialog_data.store(ctx, std::memory_order_relaxed);
        return o_build(owner, ctx, a, b);
    }

    constexpr size_t PIN_ROW_OFF = 0x248;  // item -> underlying WorldMapPointParam row
    // The hook's r8 (map_area) is CS::WorldMapArea. It carries the live view transform:
    // pan @+0x378/+0x37C, zoom/scale @+0x380, fullRect side @+0x358 (10496). We publish
    // this object directly (no dialog hunt) for the overlay projection. Verified live.

    bool read_vt(void *obj, uintptr_t &out)
    {
        __try { out = *reinterpret_cast<uintptr_t *>(obj); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

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

        // Publish the WorldMapArea (r8) if its vtable matches; it drives the projection.
        void *area = nullptr;
        uintptr_t avt = 0;
        if (g_maparea_vt && map_area && read_vt(map_area, avt) && avt == g_maparea_vt)
            area = map_area;
        else
        {
            static bool logged = false;
            if (!logged)
            {
                spdlog::info("[maphover] map_area.vt=0x{:X} != expected 0x{:X} (maparea AOB {})",
                             avt, g_maparea_vt, g_maparea_vt ? "ok" : "MISSED");
                logged = true;
            }
        }
        g_dialog.store(area, std::memory_order_relaxed);
        g_last_hook_ms.store(GetTickCount64(), std::memory_order_relaxed);
        return o_placename(panel, item, map_area);
    }
}  // namespace

void goblin::maphover::setup()
{
    // Resolve the two vtable gates by AOB (each from its vtable's ctor `lea rax,[rip+vt]`),
    // so a game update that moves the vtable doesn't silently misgate. relative_offsets
    // {{3,7}} yields the lea target = the vtable address. A miss leaves the gate 0.
    try
    {
        g_pin_vt = reinterpret_cast<uintptr_t>(modutils::scan<void>(
            {.aob = "48 8D 05 ?? ?? ?? ?? 48 89 06 48 89 BE 30 02 00 00",
             .relative_offsets = {{3, 7}}}));
    }
    catch (const std::exception &e)
    {
        spdlog::error("[maphover] pin-vtable AOB miss (hover/manual-hide disabled): {}", e.what());
    }
    try
    {
        g_maparea_vt = reinterpret_cast<uintptr_t>(modutils::scan<void>(
            {.aob = "48 8D 05 ?? ?? ?? ?? 48 89 01 48 8D 79 70 48 8B 07 48 8B CF FF 50 08",
             .relative_offsets = {{3, 7}}}));
    }
    catch (const std::exception &e)
    {
        spdlog::error("[maphover] maparea-vtable AOB miss (layer/projection disabled): {}", e.what());
    }
    try
    {
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
    try
    {
        auto *bf = modutils::hook<BuildFn>(
            {.aob = "40 55 53 56 57 41 54 41 56 41 57 48 8B EC 48 83 EC 60 48 C7 45 D0 FE FF "
                    "FF FF 4C 8B F9 8B 42 34"},
            build_detour, o_build);
        spdlog::info("[maphover] buildMarkers hook armed @ 0x{:X}", reinterpret_cast<uintptr_t>(bf));
    }
    catch (const std::exception &e)
    {
        spdlog::error("[maphover] buildMarkers hook failed (layer pin list disabled): {}", e.what());
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

void *goblin::maphover::map_dialog()
{
    if (GetTickCount64() - g_last_hook_ms.load(std::memory_order_relaxed) > 300)
        return nullptr;  // map closed - do not hand back a stale dialog
    return g_dialog.load(std::memory_order_relaxed);
}

int goblin::maphover::map_layer()
{
    if (GetTickCount64() - g_last_hook_ms.load(std::memory_order_relaxed) > 300)
        return -1;  // map closed
    // Currently DISPLAYED map, read from the WorldMapDialog. The dialog encodes both map
    // dimensions in one int at dialogBase+0x30DC: value = world*10 + sublayer, where
    // world 0 = Lands Between, 1 = Shadow Realm (DLC, a full canvas redraw); sublayer
    // 0 = surface, 1 = underground (the UG overlay drawn on top of OW). So 0 = OW(m60),
    // 1 = UG(m12), 10 = DLC surface(m61), 11 = DLC underground. Our HighlightPoint layer
    // is the WorldMapPointParam dispMask bit: 0=OW, 1=UG, 2=DLC (both DLC sublayers -> 2).
    // We reach the field via the MapArea we already publish (hover hook r8, vt rva
    // 0x2B2CB08): the dialog's per-frame Update (FUN_1409c32f0) calls the hover routine
    // with r8 = dialogBase + 0x27D8, so the field sits at MapArea + 0x904 (0x30DC-0x27D8).
    // Live-verified 2026-07-13 (OW=0, UG=1, DLC=10). This is the DISPLAYED tab, unlike the
    // old CS::FieldArea+0xDC read, which was a generation counter (rings stuck on DLC).
    void *area = g_dialog.load(std::memory_order_relaxed);
    if (!area) return -1;
    int v = -1;
    __try { v = *reinterpret_cast<int *>(reinterpret_cast<uint8_t *>(area) + 0x904); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    if (v < 0) return -1;
    if (v >= 10) return 2;      // Shadow Realm / DLC (m61), either sublayer
    return v;                   // 0 = overworld (m60), 1 = underground (m12)
}
