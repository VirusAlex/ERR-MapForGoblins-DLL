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
#include "goblin_i18n.hpp"
#include "goblin_overlay_icons.hpp"
#include "goblin_map_icons.hpp" // shared DefineBitsLossless2 icon tags (decoded here for the atlas)
#include "miniz.h"              // zlib inflate to decode the tags
#include "goblin_inject.hpp"
#include "goblin_markers.hpp"
#include "goblin_map_timing.hpp"
#include "goblin_gfx_probe.hpp"
#include "goblin_diag.hpp"
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
#include <thread>
#include <vector>
#include <commdlg.h> // GetOpenFileNameW (WIN32_LEAN_AND_MEAN excludes it from windows.h)

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#include "stb_image.h" // dev Icon Preview: decode a picked PNG to RGBA
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h" // quality downscale to mirror the build pipeline's 96px normalize

#include "version.h" // PROJECT_VERSION (generated)

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "comdlg32.lib")

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

// Captured active-controller state for ImGui gamepad nav and overlay hotkeys.
// The game may read only controller slot 0, so hkPresent also refreshes this from
// every XInput slot before it evaluates menu inputs.
XINPUT_GAMEPAD g_pad{};
bool g_pad_ok = false;
DWORD g_pad_index = 0;

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

// Dev "Icon Preview" (Debug tab): pick a PNG off disk, show it floating + centered with a transparent
// background at a chosen on-map size, so icon art can be eyeballed against the live map without a rebuild.
ID3D12Resource *g_preview_tex = nullptr;       // picked-png texture (g_srv_heap index 3)
D3D12_GPU_DESCRIPTOR_HANDLE g_preview_gpu{};
int g_preview_w = 0, g_preview_h = 0;          // source png dimensions
int g_preview_iw = 0, g_preview_ih = 0;        // normalized (fit-to-96 then cropped) dims, for proportional draw
std::atomic<bool> g_preview_show{false};       // floating preview window visible
float g_preview_px = 65.0f;                    // on-screen height in px (slider; ~map-icon default)
std::mutex g_preview_mx;
std::wstring g_preview_path;                    // picked path (set by the dialog thread)
std::atomic<bool> g_preview_dirty{false};      // a new path is waiting to be decoded (render thread)
std::atomic<bool> g_preview_dialog_open{false}; // a file dialog is already up (avoid stacking)
static void open_preview_dialog(); // defined below (opens the file dialog on a worker thread)

// GPU sync: a fence we signal+wait after each submit so we never reuse the
// command list/allocator (or free RTVs on resize) while the GPU is still using
// them - the AMD/Steam-Deck "device removed" failure mode if omitted.
ID3D12Fence *g_fence = nullptr;
UINT64 g_fence_val = 0;
HANDLE g_fence_event = nullptr;

HWND g_hwnd = nullptr;
WNDPROC g_orig_wndproc = nullptr;

std::atomic<bool> g_menu_open{false};
// Buttons held at the moment the overlay closed (e.g. B used to close it). We
// keep suppressing them in the game's XInput read until released, so the closing
// press does not fall through into gameplay.
std::atomic<WORD> g_pad_swallow[XUSER_MAX_COUNT]{};
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

constexpr int PAD_ACTIVITY_DEADZONE = 12000;

bool pad_has_activity(const XINPUT_GAMEPAD &pad)
{
    return pad.wButtons != 0 ||
           pad.sThumbLX > PAD_ACTIVITY_DEADZONE || pad.sThumbLX < -PAD_ACTIVITY_DEADZONE ||
           pad.sThumbLY > PAD_ACTIVITY_DEADZONE || pad.sThumbLY < -PAD_ACTIVITY_DEADZONE ||
           pad.sThumbRX > PAD_ACTIVITY_DEADZONE || pad.sThumbRX < -PAD_ACTIVITY_DEADZONE ||
           pad.sThumbRY > PAD_ACTIVITY_DEADZONE || pad.sThumbRY < -PAD_ACTIVITY_DEADZONE ||
           pad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD ||
           pad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
}

bool pad_has_mask(const XINPUT_GAMEPAD &pad, uint16_t mask)
{
    return mask != 0 && (pad.wButtons & mask) == mask;
}

void set_active_gamepad(DWORD idx, const XINPUT_GAMEPAD &pad)
{
    g_pad = pad;
    g_pad_ok = true;
    g_pad_index = idx;
}

void refresh_gamepad_state()
{
    if (!oXInputGetState)
        return;

    const uint16_t toggle_mask = goblin::config::toggleGamepadMask;
    const bool menu_open = g_menu_open.load();
    bool have_first = false, have_current = false, have_active = false;
    bool have_close = false, have_combo = false;
    XINPUT_GAMEPAD first{}, current{}, active{}, close_pad{}, combo{};
    DWORD first_idx = 0, current_idx = 0, active_idx = 0, close_idx = 0, combo_idx = 0;

    for (DWORD idx = 0; idx < XUSER_MAX_COUNT; ++idx)
    {
        XINPUT_STATE state{};
        if (oXInputGetState(idx, &state) != ERROR_SUCCESS)
            continue;

        const XINPUT_GAMEPAD &pad = state.Gamepad;
        if (!have_first)
        {
            first = pad;
            first_idx = idx;
            have_first = true;
        }
        if (idx == g_pad_index)
        {
            current = pad;
            current_idx = idx;
            have_current = true;
        }
        if (!have_active && pad_has_activity(pad))
        {
            active = pad;
            active_idx = idx;
            have_active = true;
        }
        if (menu_open && (pad.wButtons & XINPUT_GAMEPAD_B))
        {
            close_pad = pad;
            close_idx = idx;
            have_close = true;
        }
        if (pad_has_mask(pad, toggle_mask))
        {
            combo = pad;
            combo_idx = idx;
            have_combo = true;
            break;
        }
    }

    if (have_combo)
        set_active_gamepad(combo_idx, combo);
    else if (have_close)
        set_active_gamepad(close_idx, close_pad);
    else if (have_active)
        set_active_gamepad(active_idx, active);
    else if (have_current)
        set_active_gamepad(current_idx, current);
    else if (have_first)
        set_active_gamepad(first_idx, first);
    else
        g_pad_ok = false;
}

void capture_swallow_buttons()
{
    for (DWORD idx = 0; idx < XUSER_MAX_COUNT; ++idx)
    {
        WORD held = 0;
        if (oXInputGetState)
        {
            XINPUT_STATE state{};
            if (oXInputGetState(idx, &state) == ERROR_SUCCESS)
                held = state.Gamepad.wButtons;
        }
        else if (g_pad_ok && idx == g_pad_index)
        {
            held = g_pad.wButtons;
        }
        g_pad_swallow[idx].store(held);
    }
}

void reset_rebind_state()
{
    g_rebind_mode.store(0);
    g_rebind_target = nullptr;
    g_captured_vk.store(0);
    g_captured_up.store(false);
    g_rebind_pad_accum = 0;
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
        reset_rebind_state();
        return;
    }
    if (mode == 1) // keyboard key - commit on RELEASE so the bind press doesn't
    {              // also fire the action it was just bound to
        const uint32_t vk = g_captured_vk.load();
        if (vk != 0 && g_captured_up.load())
        {
            *static_cast<uint32_t *>(g_rebind_target) = vk;
            reset_rebind_state();
        }
    }
    else // mode 2: gamepad combo - accumulate held buttons, commit on release
    {
        const uint16_t held = g_pad_ok ? g_pad.wButtons : 0;
        if (held)
            g_rebind_pad_accum |= held;
        else if (g_rebind_pad_accum)
        {
            *static_cast<uint16_t *>(g_rebind_target) = g_rebind_pad_accum;
            reset_rebind_state();
        }
    }
}

// ── Settings tab: live-applied category toggles ──
// Render one schema section: collapsing header + per-section "all on/off" + each
// entry (checkbox / slider / hotkey rebind). Sets `changed` on any edit.
void draw_section(const goblin::IniSection &sec, bool &changed)
{
    namespace tr = goblin::i18n;
    const tr::Language lang = tr::current_language();

    ImGui::PushID(sec.name);
    // Group toggles: flip every Bool in this section at once.
    auto set_section = [&](bool v) {
        for (const auto &e : sec.entries)
            if (e.type == goblin::IniType::Bool &&
                !(goblin::profile_is_vanilla() && e.err_only))
                *static_cast<bool *>(e.target) = v;
        changed = true;
    };
    // AllowOverlap so the "all on"/"all off" buttons drawn on top of the
    // header row capture the click instead of the header toggling the fold.
    const bool open = ImGui::CollapsingHeader(
        tr::section_label(sec.name, lang), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
    // "all on"/"all off" right-aligned on the header row (work even when collapsed).
    const ImGuiStyle &st = ImGui::GetStyle();
    const char *all_on = tr::tr(tr::TextId::AllOn, lang);
    const char *all_off = tr::tr(tr::TextId::AllOff, lang);
    const float w_on  = ImGui::CalcTextSize(all_on).x + st.FramePadding.x * 2;
    const float w_off = ImGui::CalcTextSize(all_off).x + st.FramePadding.x * 2;
    ImGui::SameLine(ImGui::GetContentRegionMax().x - w_on - w_off - st.ItemSpacing.x);
    if (ImGui::SmallButton(all_on)) set_section(true);
    ImGui::SameLine();
    if (ImGui::SmallButton(all_off)) set_section(false);
    ImGui::PopID();
    if (!open)
        return;
    if (std::strcmp(sec.name, "Compatibility") == 0)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.82f, 1.0f, 1.0f));
        ImGui::TextWrapped("%s", tr::tr(tr::TextId::RandomizerHint, lang));
        ImGui::PopStyleColor();
    }
    {
        for (const auto &e : sec.entries)
        {
            if (goblin::profile_is_vanilla() && e.err_only)
                continue;
            ImGui::PushID(e.key);
            if (std::strcmp(e.key, "fast_map_open") == 0)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.84f, 0.0f, 1.0f)); // yellow
                ImGui::TextWrapped("%s", tr::tr(tr::TextId::FastMapOpenWarning, lang));
                ImGui::PopStyleColor();
            }
            draw_row_icon(e.key);
            const char *label = tr::entry_label(e.key, lang);
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
                const bool clicked = ImGui::Checkbox(label, &v);
                hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip);
                if (locked) ImGui::PopStyleColor();
                if (clicked && !locked) { *static_cast<bool *>(e.target) = v; changed = true; }
                if (locked)
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", tr::tr(tr::TextId::IniOnly, lang));
                    hovered = hovered || ImGui::IsItemHovered();
                }
            }
            else if (e.type == goblin::IniType::U8)
            {
                int v = *static_cast<uint8_t *>(e.target);
                if (ImGui::SliderInt(label, &v, 0, 30))
                {
                    *static_cast<uint8_t *>(e.target) =
                        static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
                    changed = true;
                }
                hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip);
            }
            else if (e.type == goblin::IniType::String)
            {
                std::string &value = *static_cast<std::string *>(e.target);
                const char *preview = tr::language_preview_label(value, lang);
                if (ImGui::BeginCombo(label, preview))
                {
                    const char *options[] = {"auto", "english", "schinese", "tchinese"};
                    const std::string normalized = tr::normalize_language_config(value);
                    const std::string selected_language = normalized == "auto"
                        ? tr::language_code(tr::current_language())
                        : normalized;
                    for (const char *opt : options)
                    {
                        const bool selected = selected_language == opt &&
                            !(normalized == "auto" && std::strcmp(opt, "auto") == 0);
                        if (ImGui::Selectable(tr::language_option_label(opt, lang), selected))
                        {
                            value = opt;
                            changed = true;
                        }
                        if (selected)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip);
            }
            else // VkKey / GamepadMask: show value + in-place rebind
            {
                const bool is_key = e.type == goblin::IniType::VkKey;
                const std::string val =
                    is_key ? fmt_vk(*static_cast<uint32_t *>(e.target))
                           : fmt_gamepad(*static_cast<uint16_t *>(e.target));
                const bool capturing = g_rebind_mode.load() != 0 && g_rebind_target == e.target;
                char label_buf[192];
                std::snprintf(label_buf, sizeof label_buf, "%s = %s", label, val.c_str());
                const float btn_w = 96.0f;
                ImGui::Selectable(label_buf, false, 0,
                                  ImVec2(ImGui::GetContentRegionAvail().x - btn_w, 0));
                hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip);
                ImGui::SameLine();
                if (capturing)
                    ImGui::TextColored(ImVec4(1.0f, 0.84f, 0.38f, 1.0f), "%s",
                                       is_key ? tr::tr(tr::TextId::PressAKey, lang)
                                              : tr::tr(tr::TextId::PressComboRelease, lang));
                else if (ImGui::SmallButton(tr::entry_label("rebind", lang)))
                {
                    g_rebind_target = e.target;
                    g_rebind_pad_accum = 0;
                    g_captured_vk.store(0);
                    g_captured_up.store(false);
                    g_rebind_mode.store(is_key ? 1 : 2);
                }
            }
            if (e.comment && hovered)
                ImGui::SetTooltip("%s", tr::entry_comment(e.key, e.comment, lang));
            ImGui::PopID();
        }
    }
}


void draw_settings_tab()
{
    namespace tr = goblin::i18n;
    const tr::Language lang = tr::current_language();

    // Blink yellow <-> red every 0.5s so it's hard to miss.
    const bool blink_red = (static_cast<int>(ImGui::GetTime() * 2.0) & 1) != 0;
    ImGui::PushStyleColor(ImGuiCol_Text, blink_red ? ImVec4(1.0f, 0.27f, 0.22f, 1.0f)
                                                   : ImVec4(1.0f, 0.84f, 0.38f, 1.0f));
    ImGui::TextWrapped("%s", tr::tr(tr::TextId::ReopenMapWarning, lang));
    ImGui::PopStyleColor();
    ImGui::Separator();

    bool changed = false;
    // Global show_* toggles: flip EVERY icon category at once (across all sections).
    auto set_all_show = [&](bool v) {
        for (const auto &sec : goblin::ini_schema())
            for (const auto &e : sec.entries)
                if (e.type == goblin::IniType::Bool &&
                    std::strncmp(e.key, "show_", 5) == 0 &&
                    !(goblin::profile_is_vanilla() && e.err_only))
                    *static_cast<bool *>(e.target) = v;
        changed = true;
    };
    ImGui::TextUnformatted(tr::tr(tr::TextId::AllIconCategories, lang));
    ImGui::SameLine();
    if (ImGui::SmallButton(tr::tr(tr::TextId::ShowAll, lang))) set_all_show(true);
    ImGui::SameLine();
    if (ImGui::SmallButton(tr::tr(tr::TextId::HideAll, lang))) set_all_show(false);
    ImGui::Separator();

    // NavFlattened: keyboard/gamepad nav flows through this scroll region as if it
    // were part of the window, so you don't have to "enter" it and can't get stuck.
    // (1.90.9: NavFlattened is a ChildFlag; the old WindowFlag is a no-op.)
    ImGui::BeginChild("##scroll", ImVec2(0, 0), ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_None);
    for (const auto &sec : goblin::ini_schema())
    {
        if (goblin::profile_is_vanilla() && sec.err_only)
            continue;
        if (std::strcmp(sec.name, "Debug") == 0)
            continue; // rendered on the Debug tab instead
        draw_section(sec, changed);
    }
    ImGui::EndChild();

    if (changed)
        goblin::reapply_live_settings(); // live-apply the change
}

// ── Debug tab: diagnostics (debug_logging + marker-dump settings) + dump tool ──
void draw_debug_tab()
{
    namespace tr = goblin::i18n;
    const tr::Language lang = tr::current_language();

    // ── Injection status (top of the tab) ──────────────────────────────────
    // A live readout of every inject (hooks, world-map icons, bitmaps, remap,
    // logo, overlay) with a reason for anything that failed. Players can
    // screenshot or Copy this for a bug report so we can pinpoint the fault.
    ImGui::TextColored(ImVec4(0.80f, 0.68f, 0.40f, 1.0f), "%s", tr::tr(tr::TextId::InjectStatusTitle, lang));
    ImGui::TextDisabled("%s", tr::tr(tr::TextId::InjectStatusHint, lang));
    {
        std::string rep = goblin::diag::report();
        ImGui::BeginChild("##injstatus", ImVec2(-1, 150), true, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(rep.c_str());
        ImGui::EndChild();
        if (ImGui::Button(tr::tr(tr::TextId::CopyStatus, lang)))
            ImGui::SetClipboardText(rep.c_str());
    }
    ImGui::Separator();

    bool changed = false;
    for (const auto &sec : goblin::ini_schema())
        if (std::strcmp(sec.name, "Debug") == 0)
            draw_section(sec, changed);
    if (changed)
        goblin::reapply_live_settings();
    ImGui::Separator();

    ImGui::TextWrapped("%s", tr::tr(tr::TextId::DebugDumpDescription, lang));
    auto fill_dump = [&](goblin::markers::DumpSel sel) {
        std::string s = goblin::markers::dump_to_string(sel);
        g_dump_buf.assign(s.begin(), s.end());
        g_dump_buf.push_back('\0');
    };
    if (ImGui::Button(tr::tr(tr::TextId::DumpBeacons, lang)))
        fill_dump(goblin::markers::DUMP_BEACONS);
    ImGui::SameLine();
    if (ImGui::Button(tr::tr(tr::TextId::DumpStamps, lang)))
        fill_dump(goblin::markers::DUMP_STAMPS);
    ImGui::SameLine();
    if (ImGui::Button(tr::tr(tr::TextId::Copy, lang)) && !g_dump_buf.empty())
        ImGui::SetClipboardText(g_dump_buf.data()); // copies the current dump
    ImGui::SameLine();
    ImGui::TextDisabled("%d %s", g_dump_buf.empty() ? 0 : static_cast<int>(g_dump_buf.size() - 1),
                        tr::tr(tr::TextId::Chars, lang));
    ImGui::Separator();
    if (!g_dump_buf.empty())
    {
        // Read-only, non-selectable view (Copy handles clipboard); scrolls both ways.
        ImGui::BeginChild("##dump", ImVec2(-1, -1), true, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(g_dump_buf.data());
        ImGui::EndChild();
    }
    else
        ImGui::TextDisabled("%s", tr::tr(tr::TextId::NoDumpYet, lang));

    ImGui::Separator();
    ImGui::TextWrapped("Icon Preview: pick a PNG and see it floating + centered with a transparent "
                       "background, at a chosen on-map size. Open the in-game map first, then open this "
                       "overlay over it to compare your art against the live icons.");
    if (ImGui::Button("Icon Preview..."))
        open_preview_dialog();
    if (g_preview_tex)
    {
        ImGui::SameLine();
        if (ImGui::Button("Close preview"))
            g_preview_show.store(false);
        ImGui::SameLine();
        if (!g_preview_show.load() && ImGui::Button("Show preview"))
            g_preview_show.store(true);
        ImGui::SliderFloat("Preview size (px)", &g_preview_px, 8.0f, 256.0f, "%.0f");
        ImGui::TextDisabled("source: %d x %d px", g_preview_w, g_preview_h);
    }
}

// ── About tab: version + links ──
void draw_about_tab()
{
    namespace tr = goblin::i18n;
    const tr::Language lang = tr::current_language();

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
    ImGui::Text("%s %s", tr::tr(tr::TextId::Version, lang), PROJECT_VERSION);
    ImGui::Spacing();
    ImGui::TextWrapped("%s", tr::tr(tr::TextId::AboutDescription, lang));
    ImGui::EndGroup();
    ImGui::Spacing();
    ImGui::Separator();
    struct Link { tr::TextId label; const char *url; const char *btn; };
    static const Link links[] = {
        {tr::TextId::LinkNexus,   "https://www.nexusmods.com/eldenring/mods/10062",     "##nx"},
        {tr::TextId::LinkGithub,  "https://github.com/VirusAlex/ERR-MapForGoblins-DLL", "##gh"},
        {tr::TextId::LinkDiscord, "https://discord.gg/JvTMwPCygB",                      "##dc"},
    };
    for (const auto &l : links)
    {
        ImGui::TextDisabled("%s:", tr::tr(l.label, lang));
        ImGui::TextUnformatted(l.url);
        ImGui::SameLine();
        std::string copy_label = std::string(tr::tr(tr::TextId::Copy, lang)) + l.btn;
        if (ImGui::SmallButton(copy_label.c_str()))
            ImGui::SetClipboardText(l.url);
    }
}

// ── Bottom-of-window control hints, auto-switched by the last input device ──
void draw_control_hints()
{
    namespace tr = goblin::i18n;
    const tr::Language lang = tr::current_language();

    ImGui::Separator();
    if (g_last_input.load() == 1)
        ImGui::TextDisabled("%s", tr::tr(tr::TextId::ControlHintGamepad, lang));
    else
        ImGui::TextDisabled("%s", tr::tr(tr::TextId::ControlHintKeyboard, lang));

}

// ── The overlay window: master switch + tabs ──
void draw_settings_window()
{
    namespace tr = goblin::i18n;
    const tr::Language lang = tr::current_language();

    ImGui::SetNextWindowSize(ImVec2(560, 680), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(tr::tr(tr::TextId::WindowTitle, lang), nullptr))
    {
        ImGui::End();
        return;
    }

    // Master on/off for ALL icons. Settings auto-save on close and auto-reload on
    // open, so no Save/Reload buttons are needed.
    bool show_icons = !goblin::icons_hidden();
    if (ImGui::Checkbox(tr::tr(tr::TextId::MasterToggle, lang), &show_icons))
        goblin::set_icons_hidden(!show_icons);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("%s", tr::tr(tr::TextId::MasterToggleTooltip, lang));
    // Right-aligned Close button.
    constexpr float close_w = 90.0f;
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - close_w);
    if (ImGui::Button(tr::tr(tr::TextId::Close, lang), ImVec2(close_w, 0)))
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
        if (ImGui::BeginTabItem(tr::tr(tr::TextId::TabSettings, lang), nullptr, tab_flag(0))) { draw_settings_tab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(tr::tr(tr::TextId::TabDebug, lang),    nullptr, tab_flag(1))) { draw_debug_tab();    ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(tr::tr(tr::TextId::TabAbout, lang),    nullptr, tab_flag(2))) { draw_about_tab();    ImGui::EndTabItem(); }

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
            // Capture the FIRST key pressed (ignore Space/Enter - they activate the
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

// XInput gate: snapshot whichever controller is currently active, not just slot 0.
// While the menu is open, ZERO the state the game (and ImGui's own backend) read
// so the player/camera don't move. The captured snapshot is fed to ImGui nav in
// feed_input().
DWORD WINAPI hkXInputGetState(DWORD idx, XINPUT_STATE *st)
{
    DWORD r = oXInputGetState(idx, st);
    if (r == ERROR_SUCCESS && st)
    {
        if (!g_pad_ok || idx == g_pad_index || pad_has_activity(st->Gamepad))
            set_active_gamepad(idx, st->Gamepad);
    }
    else if (idx == g_pad_index)
    {
        g_pad_ok = false;
    }

    if (st)
    {
        if (g_menu_open.load())
            ZeroMemory(&st->Gamepad, sizeof(st->Gamepad));
        else if (idx < XUSER_MAX_COUNT && g_pad_swallow[idx].load())
        {
            // Suppress buttons held at close until they are released (no fall-through).
            WORD sw = g_pad_swallow[idx].load() & st->Gamepad.wButtons;
            g_pad_swallow[idx].store(sw);
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
    // track ITS position and suppress our software cursor - otherwise there are two
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
    if (g_preview_tex) { g_preview_tex->Release(); g_preview_tex = nullptr; }
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
    srv.NumDescriptors = 4; // [0] imgui font, [1] category-icon atlas, [2] mod logo, [3] icon-preview
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
        // Base font = Segoe UI (Latin + Cyrillic; the dump text can be Russian).
        // A CJK font is merged on top ONLY when the active UI language is Chinese,
        // so non-Chinese users don't load a CJK file or pay the larger atlas. The
        // CJK merge carries only the glyphs the UI actually uses (font_glyph_seed).
        {
            const goblin::i18n::Language ui_lang = goblin::i18n::current_language();
            const bool need_cjk = ui_lang == goblin::i18n::Language::SimplifiedChinese ||
                                  ui_lang == goblin::i18n::Language::TraditionalChinese;

            static ImVector<ImWchar> base_ranges;
            {
                ImFontGlyphRangesBuilder b;
                b.AddRanges(io.Fonts->GetGlyphRangesDefault());
                b.AddRanges(io.Fonts->GetGlyphRangesCyrillic());
                b.BuildRanges(&base_ranges);
            }
            const char *base_fonts[] = {"C:\\Windows\\Fonts\\segoeui.ttf",
                                        "C:\\Windows\\Fonts\\arial.ttf",
                                        "C:\\Windows\\Fonts\\tahoma.ttf"};
            ImFont *base = nullptr;
            for (const char *fp : base_fonts)
                if (GetFileAttributesA(fp) != INVALID_FILE_ATTRIBUTES &&
                    (base = io.Fonts->AddFontFromFileTTF(fp, 18.0f, nullptr, base_ranges.Data)) != nullptr)
                {
                    spdlog::info("[OVERLAY] base font: {} (Latin + Cyrillic)", fp);
                    break;
                }
            if (!base)
            {
                io.Fonts->AddFontDefault();
                spdlog::warn("[OVERLAY] no base system font found; text may show as '?'");
            }

            if (need_cjk && base)
            {
                static ImVector<ImWchar> cjk_ranges;
                {
                    ImFontGlyphRangesBuilder b;
                    b.AddText(goblin::i18n::font_glyph_seed_utf8()); // only glyphs the UI uses
                    b.BuildRanges(&cjk_ranges);
                }
                ImFontConfig cfg;
                cfg.MergeMode = true; // merge CJK glyphs into the Segoe UI base
                // Prefer the matching script's font first (YaHei=SC, JhengHei=TC).
                const char *cjk_sc[] = {"C:\\Windows\\Fonts\\msyh.ttc", "C:\\Windows\\Fonts\\msjh.ttc",
                                        "C:\\Windows\\Fonts\\simhei.ttf", "C:\\Windows\\Fonts\\simsun.ttc"};
                const char *cjk_tc[] = {"C:\\Windows\\Fonts\\msjh.ttc", "C:\\Windows\\Fonts\\msyh.ttc",
                                        "C:\\Windows\\Fonts\\simsun.ttc", "C:\\Windows\\Fonts\\simhei.ttf"};
                const char *const *cjk_fonts =
                    ui_lang == goblin::i18n::Language::TraditionalChinese ? cjk_tc : cjk_sc;
                bool merged = false;
                for (int i = 0; i < 4; ++i)
                {
                    const char *fp = cjk_fonts[i];
                    if (GetFileAttributesA(fp) != INVALID_FILE_ATTRIBUTES &&
                        io.Fonts->AddFontFromFileTTF(fp, 18.0f, &cfg, cjk_ranges.Data))
                    {
                        merged = true;
                        spdlog::info("[OVERLAY] merged CJK font: {}", fp);
                        break;
                    }
                }
                if (!merged)
                    spdlog::warn("[OVERLAY] no CJK font found; Chinese UI may show as '?'");
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
    goblin::diag::set_overlay(goblin::diag::OverlayState::Active, "");
    spdlog::info("[OVERLAY] DX12 backend ready ({} buffers, {}x{})", g_buffer_count,
                 desc.BufferDesc.Width, desc.BufferDesc.Height);
    return true;
}

// SEH wrapper: the swapchain/device queries + D3D object creation can AV on a torn swapchain state
// (e.g. a G-Sync / fullscreen-flip transition mid-init). A failed init must never crash the game - we
// just stay uninited and retry on the next Present. (POD-only locals so __try is legal.)
static bool seh_init_dx12(IDXGISwapChain3 *sc)
{
    __try { return init_dx12(sc); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
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
// hard allocation failure. Plain (no SEH) - the caller wraps it.
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

// ── Dev Icon Preview helpers ──────────────────────────────────────────────
// Open the native file dialog on a WORKER thread so the modal dialog never blocks the render/present
// thread. On success, stash the path and flag the render thread to decode it next frame.
static void open_preview_dialog()
{
    if (g_preview_dialog_open.exchange(true))
        return; // a dialog is already up
    std::thread([] {
        wchar_t buf[MAX_PATH] = {0};
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.lpstrFilter = L"PNG images\0*.png\0All files\0*.*\0";
        ofn.lpstrFile = buf;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrTitle = L"Map for Goblins - pick a PNG to preview";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
        if (GetOpenFileNameW(&ofn))
        {
            std::lock_guard<std::mutex> lk(g_preview_mx);
            g_preview_path.assign(buf);
            g_preview_dirty.store(true);
        }
        g_preview_dialog_open.store(false);
    }).detach();
}

// Mirror the build pipeline's generate_map_icons.normalize(): crop to the alpha bbox, fit to SIZE px
// preserving aspect, center on a transparent SIZE x SIZE canvas. The on-map icon IS this 96px image
// (not the raw png), so previewing the normalized version is what makes preview == map.
static std::vector<unsigned char> to_map_icon(const unsigned char *src, int w, int h, int SIZE,
                                              int *out_w, int *out_h)
{
    // 1) Scale the WHOLE source to fit SIZE px (longest side) FIRST - alpha-aware (STBIR_RGBA is
    //    premult-aware), so the drawn size within the source canvas is preserved (matches the map).
    float s = (w >= h) ? (float)SIZE / w : (float)SIZE / h;
    int fw = (int)(w * s + 0.5f), fh = (int)(h * s + 0.5f);
    if (fw < 1) fw = 1; if (fw > SIZE) fw = SIZE;
    if (fh < 1) fh = 1; if (fh > SIZE) fh = SIZE;
    std::vector<unsigned char> fit((size_t)fw * fh * 4);
    stbir_resize_uint8_srgb(src, w, h, 0, fit.data(), fw, fh, 0, STBIR_RGBA);
    // 2) Crop to the alpha bbox AFTER scaling (tight, variable W x H) - mirrors generate_map_icons.normalize.
    int x0 = fw, y0 = fh, x1 = -1, y1 = -1;
    for (int y = 0; y < fh; ++y)
        for (int x = 0; x < fw; ++x)
            if (fit[((size_t)y * fw + x) * 4 + 3] > 8)
            {
                if (x < x0) x0 = x; if (x > x1) x1 = x;
                if (y < y0) y0 = y; if (y > y1) y1 = y;
            }
    if (x1 < x0) { x0 = 0; y0 = 0; x1 = fw - 1; y1 = fh - 1; } // fully transparent -> whole image
    int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
    std::vector<unsigned char> out((size_t)cw * ch * 4);
    for (int y = 0; y < ch; ++y)
        memcpy(&out[(size_t)y * cw * 4], &fit[(((size_t)(y0 + y)) * fw + x0) * 4], (size_t)cw * 4);
    *out_w = cw; *out_h = ch;
    return out;
}

// Render-thread: if a path was picked, read (wide path) + decode the PNG and (re)upload it to SRV slot 3.
static void maybe_load_preview()
{
    if (!g_preview_dirty.exchange(false))
        return;
    std::wstring path;
    { std::lock_guard<std::mutex> lk(g_preview_mx); path = g_preview_path; }
    if (path.empty())
        return;
    // stb has no wide fopen and STBI_NO_STDIO is set, so read the bytes with a wide-path Win32 call.
    HANDLE hf = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE)
    {
        spdlog::warn("[preview] cannot open picked file");
        return;
    }
    LARGE_INTEGER sz{};
    std::vector<unsigned char> file;
    if (GetFileSizeEx(hf, &sz) && sz.QuadPart > 0 && sz.QuadPart < (64 << 20))
    {
        file.resize(static_cast<size_t>(sz.QuadPart));
        DWORD got = 0;
        if (!ReadFile(hf, file.data(), static_cast<DWORD>(file.size()), &got, nullptr) || got != file.size())
            file.clear();
    }
    CloseHandle(hf);
    if (file.empty())
        return;
    int w = 0, h = 0, n = 0;
    unsigned char *px = stbi_load_from_memory(file.data(), static_cast<int>(file.size()), &w, &h, &n, 4);
    if (!px)
    {
        const char *why = stbi_failure_reason();
        spdlog::warn("[preview] PNG decode failed: {}", why ? why : "?");
        return;
    }
    constexpr int SIZE = 96; // must match generate_map_icons.SIZE (the on-map icon canvas-fit size)
    int iw = 0, ih = 0;
    std::vector<unsigned char> icon = to_map_icon(px, w, h, SIZE, &iw, &ih); // tight, variable iw x ih
    if (g_preview_tex) { g_preview_tex->Release(); g_preview_tex = nullptr; }
    if (upload_rgba(icon.data(), iw, ih, 3, &g_preview_tex, &g_preview_gpu))
    {
        g_preview_w = w; g_preview_h = h;     // SOURCE dims (info line)
        g_preview_iw = iw; g_preview_ih = ih; // normalized dims (proportional draw)
        g_preview_show.store(true);
        spdlog::info("[preview] loaded source {}x{} -> normalized {}x{} icon", w, h, iw, ih);
    }
    stbi_image_free(px);
}

// Floating, transparent, DRAGGABLE window showing the normalized icon (square 96px texture) at
// g_preview_px. Centered on first show; drag the icon itself to move it (an invisible button over the
// image captures the drag, since the image is otherwise an inert item).
static void draw_preview_window()
{
    if (!g_preview_show.load() || !g_preview_tex || !g_preview_gpu.ptr)
        return;
    const ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f)); // FirstUseEver: keep where dragged
    ImGui::SetNextWindowBgAlpha(0.0f);
    const ImGuiWindowFlags fl = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_AlwaysAutoResize;
    if (ImGui::Begin("##goblin_icon_preview", nullptr, fl))
    {
        // Texture is the TIGHT crop (variable iw x ih). g_preview_px = on-screen size of a FULL-canvas
        // (96px) glyph; draw this glyph at iw/96 x ih/96 of that, so the preview reflects the drawn size +
        // aspect exactly like the map (a glyph drawn smaller in the canvas shows smaller here too).
        const float sc = g_preview_px / 96.0f;
        const float dw = g_preview_iw * sc, dh = g_preview_ih * sc;
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::Image(reinterpret_cast<ImTextureID>(static_cast<uintptr_t>(g_preview_gpu.ptr)), ImVec2(dw, dh));
        ImGui::SetCursorScreenPos(at);
        ImGui::InvisibleButton("##pv_drag", ImVec2(dw, dh)); // catch drags over the image to move the window
        if (ImGui::IsItemActive())
            ImGui::SetWindowPos(ImVec2(ImGui::GetWindowPos().x + io.MouseDelta.x,
                                       ImGui::GetWindowPos().y + io.MouseDelta.y));
    }
    ImGui::End();
}

// Build the overlay's category-icon atlas RGBA AT RUNTIME by decoding the SHARED DefineBitsLossless2
// icon tags (goblin::generated::MAP_ICON_TAGS - the same source the map injects) into each cell. This
// is why no ATLAS_RGBA is embedded: map and menu draw from one icon source. Each tag is premultiplied
// ARGB [A,R',G',B'] (fmt5, zlib); we inflate, box-downscale into the cell (in premult space), then
// un-premultiply to the straight RGBA ImGui wants. Returns the atlas buffer (empty on failure).
static std::vector<unsigned char> build_atlas_rgba()
{
    using namespace goblin::overlay_icons;
    namespace gen = goblin::generated;
    const int AW = ATLAS_W, AH = ATLAS_H, C = CELL, COLS = (C > 0 ? AW / C : 1);
    std::vector<unsigned char> atlas((size_t)AW * AH * 4, 0); // transparent
    for (int ci = 0; ci < ATLAS_CELL_COUNT; ++ci)
    {
        int srcIcon = CELL_SRC_ICON[ci];
        const gen::MapIconTag *tag = nullptr;
        for (int k = 0; k < gen::MAP_ICON_TAG_COUNT; ++k)
            if (gen::MAP_ICON_TAGS[k].srcIconId == srcIcon) { tag = &gen::MAP_ICON_TAGS[k]; break; }
        if (!tag || tag->tagLen < 8) continue;
        const unsigned char *b = tag->tag;
        int w = b[3] | (b[4] << 8), h = b[5] | (b[6] << 8);
        if (w <= 0 || h <= 0 || w > 1024 || h > 1024) continue;
        std::vector<unsigned char> px((size_t)w * h * 4);
        mz_ulong destlen = (mz_ulong)px.size();
        if (mz_uncompress(px.data(), &destlen, b + 7, (mz_ulong)(tag->tagLen - 7)) != MZ_OK ||
            destlen != px.size())
            continue;
        const int cx = (ci % COLS) * C, cy = (ci / COLS) * C;
        // Tags are now cropped TIGHT (variable, often non-square). LETTERBOX into the square cell so the
        // aspect is preserved (a square stretch would distort): fit W x H into C x C, center the result.
        double fit = ((double)C / w < (double)C / h) ? (double)C / w : (double)C / h;
        int dw = (int)(w * fit + 0.5); if (dw < 1) dw = 1; if (dw > C) dw = C;
        int dh = (int)(h * fit + 0.5); if (dh < 1) dh = 1; if (dh > C) dh = C;
        const int ox = (C - dw) / 2, oy = (C - dh) / 2;
        for (int dy = 0; dy < dh; ++dy)
            for (int dx = 0; dx < dw; ++dx)
            {
                int sx0 = dx * w / dw, sx1 = (dx + 1) * w / dw; if (sx1 <= sx0) sx1 = sx0 + 1;
                int sy0 = dy * h / dh, sy1 = (dy + 1) * h / dh; if (sy1 <= sy0) sy1 = sy0 + 1;
                unsigned long sA = 0, sR = 0, sG = 0, sB = 0, n = 0;
                for (int sy = sy0; sy < sy1 && sy < h; ++sy)
                    for (int sx = sx0; sx < sx1 && sx < w; ++sx)
                    {
                        const unsigned char *s = &px[((size_t)sy * w + sx) * 4];
                        sA += s[0]; sR += s[1]; sG += s[2]; sB += s[3]; ++n;
                    }
                if (!n) continue;
                unsigned A = (unsigned)(sA / n), Rp = (unsigned)(sR / n),
                         Gp = (unsigned)(sG / n), Bp = (unsigned)(sB / n);
                unsigned R = A ? (Rp * 255 + A / 2) / A : 0; if (R > 255) R = 255;
                unsigned G = A ? (Gp * 255 + A / 2) / A : 0; if (G > 255) G = 255;
                unsigned Bb = A ? (Bp * 255 + A / 2) / A : 0; if (Bb > 255) Bb = 255;
                unsigned char *d = &atlas[((size_t)(cy + oy + dy) * AW + (cx + ox + dx)) * 4];
                d[0] = (unsigned char)R; d[1] = (unsigned char)G; d[2] = (unsigned char)Bb; d[3] = (unsigned char)A;
            }
    }
    return atlas;
}

// SEH-isolated D3D12 uploads (NO C++ objects here - __try forbids object unwinding). `atlas` may be
// null (then only the logo is uploaded).
static void upload_atlas_and_logo(const unsigned char *atlas)
{
    using namespace goblin::overlay_icons;
    __try
    {
        if (atlas)
            upload_rgba(atlas, ATLAS_W, ATLAS_H, 1, &g_atlas_tex, &g_atlas_gpu);
        upload_rgba(LOGO_RGBA, LOGO_W, LOGO_H, 2, &g_logo_tex, &g_logo_gpu);
        g_atlas_ready = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        g_atlas_ready = true; // give up rather than crash/retry
    }
}

// One-time upload of the category-icon atlas (SRV index 1) + the mod logo (index 2). The atlas is built
// at runtime from the shared lossless tags (build_atlas_rgba), so map + menu share one embedded source.
void try_upload_atlas()
{
    if (g_atlas_ready || !g_dx12_inited || !g_command_queue || !g_device || !g_srv_heap)
        return;
    std::vector<unsigned char> atlas = build_atlas_rgba(); // pure memory; no SEH needed
    upload_atlas_and_logo(atlas.empty() ? nullptr : atlas.data());
    spdlog::info("[OVERLAY] atlas {}x{} built from {} shared tags + logo {}x{}",
                 goblin::overlay_icons::ATLAS_W, goblin::overlay_icons::ATLAS_H,
                 goblin::overlay_icons::ATLAS_CELL_COUNT,
                 goblin::overlay_icons::LOGO_W, goblin::overlay_icons::LOGO_H);
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
    maybe_load_preview(); // dev: decode + upload a freshly-picked preview PNG (before NewFrame)
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
    draw_preview_window(); // dev: floating centered icon preview (transparent, passive)
    ImGui::Render();
    submit_frame(sc);
}

// ── Hooks ──
HRESULT WINAPI hkPresent(IDXGISwapChain3 *sc, UINT sync, UINT flags)
{
    goblin::map_timing::on_present(); // diagnostic: render-thread sampling profiler (needs the present thread)
    // (gfx_probe's self-heal + diagnostics moved to the background watcher thread - see gfx_probe::tick -
    //  so they run regardless of enable_overlay; injection itself is load-hook driven, also independent.)

    // Open/close on the toggle key (keyboard) OR the gamepad combo. (When the
    // overlay is DISABLED these same inputs master-toggle icons instead, handled
    // in goblin_inject's toggle_hotkey_loop - see its master_mode gate.)
    // While rebinding a hotkey in the overlay, ignore these inputs so binding
    // F10 / Y+R3 / Esc / B doesn't also open/close the menu.
    // Only react to hotkeys when the game window is foreground - GetAsyncKeyState is
    // global, so without this an Esc/F10 pressed in another app while alt-tabbed
    // would close/toggle our menu.
    const bool focused = game_focused();
    if (focused)
        refresh_gamepad_state();
    if (!g_menu_open.load() && g_rebind_mode.load() != 0)
        reset_rebind_state();
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
        capture_swallow_buttons();
        reset_rebind_state();
        goblin::save_config(goblin::g_ini_path);
    }
    prev_open = open_now;

    if (!g_dx12_inited)
    {
        if (!seh_init_dx12(sc))
            return oPresent(sc, sync, flags);
    }
    if (g_menu_open.load() && g_command_queue)
        render(sc); // its GPU submit (submit_frame) is SEH-guarded; ImGui CPU draw runs on a consistent
                    // inited state (init/teardown are serial on this same thread)

    return oPresent(sc, sync, flags);
}

// SEH-isolated teardown (POD-only; releasing D3D objects can AV if the swapchain/device is mid-transition,
// e.g. G-Sync / fullscreen-flip). Must never crash the game - on fault we just drop our state.
static void seh_resize_teardown()
{
    __try
    {
        wait_gpu();      // flush our in-flight work so freeing the RTVs is safe (AMD/Deck)
        teardown_dx12(); // drop our RTV refs so the resize can free the buffers
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        g_dx12_inited = false; // force a clean re-init on the next Present
    }
}

HRESULT WINAPI hkResizeBuffers(IDXGISwapChain3 *sc, UINT bc, UINT w, UINT h,
                               DXGI_FORMAT fmt, UINT flags)
{
    seh_resize_teardown();
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
        goblin::diag::set_overlay(goblin::diag::OverlayState::OffByConfig, "");
        return;
    }
    void *present = nullptr, *resize = nullptr, *execcl = nullptr;
    if (!capture_vtables(present, resize, execcl))
    {
        spdlog::error("[OVERLAY] failed to capture DX12 vtables; overlay disabled");
        goblin::diag::set_overlay(goblin::diag::OverlayState::Failed, "DX12 vtable capture failed");
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
        if (!xfn) // none loaded yet - pull one in
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
