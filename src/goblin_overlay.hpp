#pragma once

#include <cstdint>

// In-game config overlay (Dear ImGui in a SEPARATE transparent top-most window
// with its own D3D11 + DirectComposition device, rendered on its own thread). It
// does NOT touch the game's swapchain, so it is compatible with tools that wrap
// the game's swapchain (Special K, NVIDIA Smooth Motion / frame-gen, ReShade).
// Lets the player open a settings panel from inside the running game and flip the
// show_* / other options; changes auto-save to MapForGoblins.ini on close and
// auto-reload on open. Open/close: the configured toggle key (default F10) or ESC.
namespace goblin::overlay
{
    // Spawn the dedicated overlay thread (creates the window + D3D11 + DComp +
    // ImGui and runs the render loop). Returns immediately. Safe no-op (logs) if
    // enable_overlay is false or window/D3D creation fails. Call from setup_mod.
    void setup();

    // True while Win32 virtual-key `vk` is currently held. Reads GetAsyncKeyState
    // directly (foreground-only). Used by the marker-dump + icon master-toggle
    // hotkey loops in goblin_markers / goblin_inject.
    bool key_down(int vk);

    // True while ALL buttons in `mask` (an XINPUT_GAMEPAD_* bitmask) are held on the
    // active pad. Reads the overlay's polled pad state (updated each render frame), so it
    // needs the overlay thread running (enable_overlay). Used by the manual marker-hide loop.
    bool gamepad_mask_down(uint16_t mask);
}
