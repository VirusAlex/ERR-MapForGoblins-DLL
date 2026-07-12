#pragma once
// Temporary investigation probe (test builds only, not shipped): hooks the world-map
// "update PlaceName for the focused pin" fn so we can log which map pin the game is
// naming under the cursor = the hovered icon. Confirms our injected markers reach the
// native pin path and yields the exact hovered item (iconId + pos).
namespace goblin::maphover_probe
{
    void setup();
}
