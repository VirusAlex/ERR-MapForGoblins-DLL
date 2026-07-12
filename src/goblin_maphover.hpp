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
}
