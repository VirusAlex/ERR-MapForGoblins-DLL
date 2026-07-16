// See goblin_worldmap_probe.hpp. Captures CS::WorldMapViewModel from the engine's
// world->map-space converter and re-invokes that converter to fold any marker.
#include "goblin_worldmap_probe.hpp"

#include "modutils.hpp"

#include <spdlog/spdlog.h>
#include <windows.h>

#include <atomic>

namespace
{
    struct Vec2 { float x, y; };
    struct Vec3 { float x, y, z; };

    // Converter loop (FUN_1408877D0): __fastcall(rcx = WorldMapViewModel, rdx = out map
    // Vec2, r8 = packed id {area<<24|gridX<<16|gridZ<<8}, r9 = area-local Vec3 {x,y,z}).
    // Iterates the VM's per-map converters (VM+0xF8, count VM+0x280), applies the legacy
    // fold, and writes the map-space (u,v) to *out. Returns nonzero on a placed point.
    using ConvertFn = char(void *, Vec2 *, uint32_t *, Vec3 *);
    ConvertFn *o_convert = nullptr;

    std::atomic<void *> g_vm{nullptr};
    std::atomic<uint64_t> g_last_ms{0};

    char convert_detour(void *vm, Vec2 *out, uint32_t *packed, Vec3 *world_local)
    {
        g_vm.store(vm, std::memory_order_relaxed);
        g_last_ms.store(GetTickCount64(), std::memory_order_relaxed);
        return o_convert(vm, out, packed, world_local);
    }

    bool seh_fold(void *vm, uint32_t packed, float px, float pz, float &u, float &v)
    {
        __try
        {
            Vec2 out{0, 0};
            Vec3 wl{px, 0.0f, pz};
            uint32_t p = packed;
            if (!o_convert(vm, &out, &p, &wl)) return false;
            u = out.x;
            v = out.y;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
}

void goblin::worldmap_probe::setup()
{
    try
    {
        auto *fn = modutils::hook<ConvertFn>(
            {.aob = "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57 "
                    "48 83 EC 20 33 DB 4D 8B F9 4D 8B E0 4C 8B EA 48 8B F1 48 39 99 80 02 00 00"},
            convert_detour, o_convert);
        spdlog::info("[wmprobe] converter hook armed @ 0x{:X}", reinterpret_cast<uintptr_t>(fn));
    }
    catch (const std::exception &e)
    {
        spdlog::error("[wmprobe] setup failed (folded-layer rings disabled): {}", e.what());
    }
}

bool goblin::worldmap_probe::project(uint8_t area, uint16_t gx, uint16_t gz, float px, float pz,
                                     float &map_u, float &map_v)
{
    void *vm = g_vm.load(std::memory_order_relaxed);
    if (!vm || !o_convert) return false;  // VM captured on first map open; overlay only calls while open
    const uint32_t packed = (static_cast<uint32_t>(area) << 24) |
                            ((static_cast<uint32_t>(gx) & 0xFF) << 16) |
                            ((static_cast<uint32_t>(gz) & 0xFF) << 8);
    return seh_fold(vm, packed, px, pz, map_u, map_v);
}
