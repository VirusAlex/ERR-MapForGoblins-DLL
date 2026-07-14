#pragma once
// World-map overlay projection. Turns a marker's world X/Z into a screen pixel on the
// open world map, so the overlay can draw a highlight ring on top of the game-rendered
// icon (no baked glow variants, no map reopen). Uses the live WorldMapDialog view
// transform (center/zoom/logical-viewport) published by goblin::maphover, plus a small
// calibration (logical->device px) that is tuned once against the rendered frame.
//
// Coordinate chain (overworld / underground / DLC - one shared affine):
//   world = gridNo*256 + pos      (from WORLD_MAP_POINT_PARAM_ST)
//   mapX  = worldX - CONST_X ; mapZ = CONST_Z - worldZ
//   logical = (map - center)/zoom + viewport/2
//   screen  = origin + logical*scale
// Legacy dungeons use a per-area fold (WorldMapLegacyConvParam) and are handled
// separately once the overworld path is pixel-verified.
#include <cstdint>

namespace goblin::mapproject
{
    struct MapView
    {
        // Live WorldMapArea view state. The engine projection is:
        //   viewCentre = (pan + snapMid) / zoom
        //   screen     = (marker - viewCentre) * zoom * (real/1920or1080) + real/2
        // pan @ +0x378/+0x37C (already in screen-local px), zoom @ +0x380, snapMid =
        // midpoint of the on-screen viewport rect +0x340..+0x34C. This is cursor- AND
        // layer-independent (all fields are live), so it holds for overworld/DLC/underground.
        float panX, panZ;
        float zoom;
        float snapMidX, snapMidZ;
        bool valid;
    };

    // Live calibration. Pan/zoom/scale are already tracked by the two live rects, so the
    // only residual is a MAP-SPACE nudge (a constant offset in the world->map-space
    // affine): applied before projection it tracks zoom correctly, unlike a screen-space
    // offset which drifts as the map scales. scale is a rarely-needed multiplier override.
    struct Calib
    {
        float scale;      // 0 = auto (client/visSize); >0 overrides the device scale
        float dmap_x;     // map-space X nudge (+ moves rings right)
        float dmap_z;     // map-space Z nudge (+ moves rings down, before flip)
        float map_scale;  // map-space spread multiplier about the map centre (1 = none):
                          // corrects a mismatch between our world->map-space unit and the
                          // range the full-map rect spans (fixes corner-vs-corner drift)
        bool flip_y;      // invert the vertical axis if north/south comes out flipped
    };

    // Read the live view transform from the map dialog. false if the map is closed or
    // the dialog is not resolvable this frame.
    bool read_view(MapView &out);

    // Project a marker to a screen pixel. area 60/61 use the plain overworld affine on
    // (grid*256+pos); every other area is folded to map-space by the game's converter
    // (goblin::worldmap_probe). Returns false if a folded area cannot be placed yet
    // (view-model not captured / map not opened this session).
    bool project(uint8_t area, uint16_t gx, uint16_t gz, float px, float pz,
                 const MapView &v, const Calib &c, float client_w, float client_h,
                 float &screen_x, float &screen_y);

    // Live calibration accessors (overlay sliders write these; defaults below).
    Calib &calib();
}
