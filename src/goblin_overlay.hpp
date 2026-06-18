#pragma once

// In-game config overlay (Dear ImGui on a DX12 swapchain hook). Lets the player
// open a settings panel from inside the running game and flip the show_* / other
// options, then Save them back to MapForGoblins.ini. Prototype: changes apply on
// next launch (live in-session re-apply is a later phase). Open/close: F8.
namespace goblin::overlay
{
    // Capture the DXGI swapchain + D3D12 command-queue vtables (via a throwaway
    // device/swapchain) and QUEUE MinHook hooks on Present / ResizeBuffers /
    // ExecuteCommandLists. The hooks are applied by modutils::enable_hooks().
    // Safe no-op (logs) if D3D12 init fails. Call before enable_hooks().
    void setup();
}
