// In-game config overlay: Dear ImGui drawn into the game's own DX12 swapchain.
//
// Hook model (the standard UniversalHookX / EROverlay pattern for D3D12):
//   - IDXGISwapChain::Present (vtable idx 8)   -> drive rendering + lazy init
//   - ID3D12CommandQueue::ExecuteCommandLists  -> capture the game's graphics
//     queue (the DX12 queue is NOT reachable from a Present-only hook)
//   - IDXGISwapChain::ResizeBuffers (idx 13)   -> tear down our RTVs so the
//     resize succeeds; we re-create them on the next Present
// vtable pointers are grabbed once from a throwaway device+swapchain at setup,
// so there is no game-specific RVA/AOB to maintain.
//
// PROTOTYPE SCOPE: renders the panel, edits the live config bools, and Saves to
// the ini (effective next launch). Mouse + keyboard input via a WndProc detour.
// Gamepad nav flag is set but the game's XInput is NOT yet gated, and per-toggle
// live re-apply is a later phase (the expanded param table is filtered at init).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <Xinput.h>

#include "goblin_overlay.hpp"
#include "goblin_config.hpp"
#include "goblin_config_schema.hpp"
#include "goblin_overlay_icons.hpp"
#include "goblin_inject.hpp"
#include "goblin_markers.hpp"
#include "goblin_map_timing.hpp"
#include "modutils.hpp"

#include <spdlog/spdlog.h>
#include <imgui.h>
#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "version.h" // PROJECT_VERSION (generated)

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

// From imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                                             WPARAM wParam, LPARAM lParam);

namespace
{
using Present_t = HRESULT(WINAPI *)(IDXGISwapChain3 *, UINT, UINT);
using ResizeBuffers_t = HRESULT(WINAPI *)(IDXGISwapChain3 *, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using ExecuteCommandLists_t = void(WINAPI *)(ID3D12CommandQueue *, UINT, ID3D12CommandList *const *);
using GetRawInputData_t = UINT(WINAPI *)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
using XInputGetState_t = DWORD(WINAPI *)(DWORD, XINPUT_STATE *);

Present_t oPresent = nullptr;
ResizeBuffers_t oResizeBuffers = nullptr;
ExecuteCommandLists_t oExecuteCommandLists = nullptr;
GetRawInputData_t oGetRawInputData = nullptr;
XInputGetState_t oXInputGetState = nullptr;

// Captured controller-0 state for ImGui gamepad nav (the game's read is zeroed
// while the menu is open; we feed ImGui from this real snapshot instead).
XINPUT_GAMEPAD g_pad{};
bool g_pad_ok = false;

// Raw-input capture (ER reads kbd/mouse via raw input, not legacy WM_* msgs).
// The hook runs on the game's message thread; it only writes these atomics, and
// render() (render thread) drains them into ImGui so all ImGui IO stays on one
// thread.
std::atomic<int> g_raw_dx{0};
std::atomic<int> g_raw_dy{0};
std::atomic<int> g_raw_wheel{0};
std::atomic<uint32_t> g_raw_btn{0}; // bit0=L bit1=R bit2=M (current state)
float g_mouse_x = 0.0f, g_mouse_y = 0.0f;
bool g_need_center = true;
bool g_os_cursor = false; // OS hardware cursor visible (e.g. in-game map open)

// Raw keyboard -> ImGui (needed for InputText Ctrl+A / Ctrl+C in the dump box).
// Captured on the message thread, drained in render thread (single-thread IO).
struct KeyEv { ImGuiKey key; bool down; };
std::mutex g_key_mtx;
std::vector<KeyEv> g_key_events;

struct FrameContext
{
    ID3D12CommandAllocator *allocator = nullptr;
    ID3D12Resource *render_target = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle{};
};

ID3D12Device *g_device = nullptr;
ID3D12DescriptorHeap *g_srv_heap = nullptr; // shader-visible, holds imgui font SRV
ID3D12DescriptorHeap *g_rtv_heap = nullptr;
ID3D12GraphicsCommandList *g_command_list = nullptr;
ID3D12CommandQueue *g_command_queue = nullptr; // captured from ExecuteCommandLists
FrameContext *g_frames = nullptr;
UINT g_buffer_count = 0;

ID3D12Resource *g_atlas_tex = nullptr;        // category-icon atlas texture
D3D12_GPU_DESCRIPTOR_HANDLE g_atlas_gpu{};    // its SRV (g_srv_heap index 1)
ID3D12Resource *g_logo_tex = nullptr;         // mod logo texture
D3D12_GPU_DESCRIPTOR_HANDLE g_logo_gpu{};     // its SRV (g_srv_heap index 2)
bool g_atlas_ready = false;

// GPU sync: a fence we signal+wait after each submit so we never reuse the
// command list/allocator (or free RTVs on resize) while the GPU is still using
// them — the AMD/Steam-Deck "device removed" failure mode if omitted.
ID3D12Fence *g_fence = nullptr;
UINT64 g_fence_val = 0;
HANDLE g_fence_event = nullptr;

HWND g_hwnd = nullptr;
WNDPROC g_orig_wndproc = nullptr;

std::atomic<bool> g_menu_open{false};
// Buttons held at the moment the overlay closed (e.g. B used to close it). We
// keep suppressing them in the game's XInput read until released, so the closing
// press does not fall through into gameplay.
std::atomic<WORD> g_pad_swallow{0};
bool g_context_inited = false; // ImGui context + win32 backend + wndproc detour
bool g_dx12_inited = false;    // DX12 device objects + ImGui dx12 backend
std::vector<char> g_dump_buf;       // Tools tab: last marker-dump text (copyable)

// In-overlay hotkey rebind. mode: 0=idle, 1=capturing a keyboard key, 2=capturing
// a gamepad combo. While != 0, hkPresent ignores the open/close + ESC/B inputs so
// binding those keys doesn't act on the menu.
std::atomic<int> g_rebind_mode{0};
void *g_rebind_target = nullptr;          // config var being rebound (render thread)
std::atomic<uint32_t> g_captured_vk{0};   // first VK seen by the raw-input hook during rebind
std::atomic<bool> g_captured_up{false};   // that key was released -> safe to commit (no auto-trigger)
uint16_t g_rebind_pad_accum = 0;          // gamepad buttons accumulated this rebind

// Last input device, for control hints: 0 = keyboard/mouse, 1 = gamepad.
std::atomic<int> g_last_input{0};

// ── ER-flavored dark/gold theme ──
void apply_er_style()
{
    ImGuiStyle &s = ImGui::GetStyle();
    s.WindowRounding = 2.0f;
    s.FrameRounding = 2.0f;
    s.WindowBorderSize = 1.0f;
    s.WindowPadding = ImVec2(12, 12);
    s.ItemSpacing = ImVec2(8, 6);
    ImVec4 *c = s.Colors;
    const ImVec4 gold(0.80f, 0.68f, 0.40f, 1.0f);
    c[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.05f, 0.04f, 0.96f);
    c[ImGuiCol_TitleBg] = ImVec4(0.12f, 0.10f, 0.05f, 1.0f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.22f, 0.17f, 0.08f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(0.30f, 0.24f, 0.12f, 1.0f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.45f, 0.36f, 0.18f, 1.0f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.55f, 0.44f, 0.22f, 1.0f);
    c[ImGuiCol_CheckMark] = gold;
    c[ImGuiCol_SliderGrab] = gold;
    c[ImGuiCol_Button] = ImVec4(0.25f, 0.20f, 0.10f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.40f, 0.32f, 0.16f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.52f, 0.42f, 0.20f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.12f, 0.07f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.25f, 0.20f, 0.11f, 1.0f);
    c[ImGuiCol_Text] = ImVec4(0.92f, 0.88f, 0.78f, 1.0f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.50f, 0.40f, 1.0f);
    c[ImGuiCol_Border] = ImVec4(0.50f, 0.42f, 0.25f, 0.6f);
    c[ImGuiCol_Separator] = ImVec4(0.50f, 0.42f, 0.25f, 0.5f);
}

const goblin::overlay_icons::IconCell *find_icon_cell(const char *key); // defined below

// Category icon left of a toggle, with a fixed gutter so iconless rows align.
// All icons share one display size; two-layer "glyph on a plate" icons are zoomed
// (and edge-cropped) inside their atlas cell by the generator so their glyph reads
// at a size comparable to single-glyph icons.
void draw_row_icon(const char *key)
{
    constexpr float ICON_SZ = 28.0f;
    using namespace goblin::overlay_icons;
    const IconCell *ic = g_atlas_ready ? find_icon_cell(key) : nullptr;
    if (ic)
    {
        const ImVec2 uv0((ic->col * CELL) / static_cast<float>(ATLAS_W),
                         (ic->row * CELL) / static_cast<float>(ATLAS_H));
        const ImVec2 uv1(((ic->col + 1) * CELL) / static_cast<float>(ATLAS_W),
                         ((ic->row + 1) * CELL) / static_cast<float>(ATLAS_H));
        ImGui::Image(reinterpret_cast<ImTextureID>(static_cast<uintptr_t>(g_atlas_gpu.ptr)),
                     ImVec2(ICON_SZ, ICON_SZ), uv0, uv1);
    }
    else
    {
        ImGui::Dummy(ImVec2(ICON_SZ, ICON_SZ));
    }
    ImGui::SameLine();
}

// Human-readable current value of a VkKey hotkey (uint32 VK code).
std::string fmt_vk(uint32_t vk)
{
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
        return std::string(1, static_cast<char>(vk));
    switch (vk)
    {
    case VK_ESCAPE: return "Esc";
    case VK_SPACE:  return "Space";
    case VK_TAB:    return "Tab";
    case VK_RETURN: return "Enter";
    case VK_OEM_3:  return "`";
    case VK_INSERT: return "Insert";
    case VK_DELETE: return "Delete";
    case VK_HOME:   return "Home";
    case VK_END:    return "End";
    case VK_PRIOR:  return "PageUp";
    case VK_NEXT:   return "PageDown";
    case 0:         return "(none)";
    default:        { char b[8]; std::snprintf(b, sizeof b, "0x%02X", vk); return b; }
    }
}

// Human-readable current value of a GamepadMask hotkey (uint16 XInput button mask).
std::string fmt_gamepad(uint16_t m)
{
    static const struct { WORD bit; const char *name; } BTN[] = {
        {XINPUT_GAMEPAD_Y, "Y"}, {XINPUT_GAMEPAD_X, "X"},
        {XINPUT_GAMEPAD_A, "A"}, {XINPUT_GAMEPAD_B, "B"},
        {XINPUT_GAMEPAD_LEFT_SHOULDER, "LB"}, {XINPUT_GAMEPAD_RIGHT_SHOULDER, "RB"},
        {XINPUT_GAMEPAD_LEFT_THUMB, "L3"}, {XINPUT_GAMEPAD_RIGHT_THUMB, "R3"},
        {XINPUT_GAMEPAD_BACK, "Back"}, {XINPUT_GAMEPAD_START, "Start"},
        {XINPUT_GAMEPAD_DPAD_UP, "Up"}, {XINPUT_GAMEPAD_DPAD_DOWN, "Down"},
        {XINPUT_GAMEPAD_DPAD_LEFT, "Left"}, {XINPUT_GAMEPAD_DPAD_RIGHT, "Right"},
    };
    std::string s;
    for (const auto &b : BTN)
        if (m & b.bit) { if (!s.empty()) s += '+'; s += b.name; }
    return s.empty() ? "(none)" : s;
}

// Apply an in-progress hotkey rebind (render thread). Esc cancels. A keyboard key
// commits immediately; a gamepad combo commits once all buttons are released.
void process_rebind()
{
    const int mode = g_rebind_mode.load();
    if (mode == 0 || !g_rebind_target)
        return;
    if (g_captured_vk.load() == VK_ESCAPE) // cancel immediately
    {
        g_rebind_mode.store(0);
        g_captured_vk.store(0);
        g_captured_up.store(false);
        return;
    }
    if (mode == 1) // keyboard key — commit on RELEASE so the bind press doesn't
    {              // also fire the action it was just bound to
        const uint32_t vk = g_captured_vk.load();
        if (vk != 0 && g_captured_up.load())
        {
            *static_cast<uint32_t *>(g_rebind_target) = vk;
            g_rebind_mode.store(0);
            g_captured_vk.store(0);
            g_captured_up.store(false);
        }
    }
    else // mode 2: gamepad combo — accumulate held buttons, commit on release
    {
        const uint16_t held = g_pad_ok ? g_pad.wButtons : 0;
        if (held)
            g_rebind_pad_accum |= held;
        else if (g_rebind_pad_accum)
        {
            *static_cast<uint16_t *>(g_rebind_target) = g_rebind_pad_accum;
            g_rebind_mode.store(0);
        }
    }
}

// ── Settings tab: live-applied category toggles ──
void draw_settings_tab()
{
    // Blink yellow <-> red every 0.5s so it's hard to miss.
    const bool blink_red = (static_cast<int>(ImGui::GetTime() * 2.0) & 1) != 0;
    ImGui::PushStyleColor(ImGuiCol_Text, blink_red ? ImVec4(1.0f, 0.27f, 0.22f, 1.0f)
                                                   : ImVec4(1.0f, 0.84f, 0.38f, 1.0f));
    ImGui::TextWrapped("You MUST re-open the world map to see changes!");
    ImGui::PopStyleColor();
    ImGui::Separator();

    bool changed = false;
    // NavFlattened: keyboard/gamepad nav flows through this scroll region as if it
    // were part of the window, so you don't have to "enter" it and can't get stuck.
    // NavFlattened must be a ChildFlag in ImGui 1.90.9 (the old WindowFlag is
    // obsoleted/no-op). Without it, gamepad directional nav can't cross from the
    // window into this child (you'd have to "activate" the section and couldn't
    // leave); keyboard hid the bug because Tab traverses regardless.
    ImGui::BeginChild("##scroll", ImVec2(0, 0), ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_None);
    for (const auto &sec : goblin::ini_schema())
    {
        if (goblin::profile_is_vanilla() && sec.err_only)
            continue;
        ImGui::PushID(sec.name);
        // Group toggles: flip every Bool in this section at once.
        auto set_section = [&](bool v) {
            for (const auto &e : sec.entries)
                if (e.type == goblin::IniType::Bool &&
                    !(goblin::profile_is_vanilla() && e.err_only))
                    *static_cast<bool *>(e.target) = v;
            changed = true;
        };
        const bool open = ImGui::CollapsingHeader(sec.name, ImGuiTreeNodeFlags_DefaultOpen);
        // "all on"/"all off" right-aligned on the header row (work even when collapsed).
        const ImGuiStyle &st = ImGui::GetStyle();
        const float w_on  = ImGui::CalcTextSize("all on").x + st.FramePadding.x * 2;
        const float w_off = ImGui::CalcTextSize("all off").x + st.FramePadding.x * 2;
        ImGui::SameLine(ImGui::GetContentRegionMax().x - w_on - w_off - st.ItemSpacing.x);
        if (ImGui::SmallButton("all on")) set_section(true);
        ImGui::SameLine();
        if (ImGui::SmallButton("all off")) set_section(false);
        ImGui::PopID();
        if (!open)
            continue;
        if (std::strcmp(sec.name, "Compatibility") == 0)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.82f, 1.0f, 1.0f));
            ImGui::TextWrapped("Using a Randomizer? Enable live_loot_flags + "
                               "live_loot_labels + live_loot_icons so markers match "
                               "the shuffled item placements.");
            ImGui::PopStyleColor();
        }
        for (const auto &e : sec.entries)
        {
            if (goblin::profile_is_vanilla() && e.err_only)
                continue;
            ImGui::PushID(e.key);
            if (std::strcmp(e.key, "fast_map_open") == 0)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.84f, 0.0f, 1.0f)); // yellow
                ImGui::TextWrapped("BETA: makes the map open faster. If the map glitches "
                                   "or crashes, turn this off.");
                ImGui::PopStyleColor();
            }
            draw_row_icon(e.key);
            bool hovered = false; // mouse hover OR keyboard/gamepad nav focus on this row
            if (e.type == goblin::IniType::Bool)
            {
                // Lock the overlay/hotkey master switches: unchecking enable_overlay would
                // close the overlay with no way to reopen it, and enable_toggle_hotkey only
                // matters when the overlay is OFF (when this menu isn't visible). We grey
                // them but keep them NAVIGABLE (not BeginDisabled) so their tooltip is
                // reachable by keyboard/gamepad; the toggle is ignored.
                const bool locked = std::strcmp(e.key, "enable_overlay") == 0 ||
                                    std::strcmp(e.key, "enable_toggle_hotkey") == 0;
                bool v = *static_cast<bool *>(e.target);
                if (locked)
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                const bool clicked = ImGui::Checkbox(e.key, &v);
                hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip);
                if (locked) ImGui::PopStyleColor();
                if (clicked && !locked) { *static_cast<bool *>(e.target) = v; changed = true; }
                if (locked)
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(.ini only)");
                    hovered = hovered || ImGui::IsItemHovered();
                }
            }
            else if (e.type == goblin::IniType::U8)
            {
                int v = *static_cast<uint8_t *>(e.target);
                if (ImGui::SliderInt(e.key, &v, 0, 30))
                    *static_cast<uint8_t *>(e.target) =
                        static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
                hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip);
            }
            else // VkKey / GamepadMask: show value + in-place rebind
            {
                const bool is_key = e.type == goblin::IniType::VkKey;
                const std::string val =
                    is_key ? fmt_vk(*static_cast<uint32_t *>(e.target))
                           : fmt_gamepad(*static_cast<uint16_t *>(e.target));
                const bool capturing = g_rebind_mode.load() != 0 && g_rebind_target == e.target;
                char label[160];
                std::snprintf(label, sizeof label, "%s = %s", e.key, val.c_str());
                const float btn_w = 80.0f;
                ImGui::Selectable(label, false, 0,
                                  ImVec2(ImGui::GetContentRegionAvail().x - btn_w, 0));
                hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip);
                ImGui::SameLine();
                if (capturing)
                    ImGui::TextColored(ImVec4(1.0f, 0.84f, 0.38f, 1.0f),
                                       is_key ? "press a key" : "press combo, release");
                else if (ImGui::SmallButton("rebind"))
                {
                    g_rebind_target = e.target;
                    g_rebind_pad_accum = 0;
                    g_captured_vk.store(0);
                    g_captured_up.store(false);
                    g_rebind_mode.store(is_key ? 1 : 2);
                }
            }
            if (e.comment && hovered)
                ImGui::SetTooltip("%s", e.comment);
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    if (changed)
        goblin::reapply_live_settings(); // live-apply the show_* change
}

// ── Tools tab: marker dump into a copyable text box ──
void draw_tools_tab()
{
    ImGui::TextWrapped("Dumps the map beacons (your 1-5 placed markers) and stamps "
                       "to text. Works whether the map is open or closed. Press Copy "
                       "to put it on the clipboard.");
    if (ImGui::Button("Dump markers now"))
    {
        std::string s = goblin::markers::dump_to_string();
        g_dump_buf.assign(s.begin(), s.end());
        g_dump_buf.push_back('\0');
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy") && !g_dump_buf.empty())
        ImGui::SetClipboardText(g_dump_buf.data()); // copies the whole dump
    ImGui::SameLine();
    ImGui::TextDisabled("%d chars", g_dump_buf.empty() ? 0 : static_cast<int>(g_dump_buf.size() - 1));
    ImGui::Separator();
    if (!g_dump_buf.empty())
    {
        // Read-only, non-selectable view (Copy handles clipboard); scrolls both ways.
        ImGui::BeginChild("##dump", ImVec2(-1, -1), true, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(g_dump_buf.data());
        ImGui::EndChild();
    }
    else
        ImGui::TextDisabled("(no dump yet - press the button above)");
}

// ── About tab: version + links ──
void draw_about_tab()
{
    // Large logo on the left, title/version/description to its right.
    if (g_atlas_ready && goblin::overlay_icons::LOGO_W > 0 && g_logo_gpu.ptr)
    {
        const float lh = 120.0f;
        const float lw = lh * goblin::overlay_icons::LOGO_W /
                         static_cast<float>(goblin::overlay_icons::LOGO_H);
        ImGui::Image(reinterpret_cast<ImTextureID>(static_cast<uintptr_t>(g_logo_gpu.ptr)),
                     ImVec2(lw, lh));
        ImGui::SameLine();
    }
    ImGui::BeginGroup();
    ImGui::TextColored(ImVec4(0.80f, 0.68f, 0.40f, 1.0f), "Map for Goblins - DLL");
    ImGui::Text("Version %s", PROJECT_VERSION);
    ImGui::Spacing();
    ImGui::TextWrapped("Thousands of loot and world icons on the in-game map.");
    ImGui::EndGroup();
    ImGui::Spacing();
    ImGui::Separator();
    struct Link { const char *label; const char *url; const char *btn; };
    static const Link links[] = {
        {"Nexus Mods",        "https://www.nexusmods.com/eldenring/mods/10062",     "copy##nx"},
        {"GitHub (source)",   "https://github.com/VirusAlex/ERR-MapForGoblins-DLL", "copy##gh"},
        {"Discord (support)", "https://discord.gg/JvTMwPCygB",                      "copy##dc"},
    };
    for (const auto &l : links)
    {
        ImGui::TextDisabled("%s:", l.label);
        ImGui::TextUnformatted(l.url);
        ImGui::SameLine();
        if (ImGui::SmallButton(l.btn))
            ImGui::SetClipboardText(l.url);
    }
}

// ── Bottom-of-window control hints, auto-switched by the last input device ──
void draw_control_hints()
{
    ImGui::Separator();
    if (g_last_input.load() == 1)
        ImGui::TextDisabled("D-Pad/Stick: move   A: toggle   B: close   LB/RB: switch tab");
    else
        ImGui::TextDisabled("Mouse + Arrows: move   Click/Space: toggle   Esc: close");
}

// ── The overlay window: master switch + tabs ──
void draw_settings_window()
{
    ImGui::SetNextWindowSize(ImVec2(560, 680), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Map for Goblins - Settings", nullptr))
    {
        ImGui::End();
        return;
    }

    // Master on/off for ALL icons. Settings auto-save on close and auto-reload on
    // open, so no Save/Reload buttons are needed.
    bool show_icons = !goblin::icons_hidden();
    if (ImGui::Checkbox("Show map icons (master)", &show_icons))
        goblin::set_icons_hidden(!show_icons);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("Turns ALL map icons on/off at once.\nWhen the overlay is "
                          "disabled, toggle_key / the gamepad combo do this from "
                          "outside the menu.");
    // Right-aligned Close button.
    constexpr float close_w = 90.0f;
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - close_w);
    if (ImGui::Button("Close", ImVec2(close_w, 0)))
        g_menu_open.store(false);
    ImGui::Separator();

    // Gamepad LB/RB cycle the tabs (face/d-pad nav still works too).
    static int forced_tab = -1, cur_tab = 0;
    static bool prev_lb = false, prev_rb = false;
    if (g_pad_ok)
    {
        const bool lb = (g_pad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
        const bool rb = (g_pad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
        if (rb && !prev_rb) { cur_tab = (cur_tab + 1) % 3; forced_tab = cur_tab; }
        if (lb && !prev_lb) { cur_tab = (cur_tab + 2) % 3; forced_tab = cur_tab; }
        prev_lb = lb; prev_rb = rb;
    }
    auto tab_flag = [&](int i) {
        return forced_tab == i ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
    };

    // Body fills all but a footer reserved for the control hints.
    const float footer_h = ImGui::GetTextLineHeightWithSpacing() + 8.0f;
    // NavFlattened here too: this outer child wraps the tab bar + content, so
    // without it gamepad nav can't cross from the master checkbox into the tabs/
    // list (you'd have to "activate" this child and couldn't leave). The inner
    // ##scroll child also sets it. (1.90.9: NavFlattened is a ChildFlag.)
    ImGui::BeginChild("##body", ImVec2(0, -footer_h), ImGuiChildFlags_NavFlattened);
    if (ImGui::BeginTabBar("##tabs"))
    {
        if (ImGui::BeginTabItem("Settings", nullptr, tab_flag(0))) { draw_settings_tab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Tools",    nullptr, tab_flag(1))) { draw_tools_tab();    ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("About",    nullptr, tab_flag(2))) { draw_about_tab();    ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    forced_tab = -1;
    draw_control_hints();
    ImGui::End();
}

// ── Window proc detour: while the menu is open, swallow any LEGACY input msgs
// (belt-and-suspenders; ER's real input path is raw input, blocked separately
// in hkGetRawInputData). ImGui input itself is driven from raw input, so we do
// NOT forward to ImGui_ImplWin32_WndProcHandler here (avoids double-feed).
LRESULT CALLBACK hkWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (g_menu_open.load())
    {
        switch (msg)
        {
        case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR:
        case WM_SYSKEYDOWN: case WM_SYSKEYUP:
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        case WM_XBUTTONDOWN: case WM_XBUTTONUP:
        case WM_INPUT:
            return 0;
        case WM_SETCURSOR:
            return TRUE;
        default:
            break;
        }
    }
    return CallWindowProcW(g_orig_wndproc, hWnd, msg, wParam, lParam);
}

// Map a Win32 virtual-key to an ImGuiKey (the subset the panel needs: text
// nav + Ctrl/Shift for select-all/copy in the dump box). ImGuiKey_None = ignore.
ImGuiKey vk_to_imgui(USHORT vk)
{
    if (vk >= 'A' && vk <= 'Z') return static_cast<ImGuiKey>(ImGuiKey_A + (vk - 'A'));
    if (vk >= '0' && vk <= '9') return static_cast<ImGuiKey>(ImGuiKey_0 + (vk - '0'));
    switch (vk)
    {
    case VK_CONTROL: case VK_LCONTROL: return ImGuiKey_LeftCtrl;
    case VK_RCONTROL: return ImGuiKey_RightCtrl;
    case VK_SHIFT: case VK_LSHIFT: return ImGuiKey_LeftShift;
    case VK_RSHIFT: return ImGuiKey_RightShift;
    case VK_MENU: case VK_LMENU: return ImGuiKey_LeftAlt;
    case VK_RMENU: return ImGuiKey_RightAlt;
    case VK_LEFT: return ImGuiKey_LeftArrow;
    case VK_RIGHT: return ImGuiKey_RightArrow;
    case VK_UP: return ImGuiKey_UpArrow;
    case VK_DOWN: return ImGuiKey_DownArrow;
    case VK_HOME: return ImGuiKey_Home;
    case VK_END: return ImGuiKey_End;
    case VK_PRIOR: return ImGuiKey_PageUp;
    case VK_NEXT: return ImGuiKey_PageDown;
    case VK_DELETE: return ImGuiKey_Delete;
    case VK_BACK: return ImGuiKey_Backspace;
    case VK_RETURN: return ImGuiKey_Enter;
    case VK_TAB: return ImGuiKey_Tab;
    case VK_SPACE: return ImGuiKey_Space;
    case VK_ESCAPE: return ImGuiKey_Escape;
    default: return ImGuiKey_None;
    }
}

// True only while the game window is the foreground window. Used to suspend our
// input capture + software cursor when the user alt-tabs away (otherwise a 2nd,
// offset cursor appears and both click at once).
static bool game_focused()
{
    return g_hwnd != nullptr && GetForegroundWindow() == g_hwnd;
}

// Raw-input detour: capture mouse + keyboard for ImGui + blank the data the GAME
// reads while the menu is open (so the character doesn't move / camera doesn't turn).
UINT WINAPI hkGetRawInputData(HRAWINPUT hri, UINT cmd, LPVOID data, PUINT size, UINT hdr)
{
    UINT res = oGetRawInputData(hri, cmd, data, size, hdr);
    if (!g_menu_open.load() || cmd != RID_INPUT || data == nullptr || !game_focused())
        return res;

    auto *ri = reinterpret_cast<RAWINPUT *>(data);
    if (ri->header.dwType == RIM_TYPEMOUSE)
    {
        g_last_input.store(0, std::memory_order_relaxed); // keyboard/mouse active
        const RAWMOUSE &m = ri->data.mouse;
        if (!(m.usFlags & MOUSE_MOVE_ABSOLUTE))
        {
            g_raw_dx.fetch_add(m.lLastX, std::memory_order_relaxed);
            g_raw_dy.fetch_add(m.lLastY, std::memory_order_relaxed);
        }
        const USHORT bf = m.usButtonFlags;
        if (bf & RI_MOUSE_LEFT_BUTTON_DOWN)   g_raw_btn.fetch_or(1u);
        if (bf & RI_MOUSE_LEFT_BUTTON_UP)     g_raw_btn.fetch_and(~1u);
        if (bf & RI_MOUSE_RIGHT_BUTTON_DOWN)  g_raw_btn.fetch_or(2u);
        if (bf & RI_MOUSE_RIGHT_BUTTON_UP)    g_raw_btn.fetch_and(~2u);
        if (bf & RI_MOUSE_MIDDLE_BUTTON_DOWN) g_raw_btn.fetch_or(4u);
        if (bf & RI_MOUSE_MIDDLE_BUTTON_UP)   g_raw_btn.fetch_and(~4u);
        if (bf & RI_MOUSE_WHEEL)
            g_raw_wheel.fetch_add(static_cast<short>(m.usButtonData), std::memory_order_relaxed);
        ri->data.mouse.lLastX = 0;
        ri->data.mouse.lLastY = 0;
        ri->data.mouse.usButtonFlags = 0;
        ri->data.mouse.usButtonData = 0;
    }
    else if (ri->header.dwType == RIM_TYPEKEYBOARD)
    {
        const RAWKEYBOARD &kb = ri->data.keyboard;
        g_last_input.store(0, std::memory_order_relaxed); // keyboard/mouse active
        const bool down = (kb.Flags & RI_KEY_BREAK) == 0;
        if (g_rebind_mode.load() != 0)
        {
            // Capture the FIRST key pressed (ignore Space/Enter — they activate the
            // rebind button via nav), then commit when it's RELEASED (process_rebind),
            // so the binding press doesn't also trigger the freshly-bound action.
            if (down && kb.VKey != 0 && g_captured_vk.load() == 0 &&
                kb.VKey != VK_SPACE && kb.VKey != VK_RETURN)
                g_captured_vk.store(kb.VKey, std::memory_order_relaxed);
            if (!down && kb.VKey != 0 && kb.VKey == g_captured_vk.load())
                g_captured_up.store(true, std::memory_order_relaxed);
        }
        ImGuiKey k = vk_to_imgui(kb.VKey);
        if (k != ImGuiKey_None)
        {
            std::lock_guard<std::mutex> lk(g_key_mtx);
            g_key_events.push_back({k, down});
        }
        // blank for the game (block movement/menus while our menu is open)
        ri->data.keyboard.VKey = 0;
        ri->data.keyboard.MakeCode = 0;
        ri->data.keyboard.Flags = 0;
        ri->data.keyboard.Message = WM_NULL;
    }
    return res;
}

// XInput gate: always snapshot controller-0 (so hkPresent can read the open/close
// combo even while the menu is CLOSED); while the menu is open, also ZERO the state
// the game (and ImGui's own backend) read so the player/camera don't move. The
// captured snapshot is fed to ImGui nav in feed_input().
DWORD WINAPI hkXInputGetState(DWORD idx, XINPUT_STATE *st)
{
    DWORD r = oXInputGetState(idx, st);
    if (idx == 0)
    {
        if (r == ERROR_SUCCESS && st) { g_pad = st->Gamepad; g_pad_ok = true; }
        else g_pad_ok = false;
    }
    if (st)
    {
        if (g_menu_open.load())
            ZeroMemory(&st->Gamepad, sizeof(st->Gamepad));
        else if (idx == 0 && g_pad_swallow.load())
        {
            // Suppress buttons held at close until they are released (no fall-through).
            WORD sw = g_pad_swallow.load() & st->Gamepad.wButtons;
            g_pad_swallow.store(sw);
            st->Gamepad.wButtons &= ~sw;
        }
    }
    return r;
}

// Drain the raw-input atomics into ImGui (render thread, just before NewFrame).
void feed_input()
{
    ImGuiIO &io = ImGui::GetIO();
    // If the OS hardware cursor is visible (e.g. the in-game world map is open),
    // track ITS position and suppress our software cursor — otherwise there are two
    // mismatched cursors. In normal gameplay the OS cursor is hidden, so we drive a
    // software cursor from raw-input deltas.
    CURSORINFO ci{};
    ci.cbSize = sizeof(ci);
    g_os_cursor = GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) != 0;
    if (g_os_cursor)
    {
        POINT p{};
        if (GetCursorPos(&p) && g_hwnd)
        {
            ScreenToClient(g_hwnd, &p);
            g_mouse_x = static_cast<float>(p.x);
            g_mouse_y = static_cast<float>(p.y);
        }
        g_raw_dx.exchange(0, std::memory_order_relaxed); // consume; don't drift
        g_raw_dy.exchange(0, std::memory_order_relaxed);
        g_need_center = true; // re-seed the soft cursor if the OS cursor hides again
    }
    else
    {
        if (g_need_center)
        {
            g_mouse_x = io.DisplaySize.x * 0.5f;
            g_mouse_y = io.DisplaySize.y * 0.5f;
            g_need_center = false;
        }
        g_mouse_x += static_cast<float>(g_raw_dx.exchange(0, std::memory_order_relaxed));
        g_mouse_y += static_cast<float>(g_raw_dy.exchange(0, std::memory_order_relaxed));
        if (g_mouse_x < 0.0f) g_mouse_x = 0.0f;
        if (g_mouse_y < 0.0f) g_mouse_y = 0.0f;
        if (io.DisplaySize.x > 0 && g_mouse_x > io.DisplaySize.x) g_mouse_x = io.DisplaySize.x;
        if (io.DisplaySize.y > 0 && g_mouse_y > io.DisplaySize.y) g_mouse_y = io.DisplaySize.y;
    }
    io.AddMousePosEvent(g_mouse_x, g_mouse_y);
    const uint32_t b = g_raw_btn.load();
    io.AddMouseButtonEvent(0, (b & 1u) != 0);
    io.AddMouseButtonEvent(1, (b & 2u) != 0);
    io.AddMouseButtonEvent(2, (b & 4u) != 0);
    const int w = g_raw_wheel.exchange(0, std::memory_order_relaxed);
    if (w != 0)
        io.AddMouseWheelEvent(0.0f, static_cast<float>(w) / static_cast<float>(WHEEL_DELTA));

    // keyboard events captured on the message thread (for Ctrl+A / Ctrl+C etc.)
    {
        std::lock_guard<std::mutex> lk(g_key_mtx);
        for (const auto &e : g_key_events)
            io.AddKeyEvent(e.key, e.down);
        g_key_events.clear();
    }

    // gamepad -> ImGui nav (real state from the XInput hook; the game's own read
    // is zeroed while the menu is open so the character doesn't move).
    if (g_pad_ok)
    {
        // Tell ImGui a gamepad is present (the Win32 backend clears this each
        // NewFrame and our XInput hook zeroes the state it polls, so without this
        // gamepad nav never engages). Must be set AFTER ImGui_ImplWin32_NewFrame.
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
        const WORD bt = g_pad.wButtons;
        // Mark gamepad as the active input source (for control hints) on any button
        // or a stick pushed past the deadzone.
        if (bt != 0 || g_pad.sThumbLX > 12000 || g_pad.sThumbLX < -12000 ||
            g_pad.sThumbLY > 12000 || g_pad.sThumbLY < -12000)
            g_last_input.store(1, std::memory_order_relaxed);
        // Mask the menu-toggle combo's buttons out of the ImGui nav feed: e.g. the
        // default Y+R3 combo's Y is GamepadFaceUp -> ImGuiNavInput_Input, which
        // ACTIVATES the focused widget (the master checkbox) while you press the
        // combo to close the menu. Those buttons are reserved for toggling the
        // menu, not for navigating it; tab-switch (LB/RB) reads g_pad directly.
        const WORD nbt = bt & ~goblin::config::toggleGamepadMask;
        io.AddKeyEvent(ImGuiKey_GamepadDpadUp,    (nbt & XINPUT_GAMEPAD_DPAD_UP) != 0);
        io.AddKeyEvent(ImGuiKey_GamepadDpadDown,  (nbt & XINPUT_GAMEPAD_DPAD_DOWN) != 0);
        io.AddKeyEvent(ImGuiKey_GamepadDpadLeft,  (nbt & XINPUT_GAMEPAD_DPAD_LEFT) != 0);
        io.AddKeyEvent(ImGuiKey_GamepadDpadRight, (nbt & XINPUT_GAMEPAD_DPAD_RIGHT) != 0);
        io.AddKeyEvent(ImGuiKey_GamepadFaceDown,  (nbt & XINPUT_GAMEPAD_A) != 0); // activate
        io.AddKeyEvent(ImGuiKey_GamepadFaceRight, (nbt & XINPUT_GAMEPAD_B) != 0); // cancel
        io.AddKeyEvent(ImGuiKey_GamepadFaceUp,    (nbt & XINPUT_GAMEPAD_Y) != 0);
        io.AddKeyEvent(ImGuiKey_GamepadFaceLeft,  (nbt & XINPUT_GAMEPAD_X) != 0);
        io.AddKeyEvent(ImGuiKey_GamepadL1,        (nbt & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0);
        io.AddKeyEvent(ImGuiKey_GamepadR1,        (nbt & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0);
        io.AddKeyEvent(ImGuiKey_GamepadStart,     (nbt & XINPUT_GAMEPAD_START) != 0);
        // Deadzone above XInput's own (~0.24) so stick drift on a Steam Deck does
        // not continuously feed nav (which reads as endless/instant movement).
        const float lx = g_pad.sThumbLX / 32767.0f;
        const float ly = g_pad.sThumbLY / 32767.0f;
        constexpr float DZ = 0.35f;
        io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft,  lx < -DZ, lx < -DZ ? -lx : 0.0f);
        io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, lx >  DZ, lx >  DZ ?  lx : 0.0f);
        io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp,    ly >  DZ, ly >  DZ ?  ly : 0.0f);
        io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown,  ly < -DZ, ly < -DZ ? -ly : 0.0f);
    }
}

// ── DX12 device-object (re)creation ──
void teardown_dx12()
{
    if (!g_dx12_inited)
        return;
    ImGui_ImplDX12_Shutdown();
    if (g_fence) { g_fence->Release(); g_fence = nullptr; }
    if (g_atlas_tex) { g_atlas_tex->Release(); g_atlas_tex = nullptr; }
    if (g_logo_tex) { g_logo_tex->Release(); g_logo_tex = nullptr; }
    g_atlas_ready = false;
    g_atlas_gpu = D3D12_GPU_DESCRIPTOR_HANDLE{};
    g_logo_gpu = D3D12_GPU_DESCRIPTOR_HANDLE{};
    if (g_command_list) { g_command_list->Release(); g_command_list = nullptr; }
    if (g_frames)
    {
        for (UINT i = 0; i < g_buffer_count; ++i)
        {
            if (g_frames[i].render_target) g_frames[i].render_target->Release();
            if (g_frames[i].allocator) g_frames[i].allocator->Release();
        }
        delete[] g_frames;
        g_frames = nullptr;
    }
    if (g_rtv_heap) { g_rtv_heap->Release(); g_rtv_heap = nullptr; }
    if (g_srv_heap) { g_srv_heap->Release(); g_srv_heap = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
    g_dx12_inited = false;
}

bool init_dx12(IDXGISwapChain3 *sc)
{
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(sc->GetDesc(&desc)))
        return false;
    if (FAILED(sc->GetDevice(IID_PPV_ARGS(&g_device))))
        return false;
    g_hwnd = desc.OutputWindow;
    g_buffer_count = desc.BufferCount;

    D3D12_DESCRIPTOR_HEAP_DESC srv{};
    srv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv.NumDescriptors = 3; // [0] imgui font, [1] category-icon atlas, [2] mod logo
    srv.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_device->CreateDescriptorHeap(&srv, IID_PPV_ARGS(&g_srv_heap))))
        return false;

    D3D12_DESCRIPTOR_HEAP_DESC rtv{};
    rtv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv.NumDescriptors = g_buffer_count;
    rtv.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(g_device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&g_rtv_heap))))
        return false;

    const UINT rtv_size =
        g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    g_frames = new FrameContext[g_buffer_count];
    for (UINT i = 0; i < g_buffer_count; ++i)
    {
        g_frames[i] = FrameContext{};
        g_frames[i].rtv_handle = h;
        if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                    IID_PPV_ARGS(&g_frames[i].allocator))))
            return false;
        ID3D12Resource *back = nullptr;
        if (SUCCEEDED(sc->GetBuffer(i, IID_PPV_ARGS(&back))) && back)
        {
            g_device->CreateRenderTargetView(back, nullptr, h);
            g_frames[i].render_target = back;
        }
        h.ptr += rtv_size;
    }

    if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           g_frames[0].allocator, nullptr,
                                           IID_PPV_ARGS(&g_command_list))))
        return false;
    g_command_list->Close();

    g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
    if (!g_fence_event)
        g_fence_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    g_fence_val = 0;

    if (!g_context_inited)
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr; // don't drop an imgui.ini next to the game
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        // Load a system font WITH Cyrillic (the game/dump text can be Russian);
        // the default ImGui font is ASCII-only and shows '?' for it.
        {
            const char *candidates[] = {"C:\\Windows\\Fonts\\segoeui.ttf",
                                        "C:\\Windows\\Fonts\\arial.ttf",
                                        "C:\\Windows\\Fonts\\tahoma.ttf"};
            bool loaded = false;
            for (const char *fp : candidates)
            {
                if (GetFileAttributesA(fp) != INVALID_FILE_ATTRIBUTES)
                {
                    io.Fonts->AddFontFromFileTTF(fp, 18.0f, nullptr,
                                                 io.Fonts->GetGlyphRangesCyrillic());
                    loaded = true;
                    spdlog::info("[OVERLAY] font: {} (Cyrillic)", fp);
                    break;
                }
            }
            if (!loaded)
            {
                io.Fonts->AddFontDefault();
                spdlog::warn("[OVERLAY] no system font found; Cyrillic will show as '?'");
            }
        }
        apply_er_style();
        ImGui_ImplWin32_Init(g_hwnd);
        g_orig_wndproc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(hkWndProc)));
        g_context_inited = true;
    }

    ImGui_ImplDX12_Init(g_device, g_buffer_count, desc.BufferDesc.Format, g_srv_heap,
                        g_srv_heap->GetCPUDescriptorHandleForHeapStart(),
                        g_srv_heap->GetGPUDescriptorHandleForHeapStart());
    g_dx12_inited = true;
    spdlog::info("[OVERLAY] DX12 backend ready ({} buffers, {}x{})", g_buffer_count,
                 desc.BufferDesc.Width, desc.BufferDesc.Height);
    return true;
}

// Block until the GPU finishes our submitted work, so we never reuse the
// command list/allocator (or free RTVs on resize) mid-flight. Cheap here: the
// overlay only renders while the menu is open, not during gameplay.
void wait_gpu()
{
    if (!g_fence || !g_command_queue)
        return;
    const UINT64 v = ++g_fence_val;
    if (FAILED(g_command_queue->Signal(g_fence, v)))
        return;
    if (g_fence->GetCompletedValue() < v && g_fence_event)
    {
        g_fence->SetEventOnCompletion(v, g_fence_event);
        WaitForSingleObject(g_fence_event, 1000);
    }
}

// Raw GPU submit, isolated so an SEH guard can wrap it (POD locals only).
void submit_frame(IDXGISwapChain3 *sc)
{
    __try
    {
        const UINT idx = sc->GetCurrentBackBufferIndex();
        if (idx >= g_buffer_count)
            return;
        FrameContext &f = g_frames[idx];
        if (!f.allocator || !f.render_target)
            return;
        f.allocator->Reset();

        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b.Transition.pResource = f.render_target;
        b.Transition.Subresource = 0;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;

        g_command_list->Reset(f.allocator, nullptr);
        g_command_list->ResourceBarrier(1, &b);
        g_command_list->OMSetRenderTargets(1, &f.rtv_handle, FALSE, nullptr);
        ID3D12DescriptorHeap *heaps[] = {g_srv_heap};
        g_command_list->SetDescriptorHeaps(1, heaps);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_command_list);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        g_command_list->ResourceBarrier(1, &b);
        g_command_list->Close();
        ID3D12CommandList *lists[] = {g_command_list};
        g_command_queue->ExecuteCommandLists(1, lists);
        wait_gpu(); // serialize: GPU done before we reuse the list/allocator next frame
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // A bad frame must never take the whole game down.
    }
}

// Upload one RGBA8 image to a DEFAULT texture and create its SRV at heap slot
// `srv_index`. Fence-waits so it's ready before first draw. Returns false on a
// hard allocation failure. Plain (no SEH) — the caller wraps it.
static bool upload_rgba(const unsigned char *rgba, int w, int h, UINT srv_index,
                        ID3D12Resource **out_tex, D3D12_GPU_DESCRIPTOR_HANDLE *out_gpu)
{
    const UINT inc =
        g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_HEAP_PROPERTIES hp_def{};
    hp_def.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = static_cast<UINT64>(w);
    td.Height = static_cast<UINT>(h);
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (FAILED(g_device->CreateCommittedResource(&hp_def, D3D12_HEAP_FLAG_NONE, &td,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(out_tex))))
        return false;

    const UINT row = static_cast<UINT>(w) * 4;
    const UINT arow = (row + 255u) & ~255u; // 256-byte row alignment
    const UINT64 upsize = static_cast<UINT64>(arow) * h;
    D3D12_HEAP_PROPERTIES hp_up{};
    hp_up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = upsize;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *upbuf = nullptr;
    if (FAILED(g_device->CreateCommittedResource(&hp_up, D3D12_HEAP_FLAG_NONE, &bd,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                 IID_PPV_ARGS(&upbuf))))
        return false;

    void *mapped = nullptr;
    D3D12_RANGE no_read{0, 0};
    if (SUCCEEDED(upbuf->Map(0, &no_read, &mapped)) && mapped)
    {
        for (int y = 0; y < h; ++y)
            memcpy(static_cast<char *>(mapped) + static_cast<size_t>(y) * arow,
                   rgba + static_cast<size_t>(y) * row, row);
        upbuf->Unmap(0, nullptr);
    }

    g_frames[0].allocator->Reset();
    g_command_list->Reset(g_frames[0].allocator, nullptr);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = *out_tex;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = upbuf;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = 0;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    src.PlacedFootprint.Footprint.Width = static_cast<UINT>(w);
    src.PlacedFootprint.Footprint.Height = static_cast<UINT>(h);
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = arow;
    g_command_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = *out_tex;
    b.Transition.Subresource = 0;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    g_command_list->ResourceBarrier(1, &b);
    g_command_list->Close();
    ID3D12CommandList *lists[] = {g_command_list};
    g_command_queue->ExecuteCommandLists(1, lists);

    ID3D12Fence *fence = nullptr;
    if (SUCCEEDED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
    {
        HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        g_command_queue->Signal(fence, 1);
        if (ev && fence->GetCompletedValue() < 1)
        {
            fence->SetEventOnCompletion(1, ev);
            WaitForSingleObject(ev, 1000);
        }
        if (ev) CloseHandle(ev);
        fence->Release();
    }
    upbuf->Release();

    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_srv_heap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(inc) * srv_index;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    g_device->CreateShaderResourceView(*out_tex, &sd, cpu);
    *out_gpu = g_srv_heap->GetGPUDescriptorHandleForHeapStart();
    out_gpu->ptr += static_cast<SIZE_T>(inc) * srv_index;
    return true;
}

// One-time GPU upload of the category-icon atlas (SRV index 1) + the mod logo
// (index 2). Fence-waited. On failure we set g_atlas_ready anyway so we don't
// retry every frame (just no icons/logo).
void try_upload_atlas()
{
    if (g_atlas_ready || !g_dx12_inited || !g_command_queue || !g_device || !g_srv_heap)
        return;
    using namespace goblin::overlay_icons;
    __try
    {
        upload_rgba(ATLAS_RGBA, ATLAS_W, ATLAS_H, 1, &g_atlas_tex, &g_atlas_gpu);
        upload_rgba(LOGO_RGBA, LOGO_W, LOGO_H, 2, &g_logo_tex, &g_logo_gpu);
        g_atlas_ready = true;
        spdlog::info("[OVERLAY] atlas {}x{} + logo {}x{} uploaded",
                     ATLAS_W, ATLAS_H, LOGO_W, LOGO_H);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        g_atlas_ready = true; // give up rather than crash/retry
    }
}

const goblin::overlay_icons::IconCell *find_icon_cell(const char *key)
{
    using namespace goblin::overlay_icons;
    for (int i = 0; i < ICON_CELL_COUNT; ++i)
        if (std::strcmp(ICON_CELLS[i].key, key) == 0)
            return &ICON_CELLS[i];
    return nullptr;
}

void render(IDXGISwapChain3 *sc)
{
    const bool focused = game_focused();
    try_upload_atlas();
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    if (focused)
        feed_input(); // drain raw-input mouse into ImGui before NewFrame processes events
    else
    {
        // Alt-tabbed away: the OS shows its own cursor, so don't drive ours (a 2nd
        // offset cursor that clicks too). Drop half-captured state; re-center on return.
        g_raw_btn.store(0);
        g_raw_dx.exchange(0, std::memory_order_relaxed);
        g_raw_dy.exchange(0, std::memory_order_relaxed);
        g_need_center = true;
    }
    process_rebind(); // commit/cancel an in-progress hotkey rebind
    ImGui::NewFrame();
    // Draw our software cursor only when focused AND the OS cursor isn't already
    // showing (the in-game map shows the OS cursor -> avoid a double cursor).
    ImGui::GetIO().MouseDrawCursor = focused && !g_os_cursor;
    draw_settings_window();
    ImGui::Render();
    submit_frame(sc);
}

// ── Hooks ──
HRESULT WINAPI hkPresent(IDXGISwapChain3 *sc, UINT sync, UINT flags)
{
    goblin::map_timing::on_present(); // diagnostic: render-thread sampling profiler

    // Open/close on the toggle key (keyboard) OR the gamepad combo. (When the
    // overlay is DISABLED these same inputs master-toggle icons instead, handled
    // in goblin_inject's toggle_hotkey_loop — see its master_mode gate.)
    // While rebinding a hotkey in the overlay, ignore these inputs so binding
    // F10 / Y+R3 / Esc / B doesn't also open/close the menu.
    // Only react to hotkeys when the game window is foreground — GetAsyncKeyState is
    // global, so without this an Esc/F10 pressed in another app while alt-tabbed
    // would close/toggle our menu.
    const bool focused = game_focused();
    const bool rebinding = g_rebind_mode.load() != 0;
    static bool prev_open_in = false;
    const int open_key = static_cast<int>(goblin::config::toggleInjectionKey); // F10
    const uint16_t pad_mask = goblin::config::toggleGamepadMask;               // Y+R3
    const bool key = focused && (GetAsyncKeyState(open_key) & 0x8000) != 0;
    const bool combo = focused && g_pad_ok && pad_mask && (g_pad.wButtons & pad_mask) == pad_mask;
    const bool open_in = key || combo;
    if (open_in && !prev_open_in && !rebinding)
        g_menu_open.store(!g_menu_open.load());
    prev_open_in = open_in;

    // Close-only shortcuts while the menu is open: ESC (keyboard) or B (gamepad).
    static bool prev_esc = false, prev_padb = false;
    const bool esc  = focused && (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    const bool padb = focused && g_pad_ok && (g_pad.wButtons & XINPUT_GAMEPAD_B) != 0;
    if (g_menu_open.load() && !rebinding && ((esc && !prev_esc) || (padb && !prev_padb)))
        g_menu_open.store(false);
    prev_esc = esc;
    prev_padb = padb;

    // Open/close side effects (also catches the in-panel Close button):
    // reload settings from disk on open; auto-save on close.
    static bool prev_open = false;
    const bool open_now = g_menu_open.load();
    if (open_now && !prev_open)
    {
        g_need_center = true;
        goblin::load_config(goblin::g_ini_path);
        goblin::reapply_live_settings();
    }
    else if (!open_now && prev_open)
    {
        // Swallow whatever gamepad buttons are held right now (the close press,
        // e.g. B, or the toggle combo) until released, so it doesn't reach the game.
        g_pad_swallow.store(g_pad_ok ? g_pad.wButtons : 0);
        goblin::save_config(goblin::g_ini_path);
    }
    prev_open = open_now;

    if (!g_dx12_inited)
    {
        if (!init_dx12(sc))
            return oPresent(sc, sync, flags);
    }
    if (g_menu_open.load() && g_command_queue)
        render(sc);

    return oPresent(sc, sync, flags);
}

HRESULT WINAPI hkResizeBuffers(IDXGISwapChain3 *sc, UINT bc, UINT w, UINT h,
                               DXGI_FORMAT fmt, UINT flags)
{
    wait_gpu();      // flush our in-flight work so freeing the RTVs is safe (AMD/Deck)
    teardown_dx12(); // drop our RTV refs so the resize can free the buffers
    HRESULT hr = oResizeBuffers(sc, bc, w, h, fmt, flags);
    // next Present re-inits the DX12 objects against the resized swapchain
    return hr;
}

void WINAPI hkExecuteCommandLists(ID3D12CommandQueue *q, UINT n, ID3D12CommandList *const *l)
{
    if (!g_command_queue && q)
    {
        const D3D12_COMMAND_QUEUE_DESC d = q->GetDesc();
        if (d.Type == D3D12_COMMAND_LIST_TYPE_DIRECT)
            g_command_queue = q; // the graphics queue that presents
    }
    oExecuteCommandLists(q, n, l);
}

// Throwaway device + swapchain just to read the vtable function pointers.
bool capture_vtables(void *&present, void *&resize, void *&execcl)
{
    HMODULE hd3d12 = GetModuleHandleA("d3d12.dll");
    if (!hd3d12) hd3d12 = LoadLibraryA("d3d12.dll");
    HMODULE hdxgi = GetModuleHandleA("dxgi.dll");
    if (!hdxgi) hdxgi = LoadLibraryA("dxgi.dll");
    if (!hd3d12 || !hdxgi)
        return false;

    auto pD3D12CreateDevice =
        reinterpret_cast<decltype(&D3D12CreateDevice)>(GetProcAddress(hd3d12, "D3D12CreateDevice"));
    auto pCreateDXGIFactory1 =
        reinterpret_cast<decltype(&CreateDXGIFactory1)>(GetProcAddress(hdxgi, "CreateDXGIFactory1"));
    if (!pD3D12CreateDevice || !pCreateDXGIFactory1)
        return false;

    ID3D12Device *dev = nullptr;
    if (FAILED(pD3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev))) || !dev)
        return false;

    ID3D12CommandQueue *queue = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) || !queue)
    {
        dev->Release();
        return false;
    }

    // hidden dummy window for the dummy swapchain
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"MFG_OverlayDummy";
    RegisterClassExW(&wc);
    HWND dummy = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100,
                                 nullptr, nullptr, wc.hInstance, nullptr);

    IDXGIFactory2 *factory = nullptr;
    bool ok = false;
    if (dummy && SUCCEEDED(pCreateDXGIFactory1(IID_PPV_ARGS(&factory))) && factory)
    {
        DXGI_SWAP_CHAIN_DESC1 scd{};
        scd.BufferCount = 2;
        scd.Width = 100;
        scd.Height = 100;
        scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        scd.SampleDesc.Count = 1;
        IDXGISwapChain1 *sc1 = nullptr;
        if (SUCCEEDED(factory->CreateSwapChainForHwnd(queue, dummy, &scd, nullptr, nullptr, &sc1)) && sc1)
        {
            void **sc_vtbl = *reinterpret_cast<void ***>(sc1);
            void **cq_vtbl = *reinterpret_cast<void ***>(queue);
            present = sc_vtbl[8];  // IDXGISwapChain::Present
            resize = sc_vtbl[13];  // IDXGISwapChain::ResizeBuffers
            execcl = cq_vtbl[10];  // ID3D12CommandQueue::ExecuteCommandLists
            ok = present && resize && execcl;
            sc1->Release();
        }
        factory->Release();
    }

    if (dummy) DestroyWindow(dummy);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    queue->Release();
    dev->Release();
    return ok;
}
} // namespace

void goblin::overlay::setup()
{
    if (!goblin::config::enableOverlay)
    {
        spdlog::info("[OVERLAY] disabled via ini (enable_overlay = false)");
        return;
    }
    void *present = nullptr, *resize = nullptr, *execcl = nullptr;
    if (!capture_vtables(present, resize, execcl))
    {
        spdlog::error("[OVERLAY] failed to capture DX12 vtables; overlay disabled");
        return;
    }
    spdlog::info("[OVERLAY] vtables: Present={:p} ResizeBuffers={:p} ExecuteCommandLists={:p}",
                 present, resize, execcl);

    // Queue the hooks; modutils::enable_hooks() (called by setup_mod) applies them.
    modutils::hook(present, reinterpret_cast<void *>(&hkPresent),
                   reinterpret_cast<void **>(&oPresent));
    modutils::hook(resize, reinterpret_cast<void *>(&hkResizeBuffers),
                   reinterpret_cast<void **>(&oResizeBuffers));
    modutils::hook(execcl, reinterpret_cast<void *>(&hkExecuteCommandLists),
                   reinterpret_cast<void **>(&oExecuteCommandLists));

    // Raw-input hook: ER reads keyboard/mouse via raw input, so this is how we
    // both capture the mouse for ImGui and block the game while the menu is open.
    if (HMODULE u32 = GetModuleHandleA("user32.dll"))
    {
        if (void *grid = reinterpret_cast<void *>(GetProcAddress(u32, "GetRawInputData")))
        {
            modutils::hook(grid, reinterpret_cast<void *>(&hkGetRawInputData),
                           reinterpret_cast<void **>(&oGetRawInputData));
            spdlog::info("[OVERLAY] hooked GetRawInputData (input capture/block)");
        }
        else
            spdlog::warn("[OVERLAY] GetRawInputData not found; menu input limited");
    }

    // XInput hook: gate the game's gamepad while the menu is open (so the
    // character/camera don't move) and capture it for ImGui nav. Hook whichever
    // xinput DLL the game already loaded.
    {
        const char *xdlls[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
        void *xfn = nullptr;
        for (const char *d : xdlls)
        {
            HMODULE h = GetModuleHandleA(d);
            if (h) { xfn = reinterpret_cast<void *>(GetProcAddress(h, "XInputGetState")); if (xfn) break; }
        }
        if (!xfn) // none loaded yet — pull one in
            if (HMODULE h = LoadLibraryA("xinput1_4.dll"))
                xfn = reinterpret_cast<void *>(GetProcAddress(h, "XInputGetState"));
        if (xfn)
        {
            modutils::hook(xfn, reinterpret_cast<void *>(&hkXInputGetState),
                           reinterpret_cast<void **>(&oXInputGetState));
            spdlog::info("[OVERLAY] hooked XInputGetState (gamepad gate + nav)");
        }
        else
            spdlog::warn("[OVERLAY] XInputGetState not found; gamepad not gated");
    }
    spdlog::info("[OVERLAY] hooks queued (open/close: configured toggle key, default F10)");
}
