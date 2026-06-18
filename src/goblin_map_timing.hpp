#pragma once

namespace goblin::map_timing
{
    // World-map open optimization (config::fastMapOpen). Hooks the per-marker
    // relayout + child-list passes at the map's dispatcher call site (AOB-resolved)
    // to skip them on re-open and amortize the first open across frames. No-op when
    // fastMapOpen is off. See docs/research_worldmap_internals.md.
    void setup();

    // Call once per frame from the Present hook: replays deferred first-open relayout
    // (a budget per frame) and latches the "built" state that enables the re-open skip.
    void on_present();
}
