// World-map overlay projection. See goblin_mapproject.hpp.
#include "goblin_mapproject.hpp"
#include "goblin_maphover.hpp"       // map_dialog() -> the live CS::WorldMapArea
#include "goblin_worldmap_probe.hpp" // fold non-overworld areas via the game's converter

#include <windows.h>

namespace
{
    // CS::WorldMapArea (r8 of the hover hook) field offsets (live-confirmed 2026-07-12).
    // pan @ +0x378/+0x37C (screen-local px), zoom @ +0x380, on-screen viewport rect
    // @+0x340..+0x34C (its midpoint = snapMid). The engine projection uses pan directly
    // (do NOT use +0x330, which equals -pan only on the overworld page, not DLC).
    constexpr size_t OFF_PAN = 0x378;       // 2 floats: panX, panZ
    constexpr size_t OFF_ZOOM = 0x380;
    constexpr size_t OFF_VISRECT = 0x340;   // 4 floats: minX, minZ, maxX, maxZ

    // World->map-space affine (overworld/underground/DLC), from the game's own converter
    // (FUN_140876140, area-60 converter: bGX=28,bGZ=64,off=128,scale=1): mapX = worldX-7040,
    // mapZ = -worldZ+16512. Confirmed by static RE + live converter read 2026-07-12.
    constexpr float CONST_X = 7040.0f;
    constexpr float CONST_Z = 16512.0f;

    // The engine renders the map in a fixed virtual 1920x1080 GFx canvas then scales to
    // the backbuffer, so the canvas factor (realW/1920, realH/1080) is mandatory.
    constexpr float CANVAS_W = 1920.0f, CANVAS_H = 1080.0f;

    bool seh_read(const void *addr, void *out, size_t n)
    {
        __try { memcpy(out, addr, n); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    float rf(const uint8_t *base, size_t off)
    {
        float v = 0.0f;
        seh_read(base + off, &v, sizeof v);
        return v;
    }

    // Baked defaults - overwritten live from the overlay sliders during calibration.
    goblin::mapproject::Calib g_calib{0.0f, 0.0f, 0.0f, 1.0f, false};  // scale,dmap_x,dmap_z,map_scale,flip_y
}

bool goblin::mapproject::read_view(MapView &out)
{
    out.valid = false;
    void *area = goblin::maphover::map_dialog();  // returns the WorldMapArea
    if (!area) return false;
    auto *b = reinterpret_cast<const uint8_t *>(area);
    out.panX = rf(b, OFF_PAN + 0);
    out.panZ = rf(b, OFF_PAN + 4);
    out.zoom = rf(b, OFF_ZOOM);
    const float vminX = rf(b, OFF_VISRECT + 0), vminZ = rf(b, OFF_VISRECT + 4);
    const float vmaxX = rf(b, OFF_VISRECT + 8), vmaxZ = rf(b, OFF_VISRECT + 12);
    out.snapMidX = (vminX + vmaxX) * 0.5f;
    out.snapMidZ = (vminZ + vmaxZ) * 0.5f;
    if (!(out.zoom > 0.01f)) return false;
    if (!((vmaxX - vminX) > 1.0f) || !((vmaxZ - vminZ) > 1.0f)) return false;
    out.valid = true;
    return true;
}

bool goblin::mapproject::project(uint8_t area, uint16_t gx, uint16_t gz, float px, float pz,
                                 const MapView &v, const Calib &c, float client_w, float client_h,
                                 float &screen_x, float &screen_y)
{
    if (!v.valid) return false;

    float mapX, mapZ;
    if (area == 60 || area == 61)
    {
        // Overworld + DLC-overworld: plain affine on the reconstructed world coord.
        mapX = (static_cast<float>(gx) * 256.0f + px) - CONST_X;
        mapZ = CONST_Z - (static_cast<float>(gz) * 256.0f + pz);
    }
    else
    {
        // Underground / legacy dungeons / DLC-legacy: fold to map-space via the game's
        // own converter (adapts to the loaded regulation). Skip if not captured yet.
        if (!goblin::worldmap_probe::project(area, gx, gz, px, pz, mapX, mapZ)) return false;
    }
    mapX += c.dmap_x;
    mapZ += c.dmap_z;

    // Engine projection (cursor- and layer-independent):
    //   viewCentre = (pan + snapMid) / zoom
    //   screen     = (marker - viewCentre) * zoom * (real/virtual) + real/2
    // pan is already screen-local px; the canvas factor (real/1920, real/1080) accounts
    // for the fixed 1920x1080 GFx canvas being scaled to the backbuffer.
    const float cU = (v.panX + v.snapMidX) / v.zoom;
    const float cV = (v.panZ + v.snapMidZ) / v.zoom;
    const float kx = client_w / CANVAS_W, ky = client_h / CANVAS_H;
    screen_x = (mapX - cU) * v.zoom * kx + client_w * 0.5f;
    screen_y = (mapZ - cV) * v.zoom * ky + client_h * 0.5f;
    return true;
}

goblin::mapproject::Calib &goblin::mapproject::calib() { return g_calib; }
