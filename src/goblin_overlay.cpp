// In-game config overlay: Dear ImGui drawn in a SEPARATE, INDEPENDENT transparent
// top-most window with its own D3D11 + DirectComposition device, on its own thread.
//
// Why a separate window (not a swapchain hook): we no longer touch the game's DXGI
// swapchain at all. That makes the overlay compatible with tools that wrap the
// game's swapchain (Special K, NVIDIA Smooth Motion / driver frame-gen, ReShade) -
// the old Present/ExecuteCommandLists hook fought those wrappers and crashed.
//
// Architecture:
//   - setup() spawns a detached overlay thread that creates the window + D3D11 +
//     DComp + ImGui DX11 backend and runs the render loop. setup() returns fast.
//   - The window is WS_POPUP + WS_EX_LAYERED|TRANSPARENT|TOPMOST|NOACTIVATE|
//     TOOLWINDOW; transparency comes from DirectComposition (premultiplied alpha),
//     NOT UpdateLayeredWindow.
//   - Each loop iteration the overlay window is moved to exactly cover the game's
//     client area, so it sits over the game like a HUD.
//   - F10 (toggle key) + ESC are polled with GetAsyncKeyState (rising edge) to
//     open/close the menu. While CLOSED the window is click-through (WS_EX_TRANSPARENT)
//     and renders nothing; while OPEN it is interactive and ImGui draws the panel.
//   - Input is our OWN WndProc -> ImGui_ImplWin32_WndProcHandler (mouse + keyboard).
//     Optional gamepad nav is polled directly via XInputGetState (no hook).
//
// All UI drawing code (draw_settings_window / tabs / rebind / preview) is backend
// -agnostic and reused verbatim; only the texture path is now D3D11 SRVs.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <dcomp.h>
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
#include "goblin_progress.hpp"
#include "goblin_diag.hpp"
#include "modutils.hpp" // hook GetRawInputData (menu input-leak block)

#include <spdlog/spdlog.h>
#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
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

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "comdlg32.lib")

// From imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                                             WPARAM wParam, LPARAM lParam);

namespace
{
using XInputGetState_t = DWORD(WINAPI *)(DWORD, XINPUT_STATE *);
XInputGetState_t pXInputGetState = nullptr; // resolved once on the overlay thread
XInputGetState_t o_XInputGetState = nullptr; // trampoline: OUR poll reads the real pad
                                             // through this; the hook feeds the GAME a
                                             // disconnected pad while the menu is open.

// ── Our own window + D3D11 + DirectComposition state (overlay thread only) ──
const wchar_t *OVERLAY_CLASS = L"MFG_OverlayWindow";
HWND g_hwnd = nullptr;          // our overlay window
HWND g_game_hwnd = nullptr;     // tracked game main window (for cover + focus return)

ID3D11Device *g_d3d_device = nullptr;
ID3D11DeviceContext *g_d3d_ctx = nullptr;
IDXGISwapChain1 *g_swapchain = nullptr; // composition swapchain (B8G8R8A8, premult alpha)
ID3D11RenderTargetView *g_rtv = nullptr;
IDCompositionDevice *g_dcomp_device = nullptr;
IDCompositionTarget *g_dcomp_target = nullptr;
IDCompositionVisual *g_dcomp_visual = nullptr;
UINT g_back_w = 0, g_back_h = 0; // current swapchain back-buffer size
// Proton/Wine fallback: DirectComposition (CreateSwapChainForComposition) returns
// E_NOTIMPL under Wine, so there we present via a WS_EX_LAYERED window fed by
// UpdateLayeredWindow (render ImGui to an offscreen RT -> CPU readback -> per-pixel
// alpha blit). Windows keeps the DComp path unchanged. g_use_layered selects the path.
bool g_use_layered = false;
// Render mode is chosen by ini `overlay_render_mode`:
//   layered  = WS_EX_LAYERED + UpdateLayeredWindow (GDI, NO DXGI swapchain -> invisible to
//              swapchain hooks: OBS Game Capture / ReShade / RivaTuner-RTSS / Special K). Default.
//   surface  = DirectComposition SURFACE (GPU-composited, still NO swapchain -> same compat, no
//              CPU readback). Uses g_dcomp_surface + an intermediate full-size RT (g_surf_tex).
//   swapchain= DirectComposition composition SWAPCHAIN (lightest, but that swapchain gets grabbed
//              by those tools -> capture/FPS-limit/transparency conflicts). g_swapchain path.
// A GPU path that fails at init (e.g. Wine E_NOTIMPL) falls back to layered.
enum class RenderMode { Layered, Surface, Swapchain };
RenderMode g_render_mode = RenderMode::Layered;
bool g_use_surface = false;                        // surface mode active (post-init)
IDCompositionSurface *g_dcomp_surface = nullptr;   // 'surface' mode content (no swapchain)
ID3D11Texture2D *g_surf_tex = nullptr;             // surface mode: intermediate full-size RT
ID3D11RenderTargetView *g_surf_rtv = nullptr;      // surface mode: RTV for g_surf_tex
ID3D11Texture2D *g_ltex = nullptr;        // offscreen render target (B8G8R8A8)
ID3D11RenderTargetView *g_lrtv = nullptr; // RTV for g_ltex
ID3D11Texture2D *g_lstaging = nullptr;    // CPU-readable copy of g_ltex
HDC g_lmemdc = nullptr;                    // memory DC holding g_ldib
HBITMAP g_ldib = nullptr;                  // 32bpp top-down DIB for UpdateLayeredWindow
void *g_ldibbits = nullptr;               // g_ldib pixel buffer

// Texture + SRV trio for each UI image. ImGui ImTextureID = the SRV pointer.
ID3D11Texture2D *g_atlas_tex = nullptr;   // category-icon atlas
ID3D11ShaderResourceView *g_atlas_srv = nullptr;
ID3D11Texture2D *g_logo_tex = nullptr;    // mod logo
ID3D11ShaderResourceView *g_logo_srv = nullptr;
bool g_atlas_ready = false;

// Dev "Icon Preview" (Debug tab): pick a PNG off disk, show it floating + centered with a transparent
// background at a chosen on-map size, so icon art can be eyeballed against the live map without a rebuild.
ID3D11Texture2D *g_preview_tex = nullptr;
ID3D11ShaderResourceView *g_preview_srv = nullptr;
int g_preview_w = 0, g_preview_h = 0;          // source png dimensions
int g_preview_iw = 0, g_preview_ih = 0;        // normalized (fit-to-96 then cropped) dims, for proportional draw
std::atomic<bool> g_preview_show{false};       // floating preview window visible
float g_preview_px = 65.0f;                    // on-screen height in px (slider; ~map-icon default)
std::mutex g_preview_mx;
std::wstring g_preview_path;                    // picked path (set by the dialog thread)
std::atomic<bool> g_preview_dirty{false};      // a new path is waiting to be decoded (render thread)
std::atomic<bool> g_preview_dialog_open{false}; // a file dialog is already up (avoid stacking)
static void open_preview_dialog(); // defined below (opens the file dialog on a worker thread)

std::atomic<bool> g_running{false}; // overlay thread alive (teardown guard)
std::atomic<bool> g_menu_open{false};
// Open/close + rebind use GetAsyncKeyState polling (the game keeps focus -> reliable +
// symmetric). Key leak into the game while the menu is open is blocked by a hook on
// GetRawInputData (see hk_GetRawInputData). We never steal the game's focus.
bool g_context_inited = false; // ImGui context + win32 backend created
bool g_d3d_inited = false;     // D3D11 + DComp + ImGui dx11 backend created
std::vector<char> g_dump_buf;  // Tools tab: last marker-dump text (copyable)

// Captured active-controller state for ImGui gamepad nav (polled, not hooked).
XINPUT_GAMEPAD g_pad{};
bool g_pad_ok = false;

// In-overlay hotkey rebind. mode: 0=idle, 1=capturing a keyboard key, 2=capturing
// a gamepad combo. While != 0, the open/close + ESC inputs are ignored so binding
// those keys doesn't act on the menu.
std::atomic<int> g_rebind_mode{0};
void *g_rebind_target = nullptr;          // config var being rebound (render thread)
std::atomic<uint32_t> g_captured_vk{0};   // first VK seen during rebind
std::atomic<bool> g_captured_up{false};   // that key was released -> safe to commit (no auto-trigger)
uint16_t g_rebind_pad_accum = 0;          // gamepad buttons accumulated this rebind

// Last input device, for control hints: 0 = keyboard/mouse, 1 = gamepad.
std::atomic<int> g_last_input{0};

// Hotkey state read straight from the OS (callers gate on window focus). Read
// GetAsyncKeyState directly - it worked for years; a key-state table missed F10.
inline bool kd(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

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
    if (ic && g_atlas_srv)
    {
        const ImVec2 uv0((ic->col * CELL) / static_cast<float>(ATLAS_W),
                         (ic->row * CELL) / static_cast<float>(ATLAS_H));
        const ImVec2 uv1(((ic->col + 1) * CELL) / static_cast<float>(ATLAS_W),
                         ((ic->row + 1) * CELL) / static_cast<float>(ATLAS_H));
        ImGui::Image(reinterpret_cast<ImTextureID>(g_atlas_srv), ImVec2(ICON_SZ, ICON_SZ), uv0, uv1);
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

void reset_rebind_state()
{
    g_rebind_mode.store(0);
    g_rebind_target = nullptr;
    g_captured_vk.store(0);
    g_captured_up.store(false);
    g_rebind_pad_accum = 0;
}

// Poll the keyboard for a rebind capture (render thread). Scans the VK range and
// records the first key down, then flags it released - mirrors the old raw-input
// capture but via GetAsyncKeyState so we keep one input path.
void poll_rebind_keyboard()
{
    if (g_rebind_mode.load() != 1)
        return;
    const uint32_t cur = g_captured_vk.load();
    if (cur == 0)
    {
        for (int vk = 0x08; vk <= 0xFE; ++vk)
        {
            if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON ||
                vk == VK_SPACE || vk == VK_RETURN) // Space/Enter activate the rebind button via nav
                continue;
            if (kd(vk)) { g_captured_vk.store(static_cast<uint32_t>(vk)); break; }
        }
    }
    else if (!kd(static_cast<int>(cur)))
    {
        g_captured_up.store(true);
    }
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
            if (std::strcmp(e.key, "overlay_font_scale") == 0 ||
                std::strcmp(e.key, "overlay_opacity") == 0)
                continue; // shown as prominent sliders at the top of the Settings tab
            if (std::strncmp(e.key, "overlay_window_", 15) == 0)
                continue; // auto-managed window geometry (saved on close) - not a UI control
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
            else if (e.type == goblin::IniType::Language)
            {
                std::string &value = *static_cast<std::string *>(e.target);
                const char *preview = tr::language_preview_label(value, lang);
                if (ImGui::BeginCombo(label, preview))
                {
                    const char *options[] = {"auto", "english", "schinese", "tchinese", "korean"};
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

    // Overlay text size (QoL for 4K / high-DPI). Scales all overlay text live via
    // io.FontGlobalScale (applied each frame in render()); persisted to the
    // overlay_font_scale ini key by the auto-save on close. Rendered here (not via
    // draw_section) so it is easy to find; draw_section skips the schema entry.
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
    ImGui::SliderFloat("Overlay text size", &goblin::config::fontScale, 0.8f, 3.0f,
                       "%.2fx", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("Scales the overlay menu text. Raise it on 4K / high-DPI screens.");

    // Overlay panel opacity (window bg alpha). Persisted to overlay_opacity on close.
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
    ImGui::SliderFloat("Overlay opacity", &goblin::config::overlayOpacity, 0.3f, 1.0f,
                       "%.2f", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("Menu panel transparency. Lower it to see more of the map behind the menu.");
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

    // ── Map icon status (top of the tab) ───────────────────────────────────
    // A live readout of every load step (hooks, world-map icons, bitmaps, remap,
    // logo, overlay, + a live heap-pointer sample) with a reason for anything that
    // failed. Players can screenshot or Copy this for a bug report so we can
    // pinpoint the fault (incl. pointer-range issues like the looks_heap bug).
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
        ImGui::BeginChild("##exp", ImVec2(-1, -1), true, ImGuiWindowFlags_HorizontalScrollbar);
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
    if (g_preview_srv)
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
    if (g_atlas_ready && goblin::overlay_icons::LOGO_W > 0 && g_logo_srv)
    {
        const float lh = 120.0f;
        const float lw = lh * goblin::overlay_icons::LOGO_W /
                         static_cast<float>(goblin::overlay_icons::LOGO_H);
        ImGui::Image(reinterpret_cast<ImTextureID>(g_logo_srv), ImVec2(lw, lh));
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

// Progress-bar fill: dark yellow (menu-frame tone), dark green at 100%.
static const ImVec4 kBarYellow(0.60f, 0.48f, 0.16f, 1.0f);
static const ImVec4 kBarGreen(0.28f, 0.46f, 0.20f, 1.0f);
static inline ImVec4 progress_bar_color(float frac)
{
    return frac >= 1.0f ? kBarGreen : kBarYellow;
}

// One clickable category row: full-width bar (dark-yellow / dark-green at 100%)
// with the category name on the left and "collected/total" on the right, drawn
// over a Selectable so the WHOLE bar is clickable. Clicking toggles map "focus"
// on that category (show only its uncollected markers). Returns true if clicked.
static bool draw_category_bar(const char *name, int collected, int total,
                              bool focused, const char *tooltip)
{
    const float frac = total ? static_cast<float>(collected) / total : 0.0f;
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = ImGui::GetFrameHeight();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + w, p0.y + h);

    // Invisible full-row hit target (no default bg; we paint our own).
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
    const bool clicked = ImGui::Selectable("##catbar", false, ImGuiSelectableFlags_None, ImVec2(w, h));
    ImGui::PopStyleColor(3);
    const bool hovered = ImGui::IsItemHovered();
    if (hovered && tooltip) ImGui::SetTooltip("%s", tooltip);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float rounding = 3.0f;
    // Track background + fill.
    dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImGuiCol_FrameBg), rounding);
    if (frac > 0.0f)
    {
        ImVec4 fc = progress_bar_color(frac);
        if (hovered) { fc.x *= 1.25f; fc.y *= 1.25f; fc.z *= 1.25f; }  // brighten on hover
        dl->AddRectFilled(p0, ImVec2(p0.x + w * frac, p1.y), ImGui::GetColorU32(fc), rounding);
    }
    // Border: brighter/gold when this category is the active map focus.
    const ImU32 border = focused ? ImGui::GetColorU32(ImVec4(0.90f, 0.78f, 0.35f, 1.0f))
                                  : ImGui::GetColorU32(ImGuiCol_Border);
    dl->AddRect(p0, p1, border, rounding, 0, focused ? 2.0f : 1.0f);

    // Labels: name left, count right, vertically centred.
    const ImU32 txt = ImGui::GetColorU32(ImGuiCol_Text);
    const float ty = p0.y + (h - ImGui::GetFontSize()) * 0.5f;
    dl->AddText(ImVec2(p0.x + 6.0f, ty), txt, name);
    char cnt[32];
    std::snprintf(cnt, sizeof(cnt), "%d/%d", collected, total);
    const float cw = ImGui::CalcTextSize(cnt).x;
    dl->AddText(ImVec2(p1.x - cw - 6.0f, ty), txt, cnt);
    return clicked;
}

// ── Progress tab: per-region collected/total, broken down by category ──
// Groups every baked marker into ~22 coarse regions (goblin_progress) and shows
// an overall bar per region + a clickable bar per enabled category. Clicking a
// category isolates its uncollected markers on the live map (goblin focus mode).
void draw_progress_tab()
{
    namespace tr = goblin::i18n;
    const tr::Language lang = tr::current_language();
    namespace prog = goblin::progress;

    prog::rebuild_if_stale(ImGui::GetTime());
    const auto &regions = prog::snapshot();

    ImGui::TextDisabled("%s", tr::tr(tr::TextId::ProgressHint, lang));
    ImGui::TextDisabled("%s", tr::tr(tr::TextId::ProgressClickHint, lang));

    // Active focus is a (category, region) pair.
    const int focusCat = goblin::focus_category();
    const int32_t focusReg = goblin::focus_region();
    // Active-focus banner: which category+region the map is currently isolating.
    if (focusCat >= 0)
    {
        const char *regName = "?";
        for (const auto &r : regions)
            if (r.place_name_id == focusReg) { regName = r.name.c_str(); break; }
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.78f, 0.35f, 1.0f));
        ImGui::TextWrapped("%s %s - %s", tr::tr(tr::TextId::ProgressShowingOnly, lang),
                           goblin::markers::category_name(
                               static_cast<goblin::generated::Category>(focusCat)),
                           regName);
        ImGui::PopStyleColor();
        if (ImGui::SmallButton(tr::tr(tr::TextId::ProgressFocusClear, lang)))
        {
            goblin::set_focus_category(-1);
            goblin::reapply_live_settings();
        }
    }
    ImGui::Separator();

    // Clicking a category defers the focus change until after the draw loop.
    int toggle_cat = -2;         // -2 = no change this frame
    int32_t toggle_reg = -1;

    ImGui::BeginChild("##progscroll", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
    bool any = false;
    for (const auto &rp : regions)
    {
        // Region-wide visible totals: only categories currently shown (enabled)
        // and present in this region. Toggling a category needs no recompute.
        int vis_coll = 0, vis_tot = 0;
        for (int ci = 0; ci < prog::kCategoryCount; ++ci)
        {
            if (rp.cats[ci].total <= 0) continue;
            if (!goblin::category_enabled(static_cast<goblin::generated::Category>(ci)))
                continue;
            vis_coll += rp.cats[ci].collected;
            vis_tot += rp.cats[ci].total;
        }
        if (vis_tot == 0) continue;  // nothing enabled here
        any = true;

        // ###id keeps the collapse state stable as the counts in the label change.
        char header[256];
        std::snprintf(header, sizeof(header), "%s  (%d/%d)###reg_%d",
                      rp.name.c_str(), vis_coll, vis_tot, rp.place_name_id);
        if (!ImGui::CollapsingHeader(header))
            continue;

        const float rfrac = vis_tot ? static_cast<float>(vis_coll) / vis_tot : 0.0f;
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, progress_bar_color(rfrac));
        ImGui::ProgressBar(rfrac, ImVec2(-1.0f, 0.0f));
        ImGui::PopStyleColor();
        ImGui::Spacing();
        for (int ci = 0; ci < prog::kCategoryCount; ++ci)
        {
            const auto cat = static_cast<goblin::generated::Category>(ci);
            if (rp.cats[ci].total <= 0 || !goblin::category_enabled(cat))
                continue;
            const bool focused = (focusCat == ci && focusReg == rp.place_name_id);
            ImGui::PushID(rp.place_name_id * 128 + ci);
            if (draw_category_bar(goblin::markers::category_name(cat),
                                  rp.cats[ci].collected, rp.cats[ci].total,
                                  focused, tr::tr(tr::TextId::ProgressClickHint, lang)))
            {
                toggle_cat = ci;
                toggle_reg = rp.place_name_id;
            }
            ImGui::PopID();
        }
    }
    if (!any)
        ImGui::TextDisabled("(no markers to show - enable a category on the Settings tab)");
    ImGui::EndChild();

    if (toggle_cat != -2)
    {
        const bool same = (focusCat == toggle_cat && focusReg == toggle_reg);
        if (same)
            goblin::set_focus_category(-1);
        else
            goblin::set_focus_category(toggle_cat, toggle_reg);
        goblin::reapply_live_settings();  // apply the isolate/restore on the live map
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

    // Restore saved geometry (once per session, on first open). overlayWinW<=0 = unset
    // -> fall back to the default size/position. Live moves/resizes are captured before
    // End() below and persisted to the ini on close.
    // Restore saved geometry (once per session, on first open). X = window CENTER as a
    // fraction of screen width, Y = window TOP as a fraction of screen height -> convert
    // to pixels here. A too-small W/H (stale/corrupt ini) falls back to the default size;
    // then clamp size to the screen + keep the window on-screen (resolution/aspect change).
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    {
        float w = goblin::config::overlayWinW, h = goblin::config::overlayWinH;
        if (w < 350.0f) w = 560.0f; // implausibly small -> restore default
        if (h < 250.0f) h = 680.0f;
        if (disp.x > 0 && w > disp.x) w = disp.x;
        if (disp.y > 0 && h > disp.y) h = disp.y;
        float x = goblin::config::overlayWinX * disp.x - w * 0.5f; // center-fraction -> left px
        float y = goblin::config::overlayWinY * disp.y;            // top-fraction -> top px
        const float xmax = disp.x > w ? disp.x - w : 0.0f;         // keep fully on-screen
        const float ymax = disp.y > 40.0f ? disp.y - 40.0f : 0.0f; // keep title bar reachable
        x = x < 0 ? 0 : (x > xmax ? xmax : x);
        y = y < 0 ? 0 : (y > ymax ? ymax : y);
        ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_FirstUseEver);
    }
    // Menu panel opacity (window background alpha), user-adjustable + persisted. Floor at
    // 0.1 so a hand-edited ini can never make the menu fully invisible/unclickable.
    float op = goblin::config::overlayOpacity;
    op = op < 0.1f ? 0.1f : (op > 1.0f ? 1.0f : op);
    ImGui::SetNextWindowBgAlpha(op);
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
        if (rb && !prev_rb) { cur_tab = (cur_tab + 1) % 4; forced_tab = cur_tab; }
        if (lb && !prev_lb) { cur_tab = (cur_tab + 3) % 4; forced_tab = cur_tab; }
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
        if (ImGui::BeginTabItem(tr::tr(tr::TextId::TabProgress, lang), nullptr, tab_flag(1))) { draw_progress_tab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(tr::tr(tr::TextId::TabDebug, lang),    nullptr, tab_flag(2))) { draw_debug_tab();    ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(tr::tr(tr::TextId::TabAbout, lang),    nullptr, tab_flag(3))) { draw_about_tab();    ImGui::EndTabItem(); }

        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    forced_tab = -1;
    draw_control_hints();
    // Capture the current geometry so the auto-save-on-close persists it. Store X as the
    // window CENTER fraction, Y as the TOP fraction (resolution-independent); W/H in pixels.
    const ImVec2 wpos = ImGui::GetWindowPos(), wsize = ImGui::GetWindowSize();
    const ImVec2 d = ImGui::GetIO().DisplaySize;
    if (d.x > 0.0f) goblin::config::overlayWinX = (wpos.x + wsize.x * 0.5f) / d.x;
    if (d.y > 0.0f) goblin::config::overlayWinY = wpos.y / d.y;
    goblin::config::overlayWinW = wsize.x;
    goblin::config::overlayWinH = wsize.y;
    ImGui::End();
}

// ── Our own window proc: feed ImGui (mouse/keyboard/char), nothing else ──
LRESULT CALLBACK overlay_wndproc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return 0;
    switch (msg)
    {
    case WM_SETCURSOR:
        // Hide the OS cursor over our client area (we draw ImGui's software cursor). Our
        // window is a background window - without this the OS shows the IDC_APPSTARTING
        // "background busy" cursor next to it. Handling it ourselves suppresses that.
        if (LOWORD(lParam) == HTCLIENT)
        {
            SetCursor(nullptr);
            return TRUE;
        }
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    case WM_DESTROY:
        return 0;
    default:
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
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

// Upload one RGBA8 image to an immutable D3D11 texture + create its SRV. Returns
// false on a hard allocation failure. ImGui ImTextureID = the returned SRV ptr.
static bool upload_rgba(const unsigned char *rgba, int w, int h,
                        ID3D11Texture2D **out_tex, ID3D11ShaderResourceView **out_srv)
{
    if (!g_d3d_device || w <= 0 || h <= 0)
        return false;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(w);
    td.Height = static_cast<UINT>(h);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA srd{};
    srd.pSysMem = rgba;
    srd.SysMemPitch = static_cast<UINT>(w) * 4;
    ID3D11Texture2D *tex = nullptr;
    if (FAILED(g_d3d_device->CreateTexture2D(&td, &srd, &tex)) || !tex)
        return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView *srv = nullptr;
    if (FAILED(g_d3d_device->CreateShaderResourceView(tex, &sd, &srv)) || !srv)
    {
        tex->Release();
        return false;
    }
    *out_tex = tex;
    *out_srv = srv;
    return true;
}

// Render-thread: if a path was picked, read (wide path) + decode the PNG and (re)upload it.
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
    if (g_preview_srv) { g_preview_srv->Release(); g_preview_srv = nullptr; }
    if (g_preview_tex) { g_preview_tex->Release(); g_preview_tex = nullptr; }
    if (upload_rgba(icon.data(), iw, ih, &g_preview_tex, &g_preview_srv))
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
    if (!g_preview_show.load() || !g_preview_srv)
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
        ImGui::Image(reinterpret_cast<ImTextureID>(g_preview_srv), ImVec2(dw, dh));
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

// One-time upload of the category-icon atlas + the mod logo to D3D11 textures/SRVs.
// The atlas is built at runtime from the shared lossless tags (build_atlas_rgba),
// so map + menu share one embedded icon source.
void try_upload_atlas()
{
    if (g_atlas_ready || !g_d3d_inited || !g_d3d_device)
        return;
    using namespace goblin::overlay_icons;
    std::vector<unsigned char> atlas = build_atlas_rgba();
    if (!atlas.empty())
        upload_rgba(atlas.data(), ATLAS_W, ATLAS_H, &g_atlas_tex, &g_atlas_srv);
    upload_rgba(LOGO_RGBA, LOGO_W, LOGO_H, &g_logo_tex, &g_logo_srv);
    g_atlas_ready = true; // mark done even on partial failure (don't retry every frame)
}

const goblin::overlay_icons::IconCell *find_icon_cell(const char *key)
{
    using namespace goblin::overlay_icons;
    for (int i = 0; i < ICON_CELL_COUNT; ++i)
        if (std::strcmp(ICON_CELLS[i].key, key) == 0)
            return &ICON_CELLS[i];
    return nullptr;
}

// ── Gamepad nav: poll directly (no hook). Picks the first active controller. ──
void poll_gamepad()
{
    g_pad_ok = false;
    // Read the REAL pad through the trampoline (o_) if we hooked XInputGetState; the hook
    // returns "disconnected" to the GAME while the menu is open, but not through o_.
    XInputGetState_t xget = o_XInputGetState ? o_XInputGetState : pXInputGetState;
    if (!xget)
        return;
    for (DWORD idx = 0; idx < XUSER_MAX_COUNT; ++idx)
    {
        XINPUT_STATE state{};
        if (xget(idx, &state) == ERROR_SUCCESS)
        {
            g_pad = state.Gamepad;
            g_pad_ok = true;
            break;
        }
    }
}

// Feed the polled gamepad to ImGui nav (mouse + keyboard come via the WndProc).
void feed_gamepad()
{
    if (!g_pad_ok)
        return;
    ImGuiIO &io = ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    const WORD bt = g_pad.wButtons;
    if (bt != 0 || g_pad.sThumbLX > 12000 || g_pad.sThumbLX < -12000 ||
        g_pad.sThumbLY > 12000 || g_pad.sThumbLY < -12000)
        g_last_input.store(1, std::memory_order_relaxed);
    // Mask the menu-toggle combo's buttons out of the nav feed (so the closing
    // press doesn't also activate the focused widget).
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
    const float lx = g_pad.sThumbLX / 32767.0f;
    const float ly = g_pad.sThumbLY / 32767.0f;
    constexpr float DZ = 0.35f;
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft,  lx < -DZ, lx < -DZ ? -lx : 0.0f);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, lx >  DZ, lx >  DZ ?  lx : 0.0f);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp,    ly >  DZ, ly >  DZ ?  ly : 0.0f);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown,  ly < -DZ, ly < -DZ ? -ly : 0.0f);
}

// Feed keyboard NAV keys to ImGui by polling (we never hold focus, so the Win32 backend
// never receives WM_KEYDOWN). Covers arrows/Tab/Enter/Space/PageUp/Down for menu nav;
// full text entry (chars) is not supported this way. These keys are also blocked from
// the game by the raw-input hook while the menu is open, so they do not double-act.
void feed_nav_keyboard()
{
    ImGuiIO &io = ImGui::GetIO();
    struct Map { int vk; ImGuiKey key; };
    static const Map maps[] = {
        {VK_UP, ImGuiKey_UpArrow},     {VK_DOWN, ImGuiKey_DownArrow},
        {VK_LEFT, ImGuiKey_LeftArrow}, {VK_RIGHT, ImGuiKey_RightArrow},
        {VK_RETURN, ImGuiKey_Enter},   {VK_SPACE, ImGuiKey_Space},
        {VK_TAB, ImGuiKey_Tab},        {VK_PRIOR, ImGuiKey_PageUp},
        {VK_NEXT, ImGuiKey_PageDown},
    };
    static bool prev[sizeof(maps) / sizeof(maps[0])] = {};
    for (size_t i = 0; i < sizeof(maps) / sizeof(maps[0]); ++i)
    {
        const bool down = kd(maps[i].vk);
        if (down != prev[i])
        {
            io.AddKeyEvent(maps[i].key, down);
            prev[i] = down;
            if (down)
                g_last_input.store(0, std::memory_order_relaxed); // keyboard active
        }
    }
}

// ── Game window tracking ──
// Best-effort: pick the foreground window if it belongs to eldenring.exe (our own
// process). Cache it so we keep covering it even after focus moves to our overlay.
HWND find_game_window()
{
    HWND fg = GetForegroundWindow();
    if (fg && fg != g_hwnd)
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        if (pid == GetCurrentProcessId())
        {
            // Skip tool windows (e.g. ours) - the game's main window is a normal top-level.
            const LONG ex = GetWindowLongW(fg, GWL_EXSTYLE);
            if (!(ex & WS_EX_TOOLWINDOW))
                g_game_hwnd = fg;
        }
    }
    return g_game_hwnd;
}

// Move/resize our overlay to exactly cover the game's client area. Returns the
// client size so the caller can resize the swapchain on change.
void cover_game_window(int &out_w, int &out_h)
{
    out_w = out_h = 0;
    HWND game = find_game_window();
    if (!game || !IsWindow(game))
        return;
    RECT cr{};
    if (!GetClientRect(game, &cr))
        return;
    POINT tl{cr.left, cr.top};
    ClientToScreen(game, &tl);
    const int w = cr.right - cr.left, h = cr.bottom - cr.top;
    if (w <= 0 || h <= 0)
        return;
    SetWindowPos(g_hwnd, HWND_TOPMOST, tl.x, tl.y, w, h, SWP_NOACTIVATE);
    out_w = w;
    out_h = h;
}

// ── Proton/Wine layered-window fallback helpers (used when DComp is E_NOTIMPL) ──
static void release_layered_targets()
{
    if (g_lrtv) { g_lrtv->Release(); g_lrtv = nullptr; }
    if (g_ltex) { g_ltex->Release(); g_ltex = nullptr; }
    if (g_lstaging) { g_lstaging->Release(); g_lstaging = nullptr; }
    if (g_lmemdc) { DeleteDC(g_lmemdc); g_lmemdc = nullptr; }
    if (g_ldib) { DeleteObject(g_ldib); g_ldib = nullptr; }
    g_ldibbits = nullptr;
}

// Offscreen RT (ImGui draws here) + a CPU-readable staging copy + a top-down 32bpp DIB
// that UpdateLayeredWindow blits from. ImGui's blend over a transparent RT yields
// PREMULTIPLIED BGRA, which is exactly what ULW_ALPHA wants -> a plain row copy, no math.
static bool create_layered_targets(UINT w, UINT h)
{
    release_layered_targets();
    if (!g_d3d_device || w == 0 || h == 0) return false;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(g_d3d_device->CreateTexture2D(&td, nullptr, &g_ltex)) || !g_ltex) return false;
    if (FAILED(g_d3d_device->CreateRenderTargetView(g_ltex, nullptr, &g_lrtv)) || !g_lrtv) return false;
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(g_d3d_device->CreateTexture2D(&td, nullptr, &g_lstaging)) || !g_lstaging) return false;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = static_cast<LONG>(w);
    bi.bmiHeader.biHeight = -static_cast<LONG>(h); // negative = top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(nullptr);
    g_ldib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &g_ldibbits, nullptr, 0);
    g_lmemdc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!g_ldib || !g_lmemdc || !g_ldibbits) return false;
    SelectObject(g_lmemdc, g_ldib);
    g_back_w = w; g_back_h = h;
    return true;
}

// DComp needs WS_EX_NOREDIRECTIONBITMAP (creation-only, cannot be removed); a layered
// window needs WS_EX_LAYERED. So on the Proton fallback we swap the window for a LAYERED one.
static void recreate_window_layered()
{
    if (g_hwnd) DestroyWindow(g_hwnd);
    const DWORD ex = WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
    g_hwnd = CreateWindowExW(ex, OVERLAY_CLASS, L"Map for Goblins overlay", WS_POPUP,
                             0, 0, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
}

// ── 'surface' mode helpers: a DComp SURFACE (no swapchain) + an intermediate full-size RT we
// render ImGui into, then blit into the surface via CopySubresourceRegion each frame (BeginDraw
// hands back an atlas texture + offset, so we copy rather than render directly at the offset). ──
static void release_surface_targets()
{
    if (g_surf_rtv) { g_surf_rtv->Release(); g_surf_rtv = nullptr; }
    if (g_surf_tex) { g_surf_tex->Release(); g_surf_tex = nullptr; }
    if (g_dcomp_surface) { g_dcomp_surface->Release(); g_dcomp_surface = nullptr; }
}
static bool create_surface_targets(UINT w, UINT h)
{
    release_surface_targets();
    if (!g_dcomp_device || !g_dcomp_visual || !g_dcomp_target || !g_d3d_device || !w || !h) return false;
    if (FAILED(g_dcomp_device->CreateSurface(w, h, DXGI_FORMAT_B8G8R8A8_UNORM,
                                             DXGI_ALPHA_MODE_PREMULTIPLIED, &g_dcomp_surface)) || !g_dcomp_surface)
        return false;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(g_d3d_device->CreateTexture2D(&td, nullptr, &g_surf_tex)) || !g_surf_tex) return false;
    if (FAILED(g_d3d_device->CreateRenderTargetView(g_surf_tex, nullptr, &g_surf_rtv)) || !g_surf_rtv) return false;
    g_dcomp_visual->SetContent(g_dcomp_surface);
    g_dcomp_target->SetRoot(g_dcomp_visual);
    g_dcomp_device->Commit();
    g_back_w = w; g_back_h = h;
    return true;
}

// ── D3D11 + DirectComposition + ImGui dx11 backend creation ──
// POD-only locals: this body is wrapped by an SEH guard (a torn GPU state can AV).
static bool init_d3d()
{
    // 1) D3D11 device (BGRA support is required for DComp).
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL got{};
    const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                 want, 2, D3D11_SDK_VERSION,
                                 &g_d3d_device, &got, &g_d3d_ctx)))
    {
        spdlog::error("[OVERLAY] D3D11CreateDevice failed");
        return false;
    }

    // 2) Render mode from ini: layered (default) / surface / swapchain (see RenderMode notes above).
    {
        const std::string &mstr = goblin::config::overlayRenderMode;
        g_render_mode = (mstr == "swapchain") ? RenderMode::Swapchain
                      : (mstr == "surface")   ? RenderMode::Surface
                                              : RenderMode::Layered;
    }

    RECT cr{};
    GetClientRect(g_hwnd, &cr);
    UINT w = static_cast<UINT>(cr.right - cr.left), h = static_cast<UINT>(cr.bottom - cr.top);
    if (w == 0) w = 1;
    if (h == 0) h = 1;
    g_back_w = w;
    g_back_h = h;

    if (g_render_mode == RenderMode::Layered)
    {
        g_use_layered = true;
        recreate_window_layered();
        if (!g_hwnd) { spdlog::error("[OVERLAY] layered window creation failed"); return false; }
        if (!create_layered_targets(w, h)) { spdlog::error("[OVERLAY] layered targets failed"); return false; }
        spdlog::info("[OVERLAY] render mode: layered (GDI, max compatibility)");
    }
    else
    {
        // GPU paths (surface / swapchain) need a DXGI device + a DComp device/target/visual.
        IDXGIDevice *dxgiDevice = nullptr;
        HRESULT hr = E_FAIL;
        bool ok = false;
        if (SUCCEEDED(g_d3d_device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) && dxgiDevice)
        {
            if (SUCCEEDED(DCompositionCreateDevice(dxgiDevice, IID_PPV_ARGS(&g_dcomp_device))) && g_dcomp_device &&
                SUCCEEDED(g_dcomp_device->CreateTargetForHwnd(g_hwnd, TRUE, &g_dcomp_target)) && g_dcomp_target &&
                SUCCEEDED(g_dcomp_device->CreateVisual(&g_dcomp_visual)) && g_dcomp_visual)
            {
                if (g_render_mode == RenderMode::Surface)
                {
                    ok = create_surface_targets(w, h);
                    g_use_surface = ok;
                    if (ok) spdlog::info("[OVERLAY] render mode: surface (DComp surface, no swapchain)");
                }
                else // Swapchain
                {
                    IDXGIAdapter *adapter = nullptr;
                    IDXGIFactory2 *factory = nullptr;
                    if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) && adapter &&
                        SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory))) && factory)
                    {
                        DXGI_SWAP_CHAIN_DESC1 scd{};
                        scd.Width = w; scd.Height = h;
                        scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                        scd.SampleDesc.Count = 1;
                        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                        scd.BufferCount = 2;
                        scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
                        scd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
                        scd.Scaling = DXGI_SCALING_STRETCH;
                        hr = factory->CreateSwapChainForComposition(g_d3d_device, &scd, nullptr, &g_swapchain);
                        if (SUCCEEDED(hr) && g_swapchain)
                        {
                            g_dcomp_visual->SetContent(g_swapchain);
                            g_dcomp_target->SetRoot(g_dcomp_visual);
                            g_dcomp_device->Commit();
                            ID3D11Texture2D *back = nullptr;
                            if (SUCCEEDED(g_swapchain->GetBuffer(0, IID_PPV_ARGS(&back))) && back)
                            {
                                ok = SUCCEEDED(g_d3d_device->CreateRenderTargetView(back, nullptr, &g_rtv)) && g_rtv;
                                back->Release();
                            }
                        }
                    }
                    if (factory) factory->Release();
                    if (adapter) adapter->Release();
                    if (ok) spdlog::info("[OVERLAY] render mode: swapchain (DComp swapchain)");
                }
            }
        }
        if (dxgiDevice) dxgiDevice->Release();
        if (!ok)
        {
            spdlog::info("[OVERLAY] GPU render mode unavailable (0x{:08X}); using layered fallback",
                         static_cast<unsigned>(hr));
            if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
            if (g_swapchain) { g_swapchain->Release(); g_swapchain = nullptr; }
            release_surface_targets();
            if (g_dcomp_visual) { g_dcomp_visual->Release(); g_dcomp_visual = nullptr; }
            if (g_dcomp_target) { g_dcomp_target->Release(); g_dcomp_target = nullptr; }
            if (g_dcomp_device) { g_dcomp_device->Release(); g_dcomp_device = nullptr; }
            g_render_mode = RenderMode::Layered;
            g_use_surface = false;
            g_use_layered = true;
            recreate_window_layered();
            if (!g_hwnd || !create_layered_targets(w, h)) { spdlog::error("[OVERLAY] layered fallback failed"); return false; }
        }
    }

    // 5) ImGui context (once) + DX11 backend.
    if (!g_context_inited)
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr; // don't drop an imgui.ini next to the game
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        // Draw our OWN software cursor (MouseDrawCursor below) but never touch the OS
        // cursor: without this the backend calls SetCursor(NULL) to hide the OS cursor,
        // which clobbers the global cursor image and leaves the game (and desktop)
        // cursor-less after the menu closes. With this flag ImGui renders the cursor
        // into our frame and the game keeps managing its own OS cursor untouched.
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        // Base font = Segoe UI (Latin + Cyrillic; the dump text can be Russian).
        // A CJK font is merged on top ONLY when the active UI language is Chinese,
        // so non-Chinese users don't load a CJK file or pay the larger atlas. The
        // CJK merge carries only the glyphs the UI actually uses (font_glyph_seed).
        {
            const goblin::i18n::Language ui_lang = goblin::i18n::current_language();
            const bool need_cjk = ui_lang == goblin::i18n::Language::SimplifiedChinese ||
                                  ui_lang == goblin::i18n::Language::TraditionalChinese ||
                                  ui_lang == goblin::i18n::Language::Korean;

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
                    break;
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
                // Prefer the matching script's font first (YaHei=SC, JhengHei=TC, Malgun=KO).
                const char *cjk_sc[] = {"C:\\Windows\\Fonts\\msyh.ttc", "C:\\Windows\\Fonts\\msjh.ttc",
                                        "C:\\Windows\\Fonts\\simhei.ttf", "C:\\Windows\\Fonts\\simsun.ttc"};
                const char *cjk_tc[] = {"C:\\Windows\\Fonts\\msjh.ttc", "C:\\Windows\\Fonts\\msyh.ttc",
                                        "C:\\Windows\\Fonts\\simsun.ttc", "C:\\Windows\\Fonts\\simhei.ttf"};
                const char *cjk_ko[] = {"C:\Windows\Fonts\malgun.ttf", "C:\Windows\Fonts\malgun.ttc",
                                        "C:\Windows\Fonts\msyh.ttc", "C:\Windows\Fonts\msjh.ttc"};
                const char *const *cjk_fonts = nullptr;
                if (ui_lang == goblin::i18n::Language::TraditionalChinese)
                    cjk_fonts = cjk_tc;
                else if (ui_lang == goblin::i18n::Language::Korean)
                    cjk_fonts = cjk_ko;
                else
                    cjk_fonts = cjk_sc;
                bool merged = false;
                for (int i = 0; i < 4; ++i)
                {
                    const char *fp = cjk_fonts[i];
                    if (GetFileAttributesA(fp) != INVALID_FILE_ATTRIBUTES &&
                        io.Fonts->AddFontFromFileTTF(fp, 18.0f, &cfg, cjk_ranges.Data))
                    {
                        merged = true;
                        break;
                    }
                }
                if (!merged)
                    spdlog::warn("[OVERLAY] no CJK font found; Chinese UI may show as '?'");
            }
        }
        apply_er_style();
        ImGui_ImplWin32_Init(g_hwnd);
        g_context_inited = true;
    }

    if (!ImGui_ImplDX11_Init(g_d3d_device, g_d3d_ctx))
    {
        spdlog::error("[OVERLAY] ImGui_ImplDX11_Init failed");
        return false;
    }
    g_d3d_inited = true;
    goblin::diag::set_overlay(goblin::diag::OverlayState::Active, "");
    return true;
}

static bool seh_init_d3d()
{
    __try { return init_d3d(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Recreate the RTV + resize the composition swapchain when the game window changes
// size. POD-only locals (SEH-wrapped by the caller).
static void resize_swapchain(UINT w, UINT h)
{
    if (g_use_layered) { create_layered_targets(w, h); return; } // re-make RT+staging+DIB
    if (g_use_surface) { create_surface_targets(w, h); return; } // re-make DComp surface + intermediate RT
    if (!g_swapchain || w == 0 || h == 0)
        return;
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    if (FAILED(g_swapchain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0)))
        return;
    ID3D11Texture2D *back = nullptr;
    if (SUCCEEDED(g_swapchain->GetBuffer(0, IID_PPV_ARGS(&back))) && back)
    {
        g_d3d_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
    g_back_w = w;
    g_back_h = h;
}

static void seh_resize(UINT w, UINT h)
{
    __try { resize_swapchain(w, h); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// One rendered frame (SEH-wrapped). Clears to transparent; draws ImGui only when
// the menu is open; presents. POD-only locals.
static void render_frame(bool draw)
{
    __try
    {
        const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // fully transparent
        if (g_use_layered)
        {
            // Proton/Wine: render to the offscreen RT, read back, blit via UpdateLayeredWindow.
            if (!g_lrtv || !g_lstaging || !g_ltex || !g_ldib || !g_d3d_ctx)
                return;
            g_d3d_ctx->OMSetRenderTargets(1, &g_lrtv, nullptr);
            g_d3d_ctx->ClearRenderTargetView(g_lrtv, clear);
            if (draw)
                ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            g_d3d_ctx->CopyResource(g_lstaging, g_ltex);
            D3D11_MAPPED_SUBRESOURCE m{};
            if (SUCCEEDED(g_d3d_ctx->Map(g_lstaging, 0, D3D11_MAP_READ, 0, &m)))
            {
                const size_t rowbytes = static_cast<size_t>(g_back_w) * 4;
                for (UINT y = 0; y < g_back_h; ++y) // RT is already premultiplied BGRA
                    memcpy(static_cast<uint8_t *>(g_ldibbits) + static_cast<size_t>(y) * rowbytes,
                           static_cast<const uint8_t *>(m.pData) + static_cast<size_t>(y) * m.RowPitch,
                           rowbytes);
                g_d3d_ctx->Unmap(g_lstaging, 0);
                SIZE sz{static_cast<LONG>(g_back_w), static_cast<LONG>(g_back_h)};
                POINT src0{0, 0};
                BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
                HDC screen = GetDC(nullptr);
                // pptDst = null: cover_game_window owns position via SetWindowPos.
                UpdateLayeredWindow(g_hwnd, screen, nullptr, &sz, g_lmemdc, &src0, 0, &bf, ULW_ALPHA);
                ReleaseDC(nullptr, screen);
            }
            return;
        }
        if (g_use_surface)
        {
            // GPU 'surface' path: render into the intermediate RT, then blit into the DComp surface.
            // BeginDraw hands back an atlas texture + offset, so we CopySubresourceRegion at that offset.
            if (!g_surf_rtv || !g_surf_tex || !g_dcomp_surface || !g_dcomp_device || !g_d3d_ctx)
                return;
            g_d3d_ctx->OMSetRenderTargets(1, &g_surf_rtv, nullptr);
            g_d3d_ctx->ClearRenderTargetView(g_surf_rtv, clear);
            if (draw)
                ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            POINT off{};
            ID3D11Texture2D *dst = nullptr;
            if (SUCCEEDED(g_dcomp_surface->BeginDraw(nullptr, IID_PPV_ARGS(&dst), &off)) && dst)
            {
                D3D11_BOX box{0, 0, 0, g_back_w, g_back_h, 1};
                g_d3d_ctx->CopySubresourceRegion(dst, 0, static_cast<UINT>(off.x), static_cast<UINT>(off.y), 0,
                                                 g_surf_tex, 0, &box);
                dst->Release();
            }
            g_dcomp_surface->EndDraw();
            g_dcomp_device->Commit();
            return;
        }
        // ── Windows DComp path (unchanged). ──
        if (!g_rtv || !g_d3d_ctx || !g_swapchain)
            return;
        g_d3d_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_d3d_ctx->ClearRenderTargetView(g_rtv, clear);
        if (draw)
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swapchain->Present(1, 0);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // A bad frame must never take the whole game down.
    }
}

// ── Raw-input hook: block the game's keyboard/mouse while the menu is open ──
// We do NOT steal the game's focus (that caused cursor breakage, alt-tab-on-close, and a
// GetAsyncKeyState focus asymmetry that made F10 close-then-reopen) and a low-level
// keyboard hook does not get delivered in this game/loader. Instead we hook the game's
// GetRawInputData (how the engine reads keyboard/mouse) and neutralize the payload while
// the menu is open, so menu input never leaks into gameplay. Open/close + rebind still
// use GetAsyncKeyState polling (the game keeps focus, so it is reliable and symmetric).
// The menu's own mouse comes via our window's WM_MOUSE, independent of raw input.
using GetRawInputData_t = UINT(WINAPI *)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
GetRawInputData_t o_GetRawInputData = nullptr;

UINT WINAPI hk_GetRawInputData(HRAWINPUT hri, UINT cmd, LPVOID data, PUINT size, UINT hsz)
{
    UINT r = o_GetRawInputData(hri, cmd, data, size, hsz);
    // Only touch the actual data fetch (data != null); leave the size query alone.
    if (g_menu_open.load() && data && cmd == RID_INPUT && r != static_cast<UINT>(-1))
    {
        RAWINPUT *ri = reinterpret_cast<RAWINPUT *>(data);
        if (ri->header.dwType == RIM_TYPEMOUSE)
        {
            ri->data.mouse.lLastX = 0;
            ri->data.mouse.lLastY = 0;
            ri->data.mouse.usButtonFlags = 0;
            ri->data.mouse.usButtonData = 0;
            ri->data.mouse.ulRawButtons = 0;
        }
        else if (ri->header.dwType == RIM_TYPEKEYBOARD)
        {
            ri->data.keyboard.MakeCode = 0;
            ri->data.keyboard.VKey = 0;
            ri->data.keyboard.Message = WM_NULL;
            ri->data.keyboard.Flags = RI_KEY_BREAK; // report as a no-op key-up
        }
    }
    return r;
}

// The game's FPS camera recenters / confines the OS cursor every frame (SetCursorPos +
// ClipCursor). While the menu is open we no-op those so the cursor moves freely and our
// ImGui menu can use it (the game keeps focus; raw input is already neutralized above).
using SetCursorPos_t = BOOL(WINAPI *)(int, int);
SetCursorPos_t o_SetCursorPos = nullptr;
BOOL WINAPI hk_SetCursorPos(int x, int y)
{
    if (g_menu_open.load())
        return TRUE; // swallow the game's recenter so the cursor is not pinned to center
    return o_SetCursorPos(x, y);
}

using ClipCursor_t = BOOL(WINAPI *)(const RECT *);
ClipCursor_t o_ClipCursor = nullptr;
BOOL WINAPI hk_ClipCursor(const RECT *r)
{
    if (g_menu_open.load())
        return o_ClipCursor(nullptr); // unconfine the cursor while the menu is open
    return o_ClipCursor(r);
}

// While the menu is open, feed the GAME a NEUTRAL pad (connected, nothing pressed) so its
// buttons do not leak into gameplay. We keep the real connect status + packet number so
// the game keeps polling the slot - returning ERROR_DEVICE_NOT_CONNECTED makes games drop
// the pad and stop polling it, killing the gamepad even after the menu closes. Our own
// poll_gamepad reads the real pad via the o_ trampoline.
DWORD WINAPI hk_XInputGetState(DWORD idx, XINPUT_STATE *state)
{
    DWORD r = o_XInputGetState(idx, state);
    if (g_menu_open.load() && r == ERROR_SUCCESS && state)
        state->Gamepad = XINPUT_GAMEPAD{}; // zero buttons + centre sticks; keep connected
    return r;
}

// ── Open/close edge detection + side effects (config reload/save, focus) ──
void update_menu_toggle()
{
    if (g_rebind_mode.load() != 0 && !g_menu_open.load())
        reset_rebind_state();
    const bool rebinding = g_rebind_mode.load() != 0;

    // Toggle on the configured key (default F10) or the gamepad combo, rising edge. We do
    // NOT steal focus, so the GAME keeps focus and GetAsyncKeyState is reliable + symmetric
    // (opens AND closes). Key leak into the game is blocked by the raw-input hook above.
    static bool prev_open_in = false;
    const int open_key = static_cast<int>(goblin::config::toggleInjectionKey);
    const uint16_t pad_mask = goblin::config::toggleGamepadMask;
    const bool combo = g_pad_ok && pad_mask && (g_pad.wButtons & pad_mask) == pad_mask;
    const bool open_in = kd(open_key) || combo;
    if (open_in && !prev_open_in && !rebinding)
        g_menu_open.store(!g_menu_open.load());
    prev_open_in = open_in;

    // ESC (keyboard) or B (gamepad) close while the menu is open.
    static bool prev_esc = false, prev_padb = false;
    const bool esc = kd(VK_ESCAPE);
    const bool padb = g_pad_ok && (g_pad.wButtons & XINPUT_GAMEPAD_B) != 0;
    if (g_menu_open.load() && !rebinding && ((esc && !prev_esc) || (padb && !prev_padb)))
        g_menu_open.store(false);
    prev_esc = esc;
    prev_padb = padb;

    // Open/close side effects: reload settings on open, auto-save on close, and SHOW the
    // window on open / HIDE it on close. We do NOT steal foreground: the game keeps focus
    // (mouse still drives the menu since our topmost window gets WM_MOUSE unfocused, and
    // the keyboard hook blocks key leak). A hidden closed window touches neither input
    // nor the cursor, so the game fully owns the cursor when the menu is down.
    static bool prev_open = false;
    const bool open_now = g_menu_open.load();
    if (open_now && !prev_open)
    {
        goblin::load_config(goblin::g_ini_path);
        goblin::reapply_live_settings();
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE); // show without taking focus from the game
        SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    else if (!open_now && prev_open)
    {
        reset_rebind_state();
        goblin::save_config(goblin::g_ini_path);
        ShowWindow(g_hwnd, SW_HIDE);
    }
    prev_open = open_now;
}

// ── The overlay thread: window + D3D11 + DComp + ImGui + render loop ──
void overlay_thread()
{
    pXInputGetState = nullptr;
    {
        const char *xdlls[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
        for (const char *d : xdlls)
            if (HMODULE h = GetModuleHandleA(d))
                if (auto p = reinterpret_cast<XInputGetState_t>(GetProcAddress(h, "XInputGetState")))
                { pXInputGetState = p; break; }
        if (!pXInputGetState)
            if (HMODULE h = LoadLibraryA("xinput1_4.dll"))
                pXInputGetState = reinterpret_cast<XInputGetState_t>(GetProcAddress(h, "XInputGetState"));
    }
    // Hook XInputGetState so the game sees a disconnected pad while the menu is open (no
    // gamepad leak). Done here (not in setup) because the xinput DLL is resolved above;
    // enable_hooks re-applies the queue (dllmain already applied the earlier hooks).
    if (pXInputGetState)
    {
        try
        {
            modutils::hook(reinterpret_cast<void *>(pXInputGetState),
                           reinterpret_cast<void *>(&hk_XInputGetState),
                           reinterpret_cast<void **>(&o_XInputGetState));
            modutils::enable_hooks();
        }
        catch (const std::exception &e) { spdlog::warn("[OVERLAY] gamepad route unavailable: {}", e.what()); }
    }

    // Register our window class + create the transparent, click-through, top-most window.
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = overlay_wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = OVERLAY_CLASS;
    wc.hCursor = nullptr; // no class cursor: we draw ImGui's software cursor and never
                          // impose the OS arrow over the game (game keeps its own cursor)
    RegisterClassExW(&wc);

    // WS_EX_NOREDIRECTIONBITMAP (NOT WS_EX_LAYERED): the window content is composited
    // by DirectComposition, so we must suppress the DWM redirection surface. The window
    // is SHOWN only while the menu is open and HIDDEN when closed (a hidden window can
    // touch neither input nor the cursor), so we do not need WS_EX_TRANSPARENT
    // click-through at all - hide/show is the cleaner, race-free model.
    const DWORD ex = WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST |
                     WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
    g_hwnd = CreateWindowExW(ex, OVERLAY_CLASS, L"Map for Goblins overlay", WS_POPUP,
                             0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hwnd)
    {
        spdlog::error("[OVERLAY] CreateWindowExW failed; overlay disabled");
        goblin::diag::set_overlay(goblin::diag::OverlayState::Failed, "window creation failed");
        return;
    }
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);

    if (!seh_init_d3d())
    {
        goblin::diag::set_overlay(goblin::diag::OverlayState::Failed, "D3D11/DComp init failed");
        DestroyWindow(g_hwnd);
        g_hwnd = nullptr;
        return;
    }

    ShowWindow(g_hwnd, SW_HIDE); // menu starts closed -> window hidden (zero interference)

    // Render loop: pump our own messages, track the game window, poll hotkeys, draw.
    while (g_running.load())
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        // Cover the game's client area; resize our swapchain if it changed.
        int gw = 0, gh = 0;
        cover_game_window(gw, gh);
        if (gw > 0 && gh > 0 &&
            (static_cast<UINT>(gw) != g_back_w || static_cast<UINT>(gh) != g_back_h))
            seh_resize(static_cast<UINT>(gw), static_cast<UINT>(gh));

        poll_gamepad();
        update_menu_toggle();

        const bool open = g_menu_open.load();
        if (open)
        {
            try_upload_atlas();
            maybe_load_preview();
            poll_rebind_keyboard();
            process_rebind();

            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            feed_gamepad(); // after NewFrame (the backend clears HasGamepad each frame)
            feed_nav_keyboard(); // polled keyboard nav (we never hold focus)
            ImGui::NewFrame();
            {
                float fs = goblin::config::fontScale; // clamp defensively (ini no longer clamps)
                ImGui::GetIO().FontGlobalScale = fs < 0.8f ? 0.8f : (fs > 3.0f ? 3.0f : fs);
            }
            ImGui::GetIO().MouseDrawCursor = true; // our window has no system cursor over the game
            draw_settings_window();
            draw_preview_window();
            ImGui::Render();
            render_frame(true);
        }
        else
        {
            // Menu closed: present a fully-transparent frame so the game shows through.
            render_frame(false);
            Sleep(16); // idle pacing while closed (no vsync wait from a cleared present)
        }
    }
}

// ── Best-effort teardown (the process usually just exits). ──
void teardown()
{
    if (g_d3d_inited)
    {
        __try { ImGui_ImplDX11_Shutdown(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (g_context_inited)
    {
        __try { ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext(); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (g_atlas_srv) { g_atlas_srv->Release(); g_atlas_srv = nullptr; }
    if (g_atlas_tex) { g_atlas_tex->Release(); g_atlas_tex = nullptr; }
    if (g_logo_srv) { g_logo_srv->Release(); g_logo_srv = nullptr; }
    if (g_logo_tex) { g_logo_tex->Release(); g_logo_tex = nullptr; }
    if (g_preview_srv) { g_preview_srv->Release(); g_preview_srv = nullptr; }
    if (g_preview_tex) { g_preview_tex->Release(); g_preview_tex = nullptr; }
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    if (g_dcomp_visual) { g_dcomp_visual->Release(); g_dcomp_visual = nullptr; }
    if (g_dcomp_target) { g_dcomp_target->Release(); g_dcomp_target = nullptr; }
    if (g_dcomp_device) { g_dcomp_device->Release(); g_dcomp_device = nullptr; }
    if (g_swapchain) { g_swapchain->Release(); g_swapchain = nullptr; }
    release_layered_targets(); // Proton fallback RT/staging/DIB
    release_surface_targets(); // 'surface' mode DComp surface + intermediate RT
    if (g_d3d_ctx) { g_d3d_ctx->Release(); g_d3d_ctx = nullptr; }
    if (g_d3d_device) { g_d3d_device->Release(); g_d3d_device = nullptr; }
    if (g_hwnd) { DestroyWindow(g_hwnd); g_hwnd = nullptr; }
}
} // namespace

bool goblin::overlay::key_down(int vk) { return kd(vk); }

void goblin::overlay::setup()
{
    if (!goblin::config::enableOverlay)
    {
        spdlog::info("[OVERLAY] disabled via ini (enable_overlay = false)");
        goblin::diag::set_overlay(goblin::diag::OverlayState::OffByConfig, "");
        return;
    }
    if (g_running.exchange(true))
        return; // already running

    // Spawn the dedicated overlay thread (window + D3D11 + DComp + ImGui + loop).
    // setup() returns immediately; the thread owns all overlay state.
    std::thread([] {
        overlay_thread();
        teardown();
        g_running.store(false);
    }).detach();

    // Hook user32 input APIs so that, while the menu is open, we neutralize the game's
    // keyboard/mouse (no input leak) AND stop it from recentering/confining the cursor
    // (so our mouse works) - all WITHOUT stealing focus. Queued here; dllmain applies
    // them via modutils::enable_hooks() right after this setup returns. None of this
    // touches the swapchain, so it is safe under frame-gen / Smooth Motion / Special K.
    if (HMODULE u32 = GetModuleHandleW(L"user32.dll"))
    {
        auto hook_api = [u32](const char *name, void *detour, void **tramp) {
            if (void *p = reinterpret_cast<void *>(GetProcAddress(u32, name)))
            {
                try { modutils::hook(p, detour, tramp); }
                catch (const std::exception &e) { spdlog::warn("[OVERLAY] input route unavailable: {}", e.what()); }
            }
        };
        hook_api("GetRawInputData", reinterpret_cast<void *>(&hk_GetRawInputData),
                 reinterpret_cast<void **>(&o_GetRawInputData));
        hook_api("SetCursorPos", reinterpret_cast<void *>(&hk_SetCursorPos),
                 reinterpret_cast<void **>(&o_SetCursorPos));
        hook_api("ClipCursor", reinterpret_cast<void *>(&hk_ClipCursor),
                 reinterpret_cast<void **>(&o_ClipCursor));
    }
}
