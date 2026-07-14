#pragma once
// World-map hover detection. Hooks the game's per-frame "name the focused pin" routine
// (FUN_14087a8e0) so we always know which map pin the cursor is over - exactly the icon
// the game highlights + shows a popup for. Used by manual marker-hide (hover + hotkey)
// and (later) the passive hover-info overlay. The hovered pin's underlying
// WorldMapPointParam row pointer is published; goblin::inject matches it to one of our
// injected rows. Native + exact (no compute-nearest approximation).
namespace goblin::maphover
{
    // Arm the hook (call once at DLL init, before enable_hooks()).
    void setup();

    // The WORLD_MAP_POINT_PARAM_ST* of the pin currently under the cursor, or nullptr
    // when nothing is focused OR the world map is not currently open (heartbeat-gated:
    // the hook only fires while the map is up). Thread-safe.
    void *hovered_row();

    // The live CS::WorldMapArea object (r8 of the hook, vtable RVA 0x2B2CB08), or nullptr
    // when the map is not open. Carries the view transform (pan @+0x378/+0x37C, zoom
    // @+0x380, full-map side @+0x358) that drives the overlay world->screen projection.
    // Thread-safe; nullptr when stale (map closed). (Named map_dialog for API stability.)
    void *map_dialog();

    // The currently displayed map layer, decoded live from the WorldMapDialog field at
    // MapArea+0x904 (value = world*10 + sublayer). Matches WorldMapPointParam dispMask
    // bits: 0 = overworld (M00), 1 = underground (M01), 2 = DLC (M02, both sublayers).
    // -1 when the map is closed / not yet known.
    int map_layer();
}
