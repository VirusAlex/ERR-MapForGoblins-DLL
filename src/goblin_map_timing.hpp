#pragma once

namespace goblin::map_timing
{
    // World-map open optimization (config::fastMapOpen). Hooks the per-marker
    // relayout + child-list passes at the map's dispatcher call site (AOB-resolved)
    // to skip them on re-open and amortize the first open across frames. No-op when
    // fastMapOpen is off. The deferred-relayout replay + build latch are driven
    // internally from the map dispatcher detour (game UI thread), not externally.
    // See docs/research_worldmap_internals.md.
    void setup();
}
