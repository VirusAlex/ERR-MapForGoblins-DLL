#pragma once
#include <string>
#include <cstdint>

// Lightweight map-icon status registry. Each load step reports its outcome here;
// the overlay Debug tab renders a human-readable report a player can screenshot
// for a bug report (so we can see WHAT failed and WHY without a log).
// All setters are thread-safe: load steps run on the game's loader thread, the
// report is read from the Present/overlay thread.
namespace goblin::diag
{
    enum class OverlayState { OffByConfig, Pending, Active, Failed };

    // Load-time gfx hooks armed (ctor + registrar + lossless + sprite-loader).
    // reason = MinHook/AOB failure text when ok is false.
    void set_hooks(bool ok, const char *reason);

    // Worldmap sprite-171 injection result. base = runtime injected charId base,
    // placed/total = icon frames appended, reason = failure note (empty on ok).
    void set_sprite171(bool ok, uint32_t base, int placed, int total, const char *reason);

    // Icon bitmap (lossless tag) registration count.
    void set_bitmaps(int registered, int total);

    // MapForGoblins logo plaque re-point (cosmetic).
    void set_logo(bool ok, const char *reason);

    // Marker iconId remap (markers pointed at our added frames).
    void set_remap(int count);

    // A live game-heap pointer sample (the located worldmap sprite). Lets a copied
    // report reveal the heap region (e.g. 0x7FF2.. under ME2) for diagnosing
    // pointer-range issues like the 2026-06 looks_heap bound bug.
    void set_heap_sample(uint64_t ptr);

    // DX12 overlay backend state.
    void set_overlay(OverlayState state, const char *reason);

    // Note a self-heal re-base event (a charId collision corrected at runtime).
    void note_selfheal(uint32_t newbase);

    // Build the multi-line status report for the overlay / clipboard.
    std::string report();
}
