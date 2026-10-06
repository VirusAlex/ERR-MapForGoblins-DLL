// In-game config overlay: Dear ImGui, drawn INTO THE GAME'S OWN FRAME.
//
// WHAT SHIPS (MFG_OVERLAY_OWN_WINDOW = 0, the default): the sc2 in-swapchain backend. We publish
// ImGui draw data and the game's own D3D12 renderer paints it - no window of ours, no second
// swapchain, no D3D11 device, no DirectComposition. setup() builds the ImGui context and fonts
// WITHOUT a platform/renderer backend and runs sc2_frontend_loop() on its own thread.
//   - Custom images DO draw: the category icons, the logo and the highlight are packed into the
//     FONT ATLAS (AddCustomRectRegular + a blit per rect), because the packet carries one texture.
//   - Only an OPEN menu is published. The overlay draws nothing else: the on-map hover panel, the
//     focus banner and the highlight rings were retired on 2026-07-28 in favour of native
//     equivalents, which is why nothing here projects world coordinates to screen any more.
//   - Whether this overlay is created at all depends on menu_render_mode: at the shipping default
//     (`native`) the in-game menu owns the hotkey and the overlay is never created.
//
// THE ALTERNATIVE (MFG_OVERLAY_OWN_WINDOW = 1): a separate transparent top-most window with its
// own D3D11 + DirectComposition device, in three flavours picked by MFG_OWN_WINDOW_MODE (layered /
// surface / swapchain; Wine/Proton is forced to layered at runtime because DComp misbehaves under
// gamescope). It exists because a window of our own cannot be fought over by tools that wrap the
// game's swapchain, and because that was the shape the overlay had first. Everything specific to it
// lives under `#if MFG_OVERLAY_OWN_WINDOW` blocks further down, including the dev Icon Preview,
// which needs a texture of its own and therefore only works there.
//
// All UI drawing code (draw_settings_window and the tabs) is backend-agnostic and shared by both.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <dcomp.h>
#include <Xinput.h>

#include "goblin_overlay.hpp"
#include "goblin_config.hpp"
#include "goblin_native_menu.hpp" // key_swallowed: do not act on the press that just got bound
#include "goblin_config_schema.hpp"
#include "goblin_float_ranges.hpp" // slider ranges shared with the in-game menu
#include "goblin_i18n.hpp"
#include "goblin_overlay_icons.hpp"
#include "goblin_map_icons.hpp" // shared DefineBitsLossless2 icon tags (decoded here for the atlas)
#include "miniz.h"              // zlib inflate to decode the tags
// sc2: in-swapchain D3D12 backend - the only one built. It draws our
// ImGui into the game's OWN swapchain instead of a separate window, which is what removes the
// focus steal under Linux/Proton.
#include "sc2/hooks.hpp"
#include "sc2/log.hpp" // cte::g_hinst
#include "sc2/overlay_present.hpp"
#include "goblin_inject.hpp"
#include "goblin_markers.hpp"
#include "goblin_build_variants.hpp" // which overlay backend is in this build
// goblin_mapproject.hpp / goblin_collected.hpp were included here for the highlight-ring
// projection and the height readout; both went away with the on-map overlay drawing (2026-07-28).
#include "goblin_maphover.hpp" // back 2026-08-01: map_dialog() gates the pad combo's grace window
#include "goblin_messages.hpp"   // lookup_text() for marker names in the menu
#include "goblin_progress.hpp"
#include "goblin_search.hpp"     // item search tab
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
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>
#include <unordered_set>
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
// Our OWN reference on the xinput module (LoadLibrary, never GetModuleHandle): report 42 -
// a 2.1.3 player's game died executing free memory from our gamepad poll, i.e. the function
// behind pXInputGetState was gone. With a held reference the module we resolved cannot be
// unloaded under us; the guarded call below covers the case where the chain behind it can.
HMODULE g_xinput_module = nullptr;
// Set on the first fault inside an XInputGetState call: gamepad polling stops for the session
// (keyboard hotkeys keep working) instead of taking the process down.
std::atomic<bool> g_pad_poll_dead{false};
XInputGetState_t pXInputGetState = nullptr; // resolved once on the overlay thread
XInputGetState_t o_XInputGetState = nullptr; // trampoline: OUR poll reads the real pad
                                             // through this; the hook feeds the GAME a
                                             // disconnected pad while the menu is open.

// ── Our own window + D3D11 + DirectComposition state (overlay thread only) ──
// Own-window only: every use of this is inside MFG_OVERLAY_OWN_WINDOW (the class registration and
// the two CreateWindowExW calls). Unguarded it put a readable literal in a shipping DLL that has no
// window of its own - and readable strings with no code path are precisely what the AV heuristics
// weigh. Guarded 2026-07-31.
#if MFG_OVERLAY_OWN_WINDOW
const wchar_t *OVERLAY_CLASS = L"MFG_OverlayWindow";
#endif
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
// Render mode WAS chosen by the ini (that key now picks which MENU runs, see menu_render_mode):
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
ID3D11Texture2D *g_highlight_tex = nullptr;   // focus-highlight ring (drawn over markers)
ID3D11ShaderResourceView *g_highlight_srv = nullptr;
// True when running under Wine/Proton (ntdll exports wine_get_version). Set once in init_d3d.
// Used to re-assert the "game keeps foreground" invariant that WS_EX_NOACTIVATE gives us on
// Windows but which X11/Wayland window managers ignore.
bool g_is_wine = false;
bool g_atlas_ready = false;

// ── where the overlay's own images come from ──────────────────────────────────────────
// One indirection for every image we draw, because the two render paths get their pixels from
// different places. The window modes (surface/layered/swapchain) point these at the standalone
// D3D11 shader resource views with an IDENTITY uv transform - exactly what the code did before.
// The in-swapchain path (swapchain_2) has no D3D11 atlas of its own: its frame packet only carries
// the ImGui FONT texture token, so the icon atlas and the logo are merged into the FONT atlas as
// custom rects and these fields carry the uv transform INTO those rects. Routing every draw through
// them is what lets icons reach the D3D12 renderer without that renderer knowing about our textures.
ImTextureID g_icon_texid = nullptr;           // category / hover / progress icons
float g_icon_uofs = 0.0f, g_icon_vofs = 0.0f; // uv offset (rect origin / font atlas dims)
float g_icon_uscl = 1.0f, g_icon_vscl = 1.0f; // uv scale  (icon atlas dims / font atlas dims)
ImTextureID g_logo_texid = nullptr;
ImVec2 g_logo_uv0{0.0f, 0.0f}, g_logo_uv1{1.0f, 1.0f};
ImTextureID g_highlight_texid = nullptr;
ImVec2 g_highlight_uv0{0.0f, 0.0f}, g_highlight_uv1{1.0f, 1.0f};

// The window modes: draw straight from the SRVs, identity transform. Own-window only, like its
// definition - the in-swapchain backend points these ids at ImGui's font atlas instead.
#if MFG_OVERLAY_OWN_WINDOW
void point_images_at_srvs();
#endif

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
#if MFG_OVERLAY_OWN_WINDOW
static void open_preview_dialog(); // defined below (opens the file dialog on a worker thread)
#endif

std::atomic<bool> g_running{false}; // overlay thread alive (teardown guard)
std::atomic<bool> g_menu_open{false};
// The game's real mouse cursor, captured from its SetCursor calls (hk_SetCursor). Declared
// here (ahead of overlay_wndproc) so WM_SETCURSOR can re-assert it when the menu is closed.
std::atomic<HCURSOR> g_game_cursor{nullptr};
using SetCursor_t = HCURSOR(WINAPI *)(HCURSOR);
SetCursor_t o_SetCursor = nullptr; // real SetCursor trampoline (bypasses our capture hook)
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

// Mouse-wheel notches seen by the raw-input hook and not yet handed to ImGui.
//
// Everything else about the mouse is POLLED in sc2_feed_mouse - position from GetCursorPos,
// buttons from GetAsyncKeyState - because the overlay deliberately takes no focus and owns no
// window. The wheel cannot be polled: Windows exposes it only as a message or as raw input, and
// the raw-input hook below was already zeroing usButtonData to keep the game from scrolling
// underneath the menu, so the notches were being thrown away before anything could use them. The
// hook runs on the game's input thread, so it only accumulates here; sc2_feed_mouse drains it on
// the thread that owns the ImGui IO.
std::atomic<int> g_wheel_raw{0};

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
    if (ic && g_icon_texid)
    {
        // The cell's uv inside the icon-atlas space, then mapped through the (offset, scale)
        // transform: identity for the window modes, into the merged font-atlas rect for
        // swapchain_2.
        const float ru0 = (ic->col * CELL) / static_cast<float>(ATLAS_W);
        const float rv0 = (ic->row * CELL) / static_cast<float>(ATLAS_H);
        const float ru1 = ((ic->col + 1) * CELL) / static_cast<float>(ATLAS_W);
        const float rv1 = ((ic->row + 1) * CELL) / static_cast<float>(ATLAS_H);
        const ImVec2 uv0(g_icon_uofs + ru0 * g_icon_uscl, g_icon_vofs + rv0 * g_icon_vscl);
        const ImVec2 uv1(g_icon_uofs + ru1 * g_icon_uscl, g_icon_vofs + rv1 * g_icon_vscl);
        ImGui::Image(g_icon_texid, ImVec2(ICON_SZ, ICON_SZ), uv0, uv1);
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
            if (e.ini_only)
                continue; // the ini decides it alone: menu_render_mode picks WHICH menu
                          // exists, so no menu may offer it (IniEntry::ini_only)
            if (goblin::profile_is_vanilla() && e.err_only)
                continue;
            if (std::strcmp(e.key, "overlay_font_scale") == 0 ||
                std::strcmp(e.key, "overlay_opacity") == 0 ||
                std::strcmp(e.key, "map_panel_offset_percent") == 0)
                continue; // shown as prominent sliders at the top of the Settings tab
            if (std::strcmp(e.key, "search_hide_collected") == 0)
                continue; // lives on the Search tab, next to the results it filters
            if (std::strncmp(e.key, "overlay_window_", 15) == 0)
                continue; // auto-managed window geometry (saved on close) - not a UI control
            if (std::strcmp(e.key, "enable_manual_hide") == 0 ||
                std::strcmp(e.key, "hide_marker_key") == 0 ||
                std::strcmp(e.key, "hide_marker_gamepad") == 0)
                continue; // rendered at the TOP of the Hidden tab instead
            ImGui::PushID(e.key);
            // A yellow BETA warning was painted here for the key `fast_map_open`. build_schema() has
            // not emitted that key since it became a build variant (it is only in ini_retired_keys()
            // now), so this loop over ini_schema() could never see it: the branch was unreachable and
            // its TextId::FastMapOpenWarning was dead weight in the binary and in all eight locales.
            draw_row_icon(e.key);
            const char *label = tr::entry_label(e.key, lang);
            bool hovered = false; // mouse hover OR keyboard/gamepad nav focus on this row

            // Reserve a right-hand column for the dim ini-key code and WRAP the localized
            // label within the remaining width, so neither the name nor the code is clipped
            // by a narrow menu. The code is painted at the row's TOP-right via the draw list
            // (independent of control height) so a label that wraps to 2+ lines can't shove it.
            ImDrawList *row_dl = ImGui::GetWindowDrawList();
            const ImVec2 row_top = ImGui::GetCursorScreenPos();
            const float code_w = ImGui::CalcTextSize(e.key).x;
            const float right_x = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
            float wrap_x = ImGui::GetWindowContentRegionMax().x - code_w - ImGui::GetStyle().ItemSpacing.x * 2.0f;
            if (wrap_x < 90.0f) wrap_x = 90.0f;  // floor for extremely narrow menus
            auto wrapped_label = [&](bool dim) {
                if (dim) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::PushTextWrapPos(wrap_x);
                ImGui::TextWrapped("%s", label);
                ImGui::PopTextWrapPos();
                if (dim) ImGui::PopStyleColor();
            };
            // Inline label for combo/slider/rebind rows: name on the SAME line as the control
            // (labels are short, so there's room; no more dropping the control to a 2nd line).
            auto inline_label = [&]() {
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(label);
                hovered = hovered || ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip);
                ImGui::SameLine();
            };
            // Width for an inline control: fill to the dim-code column (never negative).
            auto inline_ctrl_w = [&]() {
                float w = ImGui::GetContentRegionAvail().x - code_w - ImGui::GetStyle().ItemSpacing.x * 2.0f;
                return w > 80.0f ? w : 80.0f;
            };

            if (e.type == goblin::IniType::Bool)
            {
                // Lock the menu/hotkey master switches: unchecking menu_enabled would
                // close the overlay with no way to reopen it, and enable_toggle_hotkey only
                // matters when the overlay is OFF. We grey them but keep them navigable so
                // their tooltip is reachable by keyboard/gamepad; the toggle is ignored.
                const bool locked = std::strcmp(e.key, "menu_enabled") == 0 ||
                                    std::strcmp(e.key, "enable_toggle_hotkey") == 0;
                bool v = *static_cast<bool *>(e.target);
                const bool box_clicked = ImGui::Checkbox("##k", &v);  // wrapped label drawn separately
                hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip);
                ImGui::SameLine();
                wrapped_label(locked);
                hovered = hovered || ImGui::IsItemHovered();
                const bool label_clicked = ImGui::IsItemClicked();
                if ((box_clicked || label_clicked) && !locked)
                {
                    *static_cast<bool *>(e.target) = box_clicked ? v : !v;
                    changed = true;
                }
                if (locked)
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", tr::tr(tr::TextId::IniOnly, lang));
                    hovered = hovered || ImGui::IsItemHovered();
                }
            }
            else if (e.type == goblin::IniType::U8)
            {
                inline_label();
                int v = *static_cast<uint8_t *>(e.target);
                ImGui::SetNextItemWidth(inline_ctrl_w());
                if (ImGui::SliderInt("##k", &v, 0, 30))
                {
                    *static_cast<uint8_t *>(e.target) =
                        static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
                    changed = true;
                }
                hovered = hovered || ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip);
            }
            else if (e.type == goblin::IniType::Language)
            {
                inline_label();
                std::string &value = *static_cast<std::string *>(e.target);
                const char *preview = tr::language_preview_label(value, lang);
                ImGui::SetNextItemWidth(inline_ctrl_w());
                if (ImGui::BeginCombo("##k", preview))
                {
                    const char *options[] = {"auto", "english", "schinese", "tchinese", "korean",
                                             "russian", "german", "french", "spanish", "vietnamese"};
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
                hovered = hovered || ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip);
            }
            else if (e.type == goblin::IniType::Float)
            {
                // Until 2026-09-11 there was no Float branch, so these rows fell into the key/combo
                // rebind branch below and offered "rebind" for a size factor.
                inline_label();
                float &value = *static_cast<float *>(e.target);
                if (const goblin::FloatRange *r = goblin::float_range(e.key))
                {
                    ImGui::SetNextItemWidth(inline_ctrl_w());
                    ImGui::SliderFloat("##k", &value, r->min, r->max, r->format,
                                       ImGuiSliderFlags_AlwaysClamp);
                    // The value itself is live while dragging (the marker code reads it every tick);
                    // the full re-apply runs once, on release, not on every frame of the drag.
                    if (ImGui::IsItemDeactivatedAfterEdit())
                        changed = true;
                }
                else
                {
                    ImGui::TextDisabled("%.2f", value); // no editable range: shown, not offered
                }
                hovered = hovered || ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip);
            }
            else if (e.type == goblin::IniType::Text)
            {
                // Read-only. The one Text key is menu_render_mode, and it is ini_only - it decides
                // WHICH menu exists, so the menu it would appear in must not offer it. This branch is
                // kept for the next Text setting, and shows the value rather than a combo of options
                // that no longer exist (it used to list surface / layered / swapchain).
                inline_label();
                ImGui::TextDisabled("%s", static_cast<std::string *>(e.target)->c_str());
                hovered = hovered || ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip);
            }
            else if (e.type == goblin::IniType::VkKey || e.type == goblin::IniType::GamepadMask)
                // name + value + in-place rebind, all on one line
            {
                const bool is_key = e.type == goblin::IniType::VkKey;
                const std::string val =
                    is_key ? fmt_vk(*static_cast<uint32_t *>(e.target))
                           : fmt_gamepad(*static_cast<uint16_t *>(e.target));
                const bool capturing = g_rebind_mode.load() != 0 && g_rebind_target == e.target;
                inline_label();
                ImGui::AlignTextToFramePadding();
                ImGui::Text("= %s", val.c_str());
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
            // Dim ini-key code painted at the row's top-right (all row types), so power users
            // see the raw key while the localized name stays left and wraps under it.
            row_dl->AddText(ImVec2(right_x - code_w, row_top.y),
                            ImGui::GetColorU32(ImGuiCol_TextDisabled), e.key);
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
    auto slider_key = [](const char *key) {  // dim raw ini key, right-aligned on the row
        ImGui::SameLine();
        const float kw = ImGui::CalcTextSize(key).x;
        const float rx = ImGui::GetWindowContentRegionMax().x - kw;
        if (rx > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(rx);
        ImGui::TextDisabled("%s", key);
    };
    // Ranges from the table the in-game menu reads too (goblin_float_ranges.hpp).
    auto top_slider = [](const char *label, float *value, const char *key) {
        const goblin::FloatRange *r = goblin::float_range(key);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
        ImGui::SliderFloat(label, value, r->min, r->max, r->format, ImGuiSliderFlags_AlwaysClamp);
    };
    top_slider(tr::tr(tr::TextId::OverlayTextSize, lang), &goblin::config::fontScale, "overlay_font_scale");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("%s", tr::tr(tr::TextId::OverlayTextSizeTip, lang));
    slider_key("overlay_font_scale");

    // Overlay panel opacity (window bg alpha). Persisted to overlay_opacity on close.
    top_slider(tr::tr(tr::TextId::OverlayOpacity, lang), &goblin::config::overlayOpacity, "overlay_opacity");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("%s", tr::tr(tr::TextId::OverlayOpacityTip, lang));
    slider_key("overlay_opacity");

    // Where the map-screen panels sit horizontally. 100 is the corner they were authored for, 0 is
    // the centre of the map area, above 100 pushes them further left. Exposed because an ultrawide
    // report could not be reproduced here: three aspect packs were diffed and the visible rect's
    // left edge is identical in all of them, so on every .gfx we have the panels land in the same
    // place. Rather than guess at a mechanism we cannot observe, let the player move them.
    // Read every frame the tooltip draws, so dragging this moves the panel with the map still open.
    top_slider(tr::tr(tr::TextId::MapPanelOffset, lang), &goblin::config::mapPanelOffsetPercent,
               "map_panel_offset_percent");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("%s", tr::tr(tr::TextId::MapPanelOffsetTip, lang));
    slider_key("map_panel_offset_percent");
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

    // The inject-status readout (a diag::report() summary plus a Copy button) lived at the top of
    // this tab until 2026-07-29: every load step with a reason for anything that failed, meant to be
    // screenshotted into a bug report. Nobody ever sent one - players send LOGS - so on 2026-07-31
    // the registry behind it left the build too (src/goblin_diag.*, see CMakeLists). Two things it
    // held were NOT logged anywhere and were moved into the log before it went: the logo plaque
    // re-point outcome, and the live worldmap sprite pointer that reveals the heap region.
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

    // Icon Preview needs a texture of its own, which only the own-window backend can create
    // (upload_rgba goes through g_d3d_device, and maybe_load_preview/draw_preview_window are
    // pumped from that backend's loop). Built into the in-swapchain build, the button opened a
    // modal file dialog and then nothing could ever appear - so it is offered only where it works.
#if MFG_OVERLAY_OWN_WINDOW
    ImGui::Separator();
    ImGui::TextWrapped("%s", tr::tr(tr::TextId::IconPreviewHint, lang));
    if (ImGui::Button(tr::tr(tr::TextId::IconPreviewOpen, lang)))
        open_preview_dialog();
    if (g_preview_srv)
    {
        ImGui::SameLine();
        if (ImGui::Button(tr::tr(tr::TextId::IconPreviewClose, lang)))
            g_preview_show.store(false);
        ImGui::SameLine();
        if (!g_preview_show.load() && ImGui::Button(tr::tr(tr::TextId::IconPreviewShow, lang)))
            g_preview_show.store(true);
        ImGui::SliderFloat(tr::tr(tr::TextId::IconPreviewSize, lang), &g_preview_px, 8.0f, 256.0f, "%.0f");
        ImGui::TextDisabled(tr::tr(tr::TextId::IconPreviewSource, lang), g_preview_w, g_preview_h);
    }
#endif // MFG_OVERLAY_OWN_WINDOW
}

// ── About tab: version + links ──
void draw_about_tab()
{
    namespace tr = goblin::i18n;
    const tr::Language lang = tr::current_language();

    // Large logo on the left, title/version/description to its right.
    if (g_atlas_ready && goblin::overlay_icons::LOGO_W > 0 && g_logo_texid)
    {
        const float lh = 120.0f;
        const float lw = lh * goblin::overlay_icons::LOGO_W /
                         static_cast<float>(goblin::overlay_icons::LOGO_H);
        ImGui::Image(g_logo_texid, ImVec2(lw, lh), g_logo_uv0, g_logo_uv1);
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
    // Split host/path (no full "https://domain/path" literal in the binary): a contiguous
    // URL literal in an unsigned, hook-installing DLL trips "TrojanDownloader" AV heuristics
    // even though we import NO network API and cannot download anything. Rebuilt at runtime.
    struct Link { tr::TextId label; const char *host; const char *path; const char *btn; };
    static const Link links[] = {
        {tr::TextId::LinkNexus,   "www.nexusmods.com", "/eldenring/mods/10062",           "##nx"},
        {tr::TextId::LinkGithub,  "github.com",        "/VirusAlex/ERR-MapForGoblins-DLL", "##gh"},
        {tr::TextId::LinkDiscord, "discord.gg",        "/JvTMwPCygB",                      "##dc"},
    };
    for (const auto &l : links)
    {
        const std::string url = std::string("https://") + l.host + l.path;
        ImGui::TextDisabled("%s:", tr::tr(l.label, lang));
        ImGui::TextUnformatted(url.c_str());
        ImGui::SameLine();
        std::string copy_label = std::string(tr::tr(tr::TextId::Copy, lang)) + l.btn;
        if (ImGui::SmallButton(copy_label.c_str()))
            ImGui::SetClipboardText(url.c_str());
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

// Disabled-color text that WRAPS at the content edge (ImGui::TextDisabled does not wrap,
// so long localized strings get clipped by a narrow menu). Used for header hints.
static void text_disabled_wrapped(const char *s)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", s);
    ImGui::PopStyleColor();
}

// ── Search tab: find markers by item name (player's language + English at once), tick the
// ones wanted, show only those on the map (the pick focus, goblin::set_focus_rows). ──
static char g_search_buf[128] = {};

static void draw_search_tab()
{
    namespace tr = goblin::i18n;
    const tr::Language lang = tr::current_language();
    goblin::progress::rebuild_if_stale(ImGui::GetTime()); // region names for the result groups

    // The text field, focused when the tab first shows so typing starts at once.
    static bool s_focus_field = true;
    if (s_focus_field) { ImGui::SetKeyboardFocusHere(); s_focus_field = false; }
    const char *clear_lbl = tr::tr(tr::TextId::SearchClearField, lang);
    const ImGuiStyle &st = ImGui::GetStyle();
    ImGui::SetNextItemWidth(-(ImGui::CalcTextSize(clear_lbl).x + st.FramePadding.x * 2.0f + st.ItemSpacing.x));
    ImGui::InputTextWithHint("##searchq", tr::tr(tr::TextId::SearchTypeHere, lang),
                             g_search_buf, sizeof g_search_buf);
    ImGui::SameLine();
    if (ImGui::Button(clear_lbl)) { g_search_buf[0] = '\0'; s_focus_field = true; }
    {
        // The layout the typed keys translate with; Alt+Shift / Ctrl+Shift / Win+Space cycles it.
        char tag[16] = {};
        WideCharToMultiByte(CP_UTF8, 0, goblin::overlay::text_layout_tag(), -1, tag, sizeof tag, nullptr, nullptr);
        if (tag[0]) { ImGui::SameLine(); ImGui::TextDisabled("[%s]", tag); }
    }

    const auto res_p = goblin::search::query(g_search_buf); // immutable snapshot
    const auto &res = *res_p;
    const size_t npick = goblin::search::pick_count();

    {
        bool only = goblin::config::searchHideCollected;
        if (ImGui::Checkbox(tr::entry_label("search_hide_collected", lang), &only))
            goblin::config::searchHideCollected = only; // saved with the ini on close; query() re-runs
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("%s", tr::entry_comment("search_hide_collected", "", lang));
    }
    if (g_search_buf[0] == '\0')
        text_disabled_wrapped(tr::tr(tr::TextId::SearchHint, lang));
    else if (res.total == 0)
        text_disabled_wrapped(tr::tr(tr::TextId::SearchNoResults, lang));
    else
    {
        ImGui::Text(tr::tr(tr::TextId::SearchMatches, lang),
                    static_cast<int>(res.total), static_cast<int>(res.groups.size()));
        if (res.truncated)
        {
            char t[160];
            std::snprintf(t, sizeof t, tr::tr(tr::TextId::SearchTruncated, lang),
                          static_cast<int>(goblin::search::kMaxHits));
            text_disabled_wrapped(t);
        }
    }

    // Actions. Picks apply to the map the moment they change (goblin::search), so there is no
    // "show" step: "show all found" ticks every hit, "clear picks" lifts the filter.
    {
        ImGui::BeginDisabled(res.total == 0);
        if (ImGui::Button(tr::tr(tr::TextId::SearchShowAll, lang)))
        {
            std::vector<uint64_t> keys;
            keys.reserve(res.total);
            for (const auto &g : res.groups)
                for (const auto &h : g.hits) keys.push_back(h.key);
            goblin::search::pick_many(keys, true);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(npick == 0);
        if (ImGui::Button(tr::tr(tr::TextId::SearchClearPicks, lang)))
            goblin::search::clear_picks();
        ImGui::EndDisabled();
    }
    if (goblin::focus_rows_active())
    {
        char subj[96];
        std::snprintf(subj, sizeof subj, tr::tr(tr::TextId::SearchFocusSubject, lang),
                      static_cast<int>(goblin::focus_rows_count()));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.78f, 0.35f, 1.0f));
        ImGui::TextWrapped("%s %s", tr::tr(tr::TextId::ProgressShowingOnly, lang), subj);
        ImGui::PopStyleColor();
        // (no reset button here: "clear picks" above IS the reset)
    }
    ImGui::Separator();

    // Results grouped by region. A collected / hidden marker gets a dim tag; that state is read
    // at most once a second (it walks every row's flags, like the Progress tab).
    static std::unordered_set<uint64_t> s_done;
    static double s_done_t = -10.0;
    if (res.total && ImGui::GetTime() - s_done_t > 1.0)
    {
        s_done = goblin::hidden_marker_original_ids();
        s_done_t = ImGui::GetTime();
    }
    // Group open/closed state is per query: new letters mean a new result set, so the tree
    // starts over - every region open, because the point of typing a name is to see where it is.
    // (Sets above 60 hits used to start collapsed; at most kMaxHits rows are submitted, and ImGui
    // draws only the visible ones, on the overlay's own thread.)
    uint32_t qhash = 2166136261u;
    for (const char *c = g_search_buf; *c; ++c) qhash = (qhash ^ static_cast<uint8_t>(*c)) * 16777619u;
    const bool open_all = true;

    ImGui::BeginChild("##searchscroll", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
    ImGui::PushID(static_cast<int>(qhash));
    for (const auto &g : res.groups)
    {
        ImGui::PushID(g.region);
        ImGui::SetNextItemOpen(open_all, ImGuiCond_Once);
        char hdr[320];
        std::snprintf(hdr, sizeof hdr, "%s (%d)###grp", g.region_name.c_str(), static_cast<int>(g.hits.size()));
        const bool open = ImGui::TreeNodeEx(hdr, ImGuiTreeNodeFlags_None);
        ImGui::SameLine();
        if (ImGui::SmallButton(tr::tr(tr::TextId::SearchPickAll, lang)))
        {
            std::vector<uint64_t> keys;
            keys.reserve(g.hits.size());
            for (const auto &h : g.hits) keys.push_back(h.key);
            goblin::search::pick_many(keys, true);
        }
        if (open)
        {
            for (const auto &h : g.hits)
            {
                ImGui::PushID(reinterpret_cast<const void *>(static_cast<uintptr_t>(h.key)));
                bool p = goblin::search::is_picked(h.key);
                if (ImGui::Checkbox("##pick", &p)) goblin::search::set_picked(h.key, p);
                ImGui::SameLine();
                const auto cat = static_cast<goblin::generated::Category>(h.cat);
                if (const char *ckey = goblin::category_config_key(cat)) draw_row_icon(ckey); // + SameLine
                ImGui::AlignTextToFramePadding();
                if (s_done.count(h.key))
                    ImGui::TextDisabled("%s  (%s)", h.name.c_str(), tr::tr(tr::TextId::SearchCollected, lang));
                else
                    ImGui::TextUnformatted(h.name.c_str());
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::PopID();
    ImGui::EndChild();
}

// ── Hidden-markers tab: markers the user hid manually (hover + hide_marker_key) ──
// Lists each with per-item Unhide + an Unhide-all button; changes apply live + persist.
// Shows the localized item name + location (from the row's baked textId/region), NOT the
// raw category enum + icon id.
static void draw_hidden_tab()
{
    namespace tr = goblin::i18n;
    const tr::Language lang = tr::current_language();

    // Manual-hide controls live at the TOP of this tab (moved off the Settings/Debug tabs):
    // the enable toggle + the hide-marker key. Reuse each schema entry's comment as the
    // English tooltip fallback (localized comments come from the i18n bundle).
    auto find_entry = [](const char *key) -> const goblin::IniEntry * {
        for (const auto &sec : goblin::ini_schema())
            for (const auto &e : sec.entries)
                if (std::strcmp(e.key, key) == 0) return &e;
        return nullptr;
    };
    bool changed = false;
    if (const auto *e = find_entry("enable_manual_hide"))
    {
        draw_row_icon(e->key);
        bool v = *static_cast<bool *>(e->target);
        if (ImGui::Checkbox(tr::entry_label(e->key, lang), &v))
        {
            *static_cast<bool *>(e->target) = v;
            changed = true;
        }
        if (e->comment && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("%s", tr::entry_comment(e->key, e->comment, lang));
    }
    if (const auto *e = find_entry("hide_marker_key"))
    {
        draw_row_icon(e->key);
        const std::string val = fmt_vk(*static_cast<uint32_t *>(e->target));
        const bool capturing = g_rebind_mode.load() != 0 && g_rebind_target == e->target;
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%s = %s", tr::entry_label(e->key, lang), val.c_str());
        const bool hov = ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip);
        ImGui::SameLine();
        if (capturing)
            ImGui::TextColored(ImVec4(1.0f, 0.84f, 0.38f, 1.0f), "%s", tr::tr(tr::TextId::PressAKey, lang));
        else if (ImGui::SmallButton(tr::entry_label("rebind", lang)))
        {
            g_rebind_target = e->target;
            g_rebind_pad_accum = 0;
            g_captured_vk.store(0);
            g_captured_up.store(false);
            g_rebind_mode.store(1);
        }
        if (e->comment && hov)
            ImGui::SetTooltip("%s", tr::entry_comment(e->key, e->comment, lang));
    }
    if (const auto *e = find_entry("hide_marker_gamepad"))
    {
        draw_row_icon(e->key);
        const std::string val = fmt_gamepad(*static_cast<uint16_t *>(e->target));
        const bool capturing = g_rebind_mode.load() != 0 && g_rebind_target == e->target;
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%s = %s", tr::entry_label(e->key, lang), val.c_str());
        const bool hov = ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip);
        ImGui::SameLine();
        if (capturing)
            ImGui::TextColored(ImVec4(1.0f, 0.84f, 0.38f, 1.0f), "%s", tr::tr(tr::TextId::PressComboRelease, lang));
        else if (ImGui::SmallButton(tr::entry_label("rebind", lang)))
        {
            g_rebind_target = e->target;
            g_rebind_pad_accum = 0;
            g_captured_vk.store(0);
            g_captured_up.store(false);
            g_rebind_mode.store(2);  // gamepad-combo capture
        }
        if (e->comment && hov)
            ImGui::SetTooltip("%s", tr::entry_comment(e->key, e->comment, lang));
    }
    if (changed)
        goblin::reapply_live_settings();
    ImGui::Separator();

    if (!goblin::config::enableManualHide)
    {
        text_disabled_wrapped(tr::tr(tr::TextId::HiddenDisabled, lang));
        return;
    }
    const auto hidden = goblin::manual_hidden_snapshot();
    if (hidden.empty())
    {
        text_disabled_wrapped(tr::tr(tr::TextId::HiddenMarkersNone, lang));
        return;
    }
    if (ImGui::Button(tr::tr(tr::TextId::UnhideAll, lang)))
    {
        goblin::clear_manual_hidden();
        goblin::persist_manual_hidden();
        goblin::reapply_live_settings();
        return;  // snapshot is now stale; refresh next frame
    }
    ImGui::Separator();
    ImGui::BeginChild("##hiddenscroll", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
    auto to_utf8 = [](const wchar_t *w, char *out, int cap) {
        if (w && *w) WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, nullptr, nullptr);
        else out[0] = '\0';
    };
    int idx = 0;
    for (const auto &h : hidden)
    {
        ImGui::PushID(idx++);
        if (ImGui::SmallButton(tr::tr(tr::TextId::Unhide, lang)))
        {
            goblin::unhide_marker(h.key);
            goblin::persist_manual_hidden();
            goblin::reapply_live_settings();
        }
        ImGui::SameLine();
        const auto cat = static_cast<goblin::generated::Category>(h.cat);
        const char *ckey = goblin::category_config_key(cat);
        if (ckey) draw_row_icon(ckey);  // category icon + SameLine
        const char *cname = ckey ? tr::entry_label(ckey, lang) : "?";
        char name[256], loc[256];
        to_utf8(goblin::lookup_text_any(h.textId), name, sizeof name);
        to_utf8(h.region > 0 ? goblin::lookup_text_any(h.region) : nullptr, loc, sizeof loc);
        // "Item - Location  (Category)"; fall back gracefully when a part is unresolved.
        if (name[0] && loc[0])
            ImGui::TextWrapped("%s  -  %s  (%s)", name, loc, cname);
        else if (name[0])
            ImGui::TextWrapped("%s  (%s)", name, cname);
        else
            ImGui::TextWrapped("%s", cname);
        ImGui::PopID();
    }
    ImGui::EndChild();
}

// A region row rendered AS its progress bar: full-width, frame-height, clickable to
// expand/collapse, with a collapse triangle + region name + count overlaid on a fill
// proportional to completion (same look as the category bars). Collapse state persists
// per region id in the window state storage. Returns true when the region is expanded.
// (Arrow is drawn as a triangle via the draw list, NOT a font glyph: the overlay font
// has no U+25B8/25BE, which would render as "?".)
static bool draw_region_header(const char *name, int collected, int total, int region_id)
{
    char sel_id[32];
    std::snprintf(sel_id, sizeof sel_id, "##reghdr%d", region_id);
    ImGuiStorage *st = ImGui::GetStateStorage();
    const ImGuiID key = ImGui::GetID(sel_id);
    bool open = st->GetBool(key, false);

    const float frac = total ? static_cast<float>(collected) / total : 0.0f;
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = ImGui::GetFrameHeight();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + w, p0.y + h);

    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
    const bool clicked = ImGui::Selectable(sel_id, false, ImGuiSelectableFlags_None, ImVec2(w, h));
    ImGui::PopStyleColor(3);
    const bool hovered = ImGui::IsItemHovered();
    if (clicked) { open = !open; st->SetBool(key, open); }

    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float rounding = 3.0f;
    dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImGuiCol_FrameBg), rounding);
    if (frac > 0.0f)
    {
        ImVec4 fc = progress_bar_color(frac);
        if (hovered) { fc.x *= 1.15f; fc.y *= 1.15f; fc.z *= 1.15f; }
        dl->AddRectFilled(p0, ImVec2(p0.x + w * frac, p1.y), ImGui::GetColorU32(fc), rounding);
    }
    dl->AddRect(p0, p1, ImGui::GetColorU32(ImGuiCol_Border), rounding);

    const ImU32 txt = ImGui::GetColorU32(ImGuiCol_Text);
    // Collapse triangle (right = collapsed, down = open), drawn as a filled tri.
    const float fs = ImGui::GetFontSize();
    const float cx = p0.x + 7.0f, cy = p0.y + h * 0.5f, a = fs * 0.32f;
    if (open)
        dl->AddTriangleFilled(ImVec2(cx - a, cy - a * 0.6f), ImVec2(cx + a, cy - a * 0.6f),
                              ImVec2(cx, cy + a * 0.8f), txt);
    else
        dl->AddTriangleFilled(ImVec2(cx - a * 0.4f, cy - a), ImVec2(cx - a * 0.4f, cy + a),
                              ImVec2(cx + a * 0.9f, cy), txt);
    const float ty = p0.y + (h - fs) * 0.5f;
    dl->AddText(ImVec2(p0.x + 20.0f, ty), txt, name);
    char cnt[32];
    std::snprintf(cnt, sizeof cnt, "%d/%d", collected, total);
    const float cw = ImGui::CalcTextSize(cnt).x;
    dl->AddText(ImVec2(p1.x - cw - 6.0f, ty), txt, cnt);
    return open;
}

void draw_progress_tab()
{
    namespace tr = goblin::i18n;
    const tr::Language lang = tr::current_language();
    namespace prog = goblin::progress;

    prog::rebuild_if_stale(ImGui::GetTime());
    const auto &regions = prog::snapshot();

    text_disabled_wrapped(tr::tr(tr::TextId::ProgressHint, lang));
    text_disabled_wrapped(tr::tr(tr::TextId::ProgressClickHint, lang));

    // Active focus is a (category, region) pair.
    const int focusCat = goblin::focus_category();
    const int32_t focusReg = goblin::focus_region();
    // Active-focus banner: which category+region the map is currently isolating. Drawn in a
    // FIXED-HEIGHT slot (always reserved, even when no filter is active) so toggling a filter
    // never shifts the region list up/down. The same text is mirrored top-left on the screen.
    const float bannerH = ImGui::GetTextLineHeightWithSpacing() * 2.0f + ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("##focusbanner", ImVec2(0, bannerH), 0, ImGuiWindowFlags_NoScrollbar);
    if (focusCat >= 0)
    {
        const char *regName = "?";
        for (const auto &r : regions)
            if (r.place_name_id == focusReg) { regName = r.name.c_str(); break; }
        const auto fcat = static_cast<goblin::generated::Category>(focusCat);
        const char *fkey = goblin::category_config_key(fcat);
        const char *fname = fkey ? tr::entry_label(fkey, lang) : goblin::markers::category_name(fcat);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.78f, 0.35f, 1.0f));
        ImGui::TextWrapped("%s %s - %s", tr::tr(tr::TextId::ProgressShowingOnly, lang), fname, regName);
        ImGui::PopStyleColor();
        if (ImGui::SmallButton(tr::tr(tr::TextId::ProgressFocusClear, lang)))
        {
            goblin::set_focus_category(-1);
            goblin::reapply_live_settings();
        }
    }
    else if (goblin::focus_rows_active())
    {
        // The search tab's pick set is isolating the map: say so here too, with the same reset.
        char subj[96];
        std::snprintf(subj, sizeof subj, tr::tr(tr::TextId::SearchFocusSubject, lang),
                      static_cast<int>(goblin::focus_rows_count()));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.78f, 0.35f, 1.0f));
        ImGui::TextWrapped("%s %s", tr::tr(tr::TextId::ProgressShowingOnly, lang), subj);
        ImGui::PopStyleColor();
        if (ImGui::SmallButton(tr::tr(tr::TextId::ProgressFocusClear, lang)))
        {
            goblin::set_focus_category(-1);
            goblin::reapply_live_settings();
        }
    }
    ImGui::EndChild();
    ImGui::Separator();

    // Clicking a category defers the focus change until after the draw loop.
    int toggle_cat = -2;         // -2 = no change this frame
    int32_t toggle_reg = -1;

    ImGui::BeginChild("##progscroll", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
    bool any = false;
    int last_mega = -1;  // emit a mega-section header (Lands Between / Dungeons / Shadow) on change
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

        // Mega-section header (The Lands Between / Dungeons / Shadow of the Erdtree) whenever
        // the group changes. Skipped for the trailing "Other" bucket (id < 0).
        if (rp.place_name_id >= 0 && static_cast<int>(rp.mega) != last_mega)
        {
            last_mega = static_cast<int>(rp.mega);
            const tr::TextId mid =
                rp.mega == prog::Mega::LandsBetween ? tr::TextId::MegaLandsBetween :
                rp.mega == prog::Mega::Dungeons     ? tr::TextId::MegaDungeons :
                                                      tr::TextId::MegaShadow;
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.82f, 1.0f, 1.0f));
            ImGui::SeparatorText(tr::tr(mid, lang));
            ImGui::PopStyleColor();
        }

        // The region header IS its progress bar (fill by completion); click to expand.
        // Collapse state persists per region id inside draw_region_header.
        if (!draw_region_header(rp.name.c_str(), vis_coll, vis_tot, rp.place_name_id))
            continue;

        ImGui::Spacing();
        ImGui::Indent(14.0f);  // nest the per-category bars under their region
        for (int ci = 0; ci < prog::kCategoryCount; ++ci)
        {
            const auto cat = static_cast<goblin::generated::Category>(ci);
            if (rp.cats[ci].total <= 0 || !goblin::category_enabled(cat))
                continue;
            const bool focused = (focusCat == ci && focusReg == rp.place_name_id);
            ImGui::PushID(rp.place_name_id * 128 + ci);
            // Reuse the Settings-tab icon + localized label for this category (via its
            // config key); fall back to the raw enum name if a category has no key.
            const char *ckey = goblin::category_config_key(cat);
            const char *cname = ckey ? tr::entry_label(ckey, lang)
                                     : goblin::markers::category_name(cat);
            if (ckey) draw_row_icon(ckey);  // icon + SameLine; bar fills the rest of the row
            if (draw_category_bar(cname, rp.cats[ci].collected, rp.cats[ci].total,
                                  focused, tr::tr(tr::TextId::ProgressClickHint, lang)))
            {
                toggle_cat = ci;
                toggle_reg = rp.place_name_id;
            }
            ImGui::PopID();
        }
        ImGui::Unindent(14.0f);
        ImGui::Spacing();
    }
    if (!any)
        ImGui::TextDisabled("%s", tr::tr(tr::TextId::ProgressNoMarkers, lang));
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
        if (rb && !prev_rb) { cur_tab = (cur_tab + 1) % 6; forced_tab = cur_tab; }
        if (lb && !prev_lb) { cur_tab = (cur_tab + 5) % 6; forced_tab = cur_tab; }
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
        {
            // Search tab title carries the pick count once anything is picked, e.g. "Search (12)".
            char stab[64];
            const size_t npick = goblin::search::pick_count();
            if (npick)
                std::snprintf(stab, sizeof stab, "%s (%zu)###tabsearch", tr::tr(tr::TextId::TabSearch, lang), npick);
            else
                std::snprintf(stab, sizeof stab, "%s###tabsearch", tr::tr(tr::TextId::TabSearch, lang));
            if (ImGui::BeginTabItem(stab, nullptr, tab_flag(2))) { draw_search_tab(); ImGui::EndTabItem(); }
        }
        {
            // Hidden-markers tab title carries the live count, e.g. "Hidden (3)".
            char htab[64];
            std::snprintf(htab, sizeof htab, "%s (%zu)###tabhidden",
                          tr::tr(tr::TextId::TabHidden, lang), goblin::manual_hidden_count());
            if (ImGui::BeginTabItem(htab, nullptr, tab_flag(3))) { draw_hidden_tab(); ImGui::EndTabItem(); }
        }
        if (ImGui::BeginTabItem(tr::tr(tr::TextId::TabDebug, lang),    nullptr, tab_flag(4))) { draw_debug_tab();    ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(tr::tr(tr::TextId::TabAbout, lang),    nullptr, tab_flag(5))) { draw_about_tab();    ImGui::EndTabItem(); }

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
// OWN-WINDOW ONLY: it is installed as wc.lpfnWndProc when that backend registers its window class,
// and the in-swapchain build has no window of its own to give it. Guarded from 2026-07-31; before
// that it compiled into every shipping DLL with no caller.
#if MFG_OVERLAY_OWN_WINDOW
LRESULT CALLBACK overlay_wndproc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return 0;
    switch (msg)
    {
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT)
        {
            // Use the trampoline (o_SetCursor), NOT SetCursor: the latter is hooked and would
            // re-capture whatever we set here as "the game's cursor".
            // Menu open: hide the OS cursor (we draw ImGui's software cursor).
            if (g_menu_open.load())
            {
                if (o_SetCursor) o_SetCursor(nullptr);
                return TRUE;
            }
            // Menu closed while the window is still shown. The two things that used to keep it up
            // with the menu closed - the on-map hover tooltip and the highlight image - were retired
            // on 2026-07-28, and the loop now shows the window only while `open && game_focused`, so
            // this is a defensive arm rather than a reachable state.
            // We must NOT fall through to DefWindowProc: with our window's class cursor null
            // it paints the OS "background app" busy ring (blue spinner) over the map. Assert
            // the game's real cursor instead - or a plain arrow until we've captured it, which
            // happens on the game's first SetCursor call. Either way, never the busy ring.
            HCURSOR gc = g_game_cursor.load();
            if (o_SetCursor) o_SetCursor(gc ? gc : LoadCursorA(nullptr, IDC_ARROW));
            return TRUE;
        }
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    case WM_DESTROY:
        return 0;
    default:
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
}

// (Same guard continues from overlay_wndproc above - the two regions were adjacent and are now
// one.) Built only with the own-window backend: every one of these needs g_d3d_device (created
// by init_d3d, which lives in that backend) and they are pumped from its loop. In the
// in-swapchain build they had no caller and no way to show anything.
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

// (guard continues)

// Upload one RGBA8 image to an immutable D3D11 texture + create its SRV. Returns
// false on a hard allocation failure. ImGui ImTextureID = the returned SRV ptr.
// It needs g_d3d_device, which only the own-window backend creates, and both of its callers
// (maybe_load_preview and try_upload_atlas) are own-window too - so it is inside the guard as of
// 2026-07-31. The note that used to sit on the old #endif here ("shared with the icon atlas", i.e.
// needed by every backend) stopped being true when the in-swapchain backend started blitting the
// atlas into ImGui's own font atlas instead - see sc2_build_context_fonts.
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

// (guard continues)
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
#endif // MFG_OVERLAY_OWN_WINDOW

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
        const int kind = b[2];
        int w = b[3] | (b[4] << 8), h = b[5] | (b[6] << 8);
        if (w <= 0 || h <= 0 || w > 1024 || h > 1024) continue;
        std::vector<unsigned char> px((size_t)w * h * 4);   // A,R',G',B' per pixel, premultiplied
        if (kind == 5)
        {
            mz_ulong destlen = (mz_ulong)px.size();
            if (mz_uncompress(px.data(), &destlen, b + 7, (mz_ulong)(tag->tagLen - 7)) != MZ_OK ||
                destlen != px.size())
                continue;
        }
        else if (kind == 3 && tag->tagLen > 8)
        {
            // Palette tag (MFG_ICON_PALETTE builds): a colour table of n premultiplied R,G,B,A entries,
            // then one index byte per pixel with rows padded to 4 bytes. Expanded into the same A,R,G,B
            // buffer the 32-bit branch fills, so the downscale below does not care which one shipped.
            const size_t n = (size_t)b[7] + 1, stride = ((size_t)w + 3) & ~(size_t)3;
            std::vector<unsigned char> raw(n * 4 + stride * (size_t)h);
            mz_ulong destlen = (mz_ulong)raw.size();
            if (mz_uncompress(raw.data(), &destlen, b + 8, (mz_ulong)(tag->tagLen - 8)) != MZ_OK ||
                destlen != raw.size())
                continue;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                    const size_t i = raw[n * 4 + (size_t)y * stride + x];
                    const unsigned char *e = &raw[(i < n ? i : 0) * 4];
                    unsigned char *d = &px[((size_t)y * w + x) * 4];
                    d[0] = e[3]; d[1] = e[0]; d[2] = e[1]; d[3] = e[2];
                }
        }
        else
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
//
// OWN-WINDOW ONLY, both of these. The in-swapchain backend has no D3D11 device to upload to: it
// blits the same atlas into ImGui's font atlas instead (sc2_build_context_fonts) and points
// g_icon_texid / g_logo_texid / g_highlight_texid at that one texture. build_atlas_rgba above stays
// OUTSIDE the guard - it is the shared source both backends decode.
#if MFG_OVERLAY_OWN_WINDOW
void try_upload_atlas()
{
    if (g_atlas_ready || !g_d3d_inited || !g_d3d_device)
        return;
    using namespace goblin::overlay_icons;
    std::vector<unsigned char> atlas = build_atlas_rgba();
    if (!atlas.empty())
        upload_rgba(atlas.data(), ATLAS_W, ATLAS_H, &g_atlas_tex, &g_atlas_srv);
    upload_rgba(LOGO_RGBA, LOGO_W, LOGO_H, &g_logo_tex, &g_logo_srv);
    upload_rgba(goblin::overlay_icons::HIGHLIGHT_RGBA, goblin::overlay_icons::HIGHLIGHT_W,
                goblin::overlay_icons::HIGHLIGHT_H, &g_highlight_tex, &g_highlight_srv);
    g_atlas_ready = true; // mark done even on partial failure (don't retry every frame)
    point_images_at_srvs(); // window modes draw straight from these; swapchain_2 overrides later
}

// The window modes (surface / layered / swapchain): every image comes from its own D3D11 view, uv
// untouched. Called once the atlas exists, and again if it is rebuilt.
void point_images_at_srvs()
{
    g_icon_texid = reinterpret_cast<ImTextureID>(g_atlas_srv);
    g_icon_uofs = 0.0f;
    g_icon_vofs = 0.0f;
    g_icon_uscl = 1.0f;
    g_icon_vscl = 1.0f;
    g_logo_texid = reinterpret_cast<ImTextureID>(g_logo_srv);
    g_logo_uv0 = ImVec2(0.0f, 0.0f);
    g_logo_uv1 = ImVec2(1.0f, 1.0f);
    g_highlight_texid = reinterpret_cast<ImTextureID>(g_highlight_srv);
    g_highlight_uv0 = ImVec2(0.0f, 0.0f);
    g_highlight_uv1 = ImVec2(1.0f, 1.0f);
}
#endif // MFG_OVERLAY_OWN_WINDOW

const goblin::overlay_icons::IconCell *find_icon_cell(const char *key)
{
    using namespace goblin::overlay_icons;
    for (int i = 0; i < ICON_CELL_COUNT; ++i)
        if (std::strcmp(ICON_CELLS[i].key, key) == 0)
            return &ICON_CELLS[i];
    return nullptr;
}

// The XInputGetState call under an SEH net. Report 42 (2.1.3): the call executed FREE memory -
// eldenring.exe imports xinput1_4 statically so the module itself cannot unload, but the chain
// behind our trampoline can (another mod hooked XInputGetState before us and was unloaded, or a
// proxy xinput). x64 unwinding treats a bad RIP as a leaf, so the fault comes back to this frame
// and the process lives; the caller retires gamepad polling for the session. Plain C in here on
// purpose: no objects with destructors may live in a __try frame.
static int xinput_fault_filter(EXCEPTION_POINTERS *ep, uintptr_t *where)
{
    if (ep && ep->ExceptionRecord)
        *where = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_EXECUTE_HANDLER;
}
static bool xinput_call_guarded(XInputGetState_t fn, DWORD idx, XINPUT_STATE *st, DWORD *rc,
                                uintptr_t *fault)
{
    __try
    {
        *rc = fn(idx, st);
        return true;
    }
    __except (xinput_fault_filter(GetExceptionInformation(), fault))
    {
        return false;
    }
}
// Name the owner of a faulting address for the log: a live module, or "unmapped" when the page
// is free (the report-42 shape), so a field log says which DLL went away.
static std::string xinput_fault_owner(uintptr_t where)
{
    HMODULE m = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(where), &m) && m)
    {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(m, path, MAX_PATH);
        const char *base = strrchr(path, '\\');
        return base ? base + 1 : path;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(where), &mbi, sizeof mbi) && mbi.State == MEM_FREE)
        return "unmapped (a module that was unloaded)";
    return "no module";
}
static void xinput_retire(const char *site, uintptr_t where)
{
    if (g_pad_poll_dead.exchange(true))
        return;
    spdlog::warn("[OVERLAY] {}: XInputGetState faulted at 0x{:X} ({}); gamepad polling is off "
                 "for this session, keyboard hotkeys keep working",
                 site, where, xinput_fault_owner(where));
}

// ── Gamepad nav: poll directly (no hook). Picks the first active controller. ──
void poll_gamepad()
{
    g_pad_ok = false;
    if (g_pad_poll_dead.load(std::memory_order_relaxed))
        return;
    // Read the REAL pad through the trampoline (o_) if we hooked XInputGetState; the hook
    // returns "disconnected" to the GAME while the menu is open, but not through o_.
    XInputGetState_t xget = o_XInputGetState ? o_XInputGetState : pXInputGetState;
    if (!xget)
        return;
    for (DWORD idx = 0; idx < XUSER_MAX_COUNT; ++idx)
    {
        XINPUT_STATE state{};
        DWORD rc = ERROR_DEVICE_NOT_CONNECTED;
        uintptr_t fault = 0;
        if (!xinput_call_guarded(xget, idx, &state, &rc, &fault))
        {
            xinput_retire("poll", fault);
            return;
        }
        if (rc == ERROR_SUCCESS)
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

// Polled TEXT entry for the in-swapchain backend. We never hold focus, so no WM_CHAR ever
// reaches ImGui and feed_nav_keyboard covers only the nav keys. The game reads typed text the
// same way we do here (a keyboard-state snapshot translated per key); we use ToUnicodeEx with
// the GAME thread's layout, so a Cyrillic or accented layout types its own letters, not the
// US ones. Characters are produced only while a text field is active (io.WantTextInput); the
// editing keys and modifiers are fed whenever the menu is up. The raw-input hook already keeps
// every key away from the game while the menu is open, so typing cannot move the player.
static void feed_text_keyboard(HWND game)
{
    ImGuiIO &io = ImGui::GetIO();
    struct KeyMap { int vk; ImGuiKey key; };
    static const KeyMap edit[] = {
        {VK_BACK, ImGuiKey_Backspace}, {VK_DELETE, ImGuiKey_Delete},
        {VK_HOME, ImGuiKey_Home},      {VK_END, ImGuiKey_End},
        {VK_SHIFT, ImGuiMod_Shift},    {VK_CONTROL, ImGuiMod_Ctrl},
        {'A', ImGuiKey_A}, {'C', ImGuiKey_C}, {'V', ImGuiKey_V}, {'X', ImGuiKey_X}, // Ctrl+A/C/V/X
    };
    static bool prev[sizeof(edit) / sizeof(edit[0])] = {};
    for (size_t i = 0; i < sizeof(edit) / sizeof(edit[0]); ++i)
    {
        const bool down = kd(edit[i].vk);
        if (down != prev[i])
        {
            io.AddKeyEvent(edit[i].key, down);
            prev[i] = down;
            if (down) g_last_input.store(0, std::memory_order_relaxed);
        }
    }

    // Per-key repeat clock: 0 = up; else the tick at which the next repeat fires.
    static uint64_t s_next[256] = {};
    // Only while the GAME is the foreground window: GetAsyncKeyState is global, and an alt-tabbed
    // player typing into another program must not type into our field too.
    DWORD fg_pid = 0;
    if (HWND fg = GetForegroundWindow()) GetWindowThreadProcessId(fg, &fg_pid);
    if (!io.WantTextInput || kd(VK_CONTROL) || fg_pid != GetCurrentProcessId())
    {
        for (auto &t : s_next) t = 0;
        return;
    }
    const uint64_t now = GetTickCount64();
    (void)game;
    goblin::overlay::text_layout_poll(); // Alt+Shift etc. cycle OUR layout (the game's thread never switches)
    const HKL layout = reinterpret_cast<HKL>(goblin::overlay::text_layout());
    BYTE ks[256] = {};
    if (kd(VK_SHIFT)) ks[VK_SHIFT] = 0x80;
    if (kd(VK_RMENU)) { ks[VK_CONTROL] = 0x80; ks[VK_MENU] = 0x80; ks[VK_RMENU] = 0x80; } // AltGr
    if (GetKeyState(VK_CAPITAL) & 1) ks[VK_CAPITAL] = 1;
    for (int vk = VK_SPACE; vk <= VK_OEM_102; ++vk)
    {
        if (!goblin::overlay::text_key(vk)) continue;
        if (!kd(vk)) { s_next[vk] = 0; continue; }
        if (s_next[vk] == 0) s_next[vk] = now + 400;      // first press: emit now, repeat after 400 ms
        else if (now < s_next[vk]) continue;
        else s_next[vk] = now + 40;                        // then every 40 ms
        wchar_t out[8] = {};
        const UINT sc = MapVirtualKeyExW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC, layout);
        // Bit 2 = do not disturb the keyboard state (dead keys) - Windows 10 1607+; older
        // systems ignore the flag.
        const int n = ToUnicodeEx(static_cast<UINT>(vk), sc, ks, out, 8, 1u << 2, layout);
        for (int i = 0; i < n; ++i)
            if (out[i] >= 0x20) io.AddInputCharacterUTF16(static_cast<ImWchar16>(out[i]));
        if (n > 0) g_last_input.store(0, std::memory_order_relaxed);
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
// Own-window only: it moves g_hwnd, and the in-swapchain build never creates one. (find_game_window
// above stays outside the guard - the focus test uses it in every backend.)
#if MFG_OVERLAY_OWN_WINDOW
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
#endif // MFG_OVERLAY_OWN_WINDOW

// ── Our own window's three backends: NOT BUILT unless MFG_OVERLAY_OWN_WINDOW=1 ──────────────────
// Everything from here to the raw-input hook exists only to get our ImGui onto the screen through a
// window of our own: the Wine/GDI layered blit, the DComp surface, the DComp swapchain, the D3D11
// device they share, and render_frame() which presents whichever one is active. The shipping build
// draws into the game's own frame instead (sc2_frontend_loop), so none of this is compiled in.
#if MFG_OVERLAY_OWN_WINDOW

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

    // 2) Render mode: a BUILD choice, not an ini one. It used to read the ini key that is
    // now `menu_render_mode` (native/imgui/dev), and load_config() rewrites anything else
    // to `native`, so "surface"/"swapchain" could never arrive here again - the own-window
    // backend would silently always be Layered. One key cannot carry two meanings; pick the
    // backend with MFG_OWN_WINDOW_MODE at compile time (goblin_build_variants.hpp), which is
    // also what makes this variant's switch a rebuild rather than a hidden string compare.
    g_render_mode = static_cast<RenderMode>(MFG_OWN_WINDOW_MODE);
    // Proton/Wine (Steam Deck): DirectComposition is unreliable - instead of a clean E_NOTIMPL
    // it can partially "work" then misbehave: the DComp surface's size/placement desyncs from
    // the gamescope-composited game window, so the menu opens shifted and DRAGGING it churns the
    // compositor (the game window collapses to a few px and loses input). Force the pure-GDI
    // layered path there (UpdateLayeredWindow, no DComp/swapchain), overriding the ini.
    {
        HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        const bool is_wine = nt && GetProcAddress(nt, "wine_get_version") != nullptr;
        if (is_wine && g_render_mode != RenderMode::Layered)
        {
            spdlog::info("[OVERLAY] Wine/Proton detected -> forcing layered render mode (DComp unreliable)");
            g_render_mode = RenderMode::Layered;
        }
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
                b.AddRanges(io.Fonts->GetGlyphRangesVietnamese());
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
                const char *cjk_ko[] = {"C:\\Windows\\Fonts\\malgun.ttf", "C:\\Windows\\Fonts\\malgun.ttc",
                                        "C:\\Windows\\Fonts\\msyh.ttc", "C:\\Windows\\Fonts\\msjh.ttc"};
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
#endif // MFG_OVERLAY_OWN_WINDOW - the hover-row helper below is needed by every backend

// One rendered frame (SEH-wrapped). Clears to transparent; draws ImGui only when
// the menu is open; presents. POD-only locals.
// Passive hover-info panel (plan_3 Step 1). While the world map is open and the
// cursor is over one of OUR markers, show a small fixed top-left panel with the
// marker's name + its height relative to the player. Never captures input; renders
// in the menu-closed path so it coexists with normal play.
// Our V3 native markers have no engine pin, so the game's hover routine can
// never report them. Equivalent focus test: project every visible native
// marker of the current layer and pick the one nearest the map reticle
// (screen centre), within a pin-sized radius. Public wrapper
// goblin::overlay::native_hover_row() lives past the anonymous namespace.
static void *native_hover_row_impl()
{
    // One pick, one anchor. This used to run its own nearest-to-the-screen-centre search, which meant
    // the manual-hide hotkey and the popup could disagree about which marker is under the reticle - and
    // on a build whose reticle is not centred, both were wrong in the same way. goblin::native_reticle_row
    // is that search, in map space, with the anchor measured from the game's own hover.
    return goblin::native_reticle_row();
}

// RETIRED 2026-07-28: draw_hover_tooltip / draw_focus_banner_onscreen / draw_map_highlights.
// All three had native equivalents by then and were being drawn ON TOP of them:
//   * the hover panel  -> the MfgTip panel   (goblin_maphover.cpp, drive_own_tip)
//   * the focus banner -> the MfgBanner panel (goblin_maphover.cpp, drive_own_banner)
//   * the highlight rings -> the glow-icon swap in goblin::apply_focus_highlight()
// so the overlay now renders NOTHING but the F10 menu. What was lost with them, deliberately: the
// tooltip's marker NAME line (the game's own popup already shows the name, which is why the native
// panel never repeated it) and the ring drawn per projected point (the glow icon marks the same
// markers, in the game's own render, with no projection to drift).
// Consequences worth knowing before reviving any of this: the world->screen projection
// (goblin_mapproject) has no overlay consumer left, HIGHLIGHT_PX is gone, and g_highlight_tex /
// g_highlight_srv / g_highlight_texid / g_highlight_uv* are now written but never read - the atlas
// rect and the D3D11 texture are still built, which is a few KB of waste kept on purpose so this
// change does not touch atlas packing. Details: docs/research_overlay_map_visuals_retired.md.

#if MFG_OVERLAY_OWN_WINDOW
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

#endif // MFG_OVERLAY_OWN_WINDOW

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

// Native-menu text capture (goblin::overlay::set_text_capture): mute only the typeable keys.
std::atomic<bool> g_text_capture{false};

UINT WINAPI hk_GetRawInputData(HRAWINPUT hri, UINT cmd, LPVOID data, PUINT size, UINT hsz)
{
    UINT r = o_GetRawInputData(hri, cmd, data, size, hsz);
    // Only touch the actual data fetch (data != null); leave the size query alone.
    const bool menu = g_menu_open.load();
    const bool capture = !menu && g_text_capture.load(std::memory_order_relaxed);
    if ((menu || capture) && data && cmd == RID_INPUT && r != static_cast<UINT>(-1))
    {
        RAWINPUT *ri = reinterpret_cast<RAWINPUT *>(data);
        if (capture)
        {
            // The native search page is typing: swallow the letters, digits, space, punctuation
            // and Backspace; everything else (Enter, Escape, arrows, the mouse) stays the game's.
            if (ri->header.dwType == RIM_TYPEKEYBOARD)
            {
                const int vk = ri->data.keyboard.VKey;
                if (vk == VK_BACK || goblin::overlay::text_key(vk))
                {
                    ri->data.keyboard.MakeCode = 0;
                    ri->data.keyboard.VKey = 0;
                    ri->data.keyboard.Message = WM_NULL;
                    ri->data.keyboard.Flags = RI_KEY_BREAK;
                }
            }
            return r;
        }
        if (ri->header.dwType == RIM_TYPEMOUSE)
        {
            // Take the wheel before neutralising the record: this is the only place it exists.
            // usButtonData carries a SIGNED delta in WHEEL_DELTA units, so it must be read as a
            // short - as a USHORT every scroll-down becomes a large positive number.
            if (menu && (ri->data.mouse.usButtonFlags & RI_MOUSE_WHEEL))
                g_wheel_raw.fetch_add(static_cast<short>(ri->data.mouse.usButtonData),
                                      std::memory_order_relaxed);
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

// Remember the GAME's real cursor so we can restore it when the menu closes. While the
// menu is open our WM_SETCURSOR sets the OS cursor to NULL (hidden) so only ImGui's
// software cursor shows; the game does NOT re-set its cursor until a state change (e.g.
// reopening the map), so without this the cursor stays invisible after closing the menu.
// We record every non-null SetCursor the game makes and re-apply it on close.
HCURSOR WINAPI hk_SetCursor(HCURSOR c)
{
    if (c) g_game_cursor.store(c);  // track the game's real cursor (ignore our own null-hide)
    return o_SetCursor ? o_SetCursor(c) : c;
}

// While the menu is open, feed the GAME a NEUTRAL pad (connected, nothing pressed) so its
// buttons do not leak into gameplay. We keep the real connect status + packet number so
// the game keeps polling the slot - returning ERROR_DEVICE_NOT_CONNECTED makes games drop
// the pad and stop polling it, killing the gamepad even after the menu closes. Our own
// poll_gamepad reads the real pad via the o_ trampoline.
// Is our open-combo held right now, and should its buttons be kept from the game?
// LATCHED: once the full combo is seen, its bits stay hidden until EVERY one of them is
// released. Without the latch the game would see a release edge the moment we start hiding,
// and a release edge is an action too.
std::atomic<bool> g_combo_latched{false};
// While the MAP is open the combo's buttons are held back from the game for a moment before it
// may see them, so a chord that is on its way can still win. Long enough to cover a human's
// two-button press, short enough that a deliberate single press does not feel broken.
constexpr uint64_t kComboGraceMs = 120;
// A tap SHORTER than the grace was hidden for its whole life and would otherwise be lost - the
// player would press Y on the map and nothing at all would happen. So it is not swallowed, it is
// DELAYED: on release we hand the game the press it never saw, held on for this long so it reads
// as a real press followed by a release across several polls.
constexpr uint64_t kComboReplayMs = 80;
uint64_t g_combo_pending_since = 0; // 0 = no partial chord in flight
WORD g_combo_pending_bits = 0;      // what we are holding back, so a short tap can be replayed
bool g_combo_gave_up = false;       // grace expired: pass the buttons through until all are up
uint64_t g_combo_replay_until = 0;  // 0 = not replaying a swallowed tap
WORD g_combo_replay_bits = 0;

// Returns the combo bits to KEEP FROM the game; `force_on` comes back with bits to hand it
// instead (the delayed replay of a tap too short to have been passed through live).
static uint16_t combo_bits_to_hide(WORD held, WORD &force_on)
{
    const uint16_t mask = goblin::config::toggleGamepadMask;
    // Only in the native menu mode: there the combo opens OUR in-game screen, and the same two
    // buttons are live game actions on the map (the reporter's Y + R3 both do something there).
    // In the ImGui backend the branch above hides the whole pad anyway.
    if (!mask || !goblin::config::menuEnabled || goblin::config::overlay_menu_enabled())
        return 0;

    const WORD in_combo = static_cast<WORD>(held & mask);
    const uint64_t tick = GetTickCount64();
    if (in_combo == 0)
    {
        // Nothing held. If a partial chord was still inside its grace when it ended, the game never
        // saw that press at all - owe it back now rather than lose it. (Not owed if the grace had
        // already expired: the buttons were passed through live from that moment.)
        if (g_combo_pending_since != 0 && !g_combo_gave_up &&
            !g_combo_latched.load(std::memory_order_relaxed))
        {
            g_combo_replay_bits = g_combo_pending_bits;
            g_combo_replay_until = tick + kComboReplayMs;
        }
        g_combo_latched.store(false, std::memory_order_relaxed);
        g_combo_pending_since = 0;
        g_combo_pending_bits = 0;
        g_combo_gave_up = false;
        if (g_combo_replay_until != 0)
        {
            if (tick < g_combo_replay_until)
            {
                force_on = g_combo_replay_bits;  // the delayed press, on its way to the game
                return 0;
            }
            g_combo_replay_until = 0;            // and its release edge
            g_combo_replay_bits = 0;
        }
        return 0;
    }
    g_combo_replay_until = 0;  // a new press supersedes any replay still in flight
    g_combo_replay_bits = 0;
    if (in_combo == mask)
    {
        g_combo_latched.store(true, std::memory_order_relaxed);
        g_combo_pending_since = 0;
        g_combo_pending_bits = 0;
        g_combo_gave_up = false;
        return mask;
    }
    if (g_combo_latched.load(std::memory_order_relaxed))
        return mask; // chord seen; stay hidden until every button is released

    // PARTIAL chord. Off the map, let it through at once - the whole point of hiding is that the
    // map binds both of our buttons to its own actions, and elsewhere the player would only feel
    // an unexplained delay. On the map, hold them back briefly: the reporter could open the menu
    // with R3+Y but not Y+R3, because Y alone had already done its map thing before R3 landed.
    if (!goblin::maphover::map_dialog())
    {
        // Also covers the map CLOSING with a partial chord still held: from here the buttons reach
        // the game live, so nothing is owed and the release must not replay a press it already saw.
        g_combo_pending_since = 0;
        g_combo_pending_bits = 0;
        return 0;
    }
    if (g_combo_gave_up)
        return 0;
    if (g_combo_pending_since == 0)
        g_combo_pending_since = tick;
    g_combo_pending_bits = in_combo;  // remember it, in case this turns out to be a short tap
    if (tick - g_combo_pending_since < kComboGraceMs)
        return mask;      // still might become our chord - the game waits
    g_combo_gave_up = true;  // it was a real single press; hand it over (late, by that grace)
    return 0;
}

DWORD WINAPI hk_XInputGetState(DWORD idx, XINPUT_STATE *state)
{
    // The GAME's thread is in here. The same dead chain that report 42 hit from our poll thread
    // would hit the game's call next, so the trampoline call gets the same net: a fault answers
    // "no controller" and retires the pad for the session instead of ending the process.
    if (g_pad_poll_dead.load(std::memory_order_relaxed))
        return ERROR_DEVICE_NOT_CONNECTED;
    DWORD r = ERROR_DEVICE_NOT_CONNECTED;
    uintptr_t fault = 0;
    if (!xinput_call_guarded(o_XInputGetState, idx, state, &r, &fault))
    {
        xinput_retire("game call", fault);
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    if (r != ERROR_SUCCESS || !state)
        return r;
    if (g_menu_open.load())
    {
        state->Gamepad = XINPUT_GAMEPAD{}; // zero buttons + centre sticks; keep connected
        return r;
    }
    // KNOWN AND UNAVOIDABLE HERE: a combo is not atomic. Between the first button going down and
    // the second landing, the game has already seen the first one - so a chord whose members are
    // both live game actions can still fire one of them on the way in. Hiding starts the frame the
    // chord completes. Removing that window needs the buttons to be BUFFERED for ~100 ms before the
    // game sees them at all, which costs every normal press that much latency; not done without a
    // decision. Binding the combo to a button the map does not use avoids it entirely.
    WORD force_on = 0;
    if (const uint16_t hide = combo_bits_to_hide(state->Gamepad.wButtons, force_on))
        state->Gamepad.wButtons &= static_cast<WORD>(~hide);
    state->Gamepad.wButtons |= force_on;
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
    // A key that was JUST bound in the native menu is still held down; acting on it here would open
    // the overlay the instant the player finished binding it to the overlay.
    const bool just_bound = goblin::nmenu::key_swallowed(static_cast<uint32_t>(open_key));
    if (open_in && !prev_open_in && !rebinding && !just_bound)
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
        // Window show/topmost is handled per-frame in the render loop (it must also
        // show for the passive hover panel while the menu is closed).
    }
    else if (!open_now && prev_open)
    {
        reset_rebind_state();
        goblin::save_config(goblin::g_ini_path);
        // Restore the game's cursor: our WM_SETCURSOR hid the OS cursor (SetCursor NULL)
        // while the menu was open, and the game won't re-set it until its next state
        // change (map reopen). Re-apply the last cursor the game used so it's visible
        // immediately on close (fixes the "cursor gone / system arrow until map reopen").
        if (o_SetCursor && g_game_cursor.load())
            o_SetCursor(g_game_cursor.load());
    }
    prev_open = open_now;
}

// ── The overlay thread: window + D3D11 + DComp + ImGui + render loop ──
// ── sc2 (backend v2) frontend: in-swapchain D3D12 overlay ──
// In the shipped build we do NOT create our own window / D3D11
// / DComp. We install the ported in-swapchain backend (src/sc2/), which draws our
// published ImGui packet into the game's OWN backbuffer just before its Present,
// so there is NO separate top-level window -> no focus-steal (the Linux/Proton
// fix) and no second swapchain. Under ERR + a mod loader we always load after the
// game is already presenting, so discovery goes through late adoption.
//
// Custom images DO draw in this mode. The packet still carries a single texture, so
// rather than add a second one the category icons, the logo and the highlight are
// PACKED INTO THE FONT ATLAS itself (AddCustomRectRegular + a blit per rect, below),
// and g_icon_texid/g_logo_texid/g_highlight_texid all point at that one atlas. That is
// why g_atlas_ready is set here too - the existing icon guards are meant to pass.
// The one exception is the dev-only Icon Preview, which needs a texture of its own and
// is built with the own-window backend.

// Build the ImGui context + fonts WITHOUT a platform/renderer backend (the sc2
// D3D12 renderer consumes the packet). Mirrors init_d3d's context/font setup so
// text + Cyrillic + optional CJK match the windowed modes; kept separate so the
// shipped layered/surface/swapchain paths stay untouched.
static void sc2_build_context_fonts()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
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
            b.AddRanges(io.Fonts->GetGlyphRangesVietnamese());
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
            spdlog::warn("[SC2] no base system font found; text may show as '?'");
        }
        if (need_cjk && base)
        {
            static ImVector<ImWchar> cjk_ranges;
            {
                ImFontGlyphRangesBuilder b;
                b.AddText(goblin::i18n::font_glyph_seed_utf8());
                b.BuildRanges(&cjk_ranges);
            }
            ImFontConfig cfg;
            cfg.MergeMode = true;
            const char *cjk_sc[] = {"C:\\Windows\\Fonts\\msyh.ttc", "C:\\Windows\\Fonts\\msjh.ttc",
                                    "C:\\Windows\\Fonts\\simhei.ttf", "C:\\Windows\\Fonts\\simsun.ttc"};
            const char *cjk_tc[] = {"C:\\Windows\\Fonts\\msjh.ttc", "C:\\Windows\\Fonts\\msyh.ttc",
                                    "C:\\Windows\\Fonts\\simsun.ttc", "C:\\Windows\\Fonts\\simhei.ttf"};
            const char *cjk_ko[] = {"C:\\Windows\\Fonts\\malgun.ttf", "C:\\Windows\\Fonts\\malgun.ttc",
                                    "C:\\Windows\\Fonts\\msyh.ttc", "C:\\Windows\\Fonts\\msjh.ttc"};
            const char *const *cjk_fonts = (ui_lang == goblin::i18n::Language::TraditionalChinese) ? cjk_tc
                                          : (ui_lang == goblin::i18n::Language::Korean)            ? cjk_ko
                                                                                                   : cjk_sc;
            bool merged = false;
            for (int i = 0; i < 4; ++i)
            {
                const char *fp = cjk_fonts[i];
                if (GetFileAttributesA(fp) != INVALID_FILE_ATTRIBUTES &&
                    io.Fonts->AddFontFromFileTTF(fp, 18.0f, &cfg, cjk_ranges.Data))
                { merged = true; break; }
            }
            if (!merged)
                spdlog::warn("[SC2] no CJK font found; CJK UI may show as '?'");
        }
    }
    apply_er_style();

    // Merge our category-icon atlas + the logo into the FONT atlas as custom rects.
    // The sc2 packet only maps the font-texture token, so this is how icons reach
    // the in-swapchain D3D12 renderer WITHOUT extending the renderer/packet: the
    // draw helpers emit ImGui::Image with the font token + a uv transform into
    // these rects. Rects must be reserved BEFORE the atlas builds; we then force a
    // build and blit our straight-alpha RGBA into the reserved regions.
    {
        using namespace goblin::overlay_icons;
        const int icon_rect = io.Fonts->AddCustomRectRegular(ATLAS_W, ATLAS_H);
        const int logo_rect = (LOGO_W > 0 && LOGO_H > 0)
                                  ? io.Fonts->AddCustomRectRegular(LOGO_W, LOGO_H)
                                  : -1;
        const int hl_rect = (HIGHLIGHT_W > 0 && HIGHLIGHT_H > 0)
                                ? io.Fonts->AddCustomRectRegular(HIGHLIGHT_W, HIGHLIGHT_H)
                                : -1;
        unsigned char *pix = nullptr; int fw = 0, fh = 0, bpp = 0;
        io.Fonts->GetTexDataAsRGBA32(&pix, &fw, &fh, &bpp);
        if (pix && fw > 0 && fh > 0 && bpp == 4)
        {
            std::vector<unsigned char> icons = build_atlas_rgba();
            if (const ImFontAtlasCustomRect *rc = io.Fonts->GetCustomRectByIndex(icon_rect))
            {
                if (!icons.empty())
                    for (int y = 0; y < rc->Height; ++y)
                        std::memcpy(pix + (static_cast<size_t>(rc->Y + y) * fw + rc->X) * 4,
                                    icons.data() + static_cast<size_t>(y) * ATLAS_W * 4,
                                    static_cast<size_t>(ATLAS_W) * 4);
                g_icon_uofs = rc->X / static_cast<float>(fw);
                g_icon_vofs = rc->Y / static_cast<float>(fh);
                g_icon_uscl = ATLAS_W / static_cast<float>(fw);
                g_icon_vscl = ATLAS_H / static_cast<float>(fh);
            }
            if (logo_rect >= 0)
                if (const ImFontAtlasCustomRect *rl = io.Fonts->GetCustomRectByIndex(logo_rect))
                {
                    for (int y = 0; y < rl->Height; ++y)
                        std::memcpy(pix + (static_cast<size_t>(rl->Y + y) * fw + rl->X) * 4,
                                    LOGO_RGBA + static_cast<size_t>(y) * LOGO_W * 4,
                                    static_cast<size_t>(LOGO_W) * 4);
                    g_logo_uv0 = ImVec2(rl->X / static_cast<float>(fw),
                                        rl->Y / static_cast<float>(fh));
                    g_logo_uv1 = ImVec2((rl->X + LOGO_W) / static_cast<float>(fw),
                                        (rl->Y + LOGO_H) / static_cast<float>(fh));
                }
            if (hl_rect >= 0)
                if (const ImFontAtlasCustomRect *rh = io.Fonts->GetCustomRectByIndex(hl_rect))
                {
                    for (int y = 0; y < rh->Height; ++y)
                        std::memcpy(pix + (static_cast<size_t>(rh->Y + y) * fw + rh->X) * 4,
                                    HIGHLIGHT_RGBA + static_cast<size_t>(y) * HIGHLIGHT_W * 4,
                                    static_cast<size_t>(HIGHLIGHT_W) * 4);
                    g_highlight_uv0 = ImVec2(rh->X / static_cast<float>(fw),
                                             rh->Y / static_cast<float>(fh));
                    g_highlight_uv1 = ImVec2((rh->X + HIGHLIGHT_W) / static_cast<float>(fw),
                                             (rh->Y + HIGHLIGHT_H) / static_cast<float>(fh));
                }
            g_atlas_ready = true; // icon/logo/highlight guards may now pass in sc2
        }
    }
}

// Feed the mouse to ImGui by polling (focus-free, no window): cursor position
// mapped into the game's client area (== the swapchain canvas) + buttons via
// async key state. The game's own reading of these is already neutralized by our
// GetRawInputData hook while the menu is open, so there is no double-acting.
static void sc2_feed_mouse(HWND game)
{
    ImGuiIO &io = ImGui::GetIO();
    POINT p{};
    if (GetCursorPos(&p) && game && ScreenToClient(game, &p))
        io.AddMousePosEvent(static_cast<float>(p.x), static_cast<float>(p.y));
    io.AddMouseButtonEvent(0, (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
    io.AddMouseButtonEvent(1, (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);
    io.AddMouseButtonEvent(2, (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0);
    // The wheel, drained from what the raw-input hook collected since the last frame. One ImGui
    // notch is one WHEEL_DELTA; accumulating first means a fast flick still lands whole.
    if (const int wheel = g_wheel_raw.exchange(0, std::memory_order_relaxed))
        io.AddMouseWheelEvent(0.0f, static_cast<float>(wheel) / static_cast<float>(WHEEL_DELTA));
}

static void sc2_frontend_loop()
{
    // g_hinst MUST be OUR DLL's module, not the EXE: present::install_hooks() pins
    // the module containing its own code and refuses interception unless it equals
    // g_hinst. GetModuleHandleW(nullptr) returns the game EXE -> mismatch -> "could
    // not pin hook module". Resolve our real module from an address inside this DLL.
    {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&sc2_frontend_loop), &self);
        cte::g_hinst = reinterpret_cast<HINSTANCE>(self);
    }
    spdlog::info("[OVERLAY] render mode = swapchain_2 (in-swapchain D3D12 backend)");

    if (!cte::hooks::init())
        spdlog::warn("[SC2] MinHook init reported failure (may already be initialized)");
    if (!cte::overlay::present::install_hooks())
        spdlog::error("[SC2] present::install_hooks() failed; in-swapchain backend unavailable");
    else
        spdlog::info("[SC2] DXGI discovery hooks installed");

    // Frontend context + fonts, then publish the atlas once (the sc2 D3D12 renderer
    // uploads it from the published RGBA and assigns the font texture token).
    sc2_build_context_fonts();
    cte::overlay::present::publish_font_atlas(ImGui::GetIO().Fonts);
    // publish_font_atlas set io.Fonts->TexID to the font token. Icons + logo live
    // in that same (merged) atlas, so point their draw helpers at the font token -
    // their Image commands then carry the only texture the packet maps, and the uv
    // transform (set in sc2_build_context_fonts) samples the merged rects.
    g_icon_texid = ImGui::GetIO().Fonts->TexID;
    g_logo_texid = ImGui::GetIO().Fonts->TexID;
    g_highlight_texid = ImGui::GetIO().Fonts->TexID;

    bool armed = false;
    ULONGLONG last_log = 0;
    ULONGLONG last_tick = GetTickCount64();
    while (g_running.load())
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        { TranslateMessage(&msg); DispatchMessageW(&msg); }

        HWND game = find_game_window();
        if (!armed && game && !cte::overlay::present::observed_swapchain_creation())
        {
            spdlog::info("[SC2] late load: game window predates our DXGI hooks; arming live-swapchain adoption");
            cte::overlay::present::log_creation_hook_forensics();
            cte::overlay::present::arm_adoption(game);
            armed = true;
        }
        if (armed)
            cte::overlay::present::service_adoption();

        poll_gamepad();
        update_menu_toggle();

        const auto canvas = cte::overlay::present::canvas();
        const bool canvas_ok = canvas.ready && canvas.width > 0 && canvas.height > 0;

        const bool open = g_menu_open.load();
        static bool s_was_open = false;
        if (s_was_open && !open)
            cte::overlay::present::log_render_stats(); // the tail of a short open, before the 3 s tick
        s_was_open = open;
        // The overlay now draws NOTHING but the F10 menu. The hover tooltip, the focus banner and
        // the projected highlight rings were retired on 2026-07-28: all three exist natively (the
        // MfgTip / MfgBanner panels in goblin_maphover.cpp and the glow-icon swap in
        // apply_focus_highlight), so the overlay copies were duplicates drawn on top of them.
        // Two things fall out of that, both wanted: with the menu closed there is nothing to show,
        // so the overlay is simply not visible; and the per-loop hover lookup is gone - it existed
        // only to feed the tooltip, and its heavy variant was once responsible for a map FPS
        // collapse (it rebuilt the ~9k-row snapshot with per-row event-flag reads every call).
        const bool want = open;

        const ULONGLONG now = GetTickCount64();
        if (now - last_log > 3000)
        {
            last_log = now;
            spdlog::info("[SC2] observed_creation={} canvas.ready={} {}x{} healthy={}",
                         cte::overlay::present::observed_swapchain_creation(),
                         canvas.ready, canvas.width, canvas.height,
                         cte::overlay::present::renderer_healthy());
            if (open)
                cte::overlay::present::log_render_stats();
        }

        if (!want || !canvas_ok)
        {
            cte::overlay::present::set_visible(false);
            last_tick = now;
            Sleep(16);
            continue;
        }

        ImGuiIO &io = ImGui::GetIO();
        io.DisplaySize = ImVec2(static_cast<float>(canvas.width), static_cast<float>(canvas.height));
        // g_back_w/g_back_h are NOT written here any more. They existed for the world->screen
        // projection that draw_map_highlights used to do, and that function went away with the
        // rest of the on-map overlay drawing (2026-07-28). Every remaining reader of the pair
        // lives inside the own-window backend, which writes them itself; feeding them from the
        // sc2 loop only meant the two backends could disagree about who owns the value.
        const float dt = (now - last_tick) / 1000.0f;
        last_tick = now;
        io.DeltaTime = dt > 0.0f ? dt : (1.0f / 60.0f);
        io.AddFocusEvent(true);

        sc2_feed_mouse(game);
        feed_nav_keyboard();
        feed_text_keyboard(game); // typed characters for the search field (polled, focus-free)
        feed_gamepad();
        if (open) { poll_rebind_keyboard(); process_rebind(); }

        ImGui::NewFrame();
        {
            const float fs = goblin::config::fontScale;
            io.FontGlobalScale = fs < 0.8f ? 0.8f : (fs > 3.0f ? 3.0f : fs);
        }
        // Only an OPEN menu reaches this point - a closed one took the `!want` continue above,
        // and `open` cannot change in between. The `else` that cleared MouseDrawCursor and the
        // later "closed and empty" guard were both left over from when the overlay also painted
        // focus rings and the hover tooltip with the menu closed (retired 2026-07-28); neither
        // could be taken any more.
        io.MouseDrawCursor = true;
        draw_settings_window();

        ImGui::Render();
        const ImDrawData *draw_data = ImGui::GetDrawData();
        cte::overlay::present::publish_draw_data(draw_data);
        cte::overlay::present::set_visible(true);

        Sleep(16); // ~60 Hz producer; shipped modes are vsync-throttled to a
                   // similar rate by their present, so match it (the game renders
                   // our published packet at its own rate regardless).
    }
    cte::overlay::present::set_visible(false);
    cte::overlay::present::shutdown();
    if (ImGui::GetCurrentContext())
        ImGui::DestroyContext();
}

void overlay_thread()
{
#if !MFG_OVERLAY_OWN_WINDOW
    // ONE backend is built (goblin_build_variants.hpp): our ImGui goes into the game's own frame,
    // with no window, no D3D11 device and no DirectComposition of ours. no ini value chooses
    // between backends any more; menu_render_mode picks which MENU runs, not how it is drawn.
    sc2_frontend_loop();
    return;
}
#else
    // swapchain_2: no window, no D3D11, no DComp - our ImGui goes into the game's own swapchain.
    // Taken before any of the window setup below, so the shipped surface/layered/swapchain modes
    // run exactly the code they always did.
    if (false) // historical: the value that used to select this backend now names a MENU
    {
        sc2_frontend_loop();
        return;
    }
    pXInputGetState = nullptr;
    {
        // LoadLibraryA on every candidate, first hit wins. On a module that is already loaded
        // (the game imports xinput1_4 statically) this only adds our reference and returns the
        // same HMODULE; on one that is not, it loads it. Either way the module we hand a pointer
        // and a hook into cannot be unloaded while we hold it (report 42).
        const char *xdlls[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
        for (const char *d : xdlls)
            if (HMODULE h = LoadLibraryA(d))
            {
                if (auto p = reinterpret_cast<XInputGetState_t>(GetProcAddress(h, "XInputGetState")))
                {
                    pXInputGetState = p;
                    g_xinput_module = h;
                    spdlog::info("[OVERLAY] XInputGetState from {} at 0x{:X}", d,
                                 reinterpret_cast<uintptr_t>(p));
                    break;
                }
                FreeLibrary(h); // no export here: give the reference back and try the next name
            }
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
        return;
    }
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);

    if (!seh_init_d3d())
    {
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
        // Nothing but the F10 menu is drawn here any more - see the note in the sc2 loop above.
        // Only paint while the game (a window in our own process) is the foreground
        // app. Our window is HWND_TOPMOST, so without this an alt-tab to another app
        // would leave the menu/hover panel drawn over whatever is now in front, with
        // no cursor. On focus loss we just hide (menu stays "open"); on return the
        // panel/cursor come back on their own - no need to close and reopen.
        DWORD fg_pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &fg_pid);
        const bool game_focused = (fg_pid == GetCurrentProcessId());
        // Window visibility is driven here (not the menu-toggle edge handler): the
        // overlay window must also be shown for the passive hover-info panel, which
        // appears while the menu is CLOSED. Shown = (menu open OR marker hovered) AND
        // the game is focused.
        {
            static bool win_shown = false;
            static bool win_clickthru = false;  // current WS_EX_TRANSPARENT state
            // Shown for the menu and nothing else now, so the window's whole lifetime matches the
            // menu's. The previous "shown while closed for the tooltip/rings" state is what needed
            // the click-through dance below; it stays because it is still what keeps the game owning
            // the cursor in the frames around opening and closing.
            const bool want = open && game_focused;
            // Click-through UNLESS the menu is open. While the window is shown only for the
            // hover tooltip / highlight rings (menu closed), it must be transparent to input
            // so the GAME fully owns the cursor - otherwise our topmost window steals cursor
            // ownership and the game cursor vanishes or flips to the system arrow (worse
            // after a map reopen). When the menu opens we drop transparency so ImGui gets
            // the mouse. Set the ex-style BEFORE showing so the first shown frame is right.
            const bool clickthru = !open;
            if (want && clickthru != win_clickthru)
            {
                LONG ex = GetWindowLongW(g_hwnd, GWL_EXSTYLE);
                ex = clickthru ? (ex | WS_EX_TRANSPARENT) : (ex & ~WS_EX_TRANSPARENT);
                SetWindowLongW(g_hwnd, GWL_EXSTYLE, ex);
                // Flush the ex-style change so hit-testing picks it up immediately.
                SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
                win_clickthru = clickthru;
            }
            if (want != win_shown)
            {
                ShowWindow(g_hwnd, want ? SW_SHOWNOACTIVATE : SW_HIDE);
                if (want)
                    SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                win_shown = want;
            }
        }
        if (open && game_focused)
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
            // Menu closed: nothing of ours is on screen at all. The branch that used to draw the
            // hover panel and the highlight rings here is gone with them - what the player sees on
            // the map is now entirely the game's own rendering, driven by our native panels and
            // icon swaps.
            render_frame(false);
            Sleep(16); // idle pacing while closed (no vsync wait from a cleared present)
        }
    }
}
#endif // MFG_OVERLAY_OWN_WINDOW

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
    if (g_highlight_srv) { g_highlight_srv->Release(); g_highlight_srv = nullptr; }
    if (g_highlight_tex) { g_highlight_tex->Release(); g_highlight_tex = nullptr; }
    if (g_preview_srv) { g_preview_srv->Release(); g_preview_srv = nullptr; }
    if (g_preview_tex) { g_preview_tex->Release(); g_preview_tex = nullptr; }
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    if (g_dcomp_visual) { g_dcomp_visual->Release(); g_dcomp_visual = nullptr; }
    if (g_dcomp_target) { g_dcomp_target->Release(); g_dcomp_target = nullptr; }
    if (g_dcomp_device) { g_dcomp_device->Release(); g_dcomp_device = nullptr; }
    if (g_swapchain) { g_swapchain->Release(); g_swapchain = nullptr; }
#if MFG_OVERLAY_OWN_WINDOW
    release_layered_targets(); // Proton fallback RT/staging/DIB
    release_surface_targets(); // 'surface' mode DComp surface + intermediate RT
#endif
    if (g_d3d_ctx) { g_d3d_ctx->Release(); g_d3d_ctx = nullptr; }
    if (g_d3d_device) { g_d3d_device->Release(); g_d3d_device = nullptr; }
    if (g_hwnd) { DestroyWindow(g_hwnd); g_hwnd = nullptr; }
}
} // namespace

bool goblin::overlay::key_down(int vk) { return kd(vk); }

void goblin::overlay::set_text_capture(bool on)
{
    if (g_text_capture.exchange(on, std::memory_order_relaxed) != on)
        spdlog::debug("[OVERLAY] native text capture {}", on ? "on" : "off");
}

bool goblin::overlay::text_key(int vk)
{
    return vk == VK_SPACE || (vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z') ||
           (vk >= VK_NUMPAD0 && vk <= VK_DIVIDE) || (vk >= VK_OEM_1 && vk <= VK_OEM_3) ||
           (vk >= VK_OEM_4 && vk <= VK_OEM_8) || vk == VK_OEM_102;
}

// ── the text feeds' keyboard layout (see the header) ──
static std::atomic<uintptr_t> g_text_hkl{0};
static wchar_t g_text_tag[8] = L"";

static void text_layout_set(HKL hkl)
{
    g_text_hkl.store(reinterpret_cast<uintptr_t>(hkl), std::memory_order_relaxed);
    wchar_t code[8] = L"";
    const LANGID lang = LOWORD(reinterpret_cast<uintptr_t>(hkl));
    if (GetLocaleInfoW(MAKELCID(lang, SORT_DEFAULT), LOCALE_SISO639LANGNAME, code, 8) > 0)
        CharUpperW(code);
    wcscpy_s(g_text_tag, code);
}

void *goblin::overlay::text_layout()
{
    uintptr_t cur = g_text_hkl.load(std::memory_order_relaxed);
    if (!cur)
    {
        // Seed from the game window's thread - the layout the player had when the game started.
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        const DWORD tid = fg ? GetWindowThreadProcessId(fg, &pid) : 0;
        text_layout_set(GetKeyboardLayout(pid == GetCurrentProcessId() ? tid : 0));
        cur = g_text_hkl.load(std::memory_order_relaxed);
    }
    return reinterpret_cast<void *>(cur);
}

const wchar_t *goblin::overlay::text_layout_tag()
{
    (void)text_layout();
    return g_text_tag;
}

bool goblin::overlay::text_layout_poll()
{
    static bool prev = false;
    bool switched = false;
    const bool shift = kd(VK_SHIFT), alt = kd(VK_MENU), ctrl = kd(VK_CONTROL);
    const bool win = kd(VK_LWIN) || kd(VK_RWIN);
    const bool combo = (shift && (alt || ctrl)) || (win && kd(VK_SPACE));
    if (combo && !prev)
    {
        HKL raw[32] = {};
        const int nraw = GetKeyboardLayoutList(32, raw);
        // ONE layout per language. A system often carries a second variant of the same language
        // (an IME or a custom layout, HKL high word 0xF0xx - seen 2026-09-02: 0xF0C00419 next to
        // 0x04190419), and cycling onto it types nothing useful. Prefer the standard layout of
        // each language (high word == language id), else the first variant listed.
        HKL list[32] = {};
        int n = 0;
        for (int i = 0; i < nraw; ++i)
        {
            const auto v = reinterpret_cast<uintptr_t>(raw[i]);
            const WORD lang = LOWORD(v);
            const bool standard = HIWORD(static_cast<DWORD>(v)) == lang;
            int at = -1;
            for (int k = 0; k < n; ++k)
                if (LOWORD(reinterpret_cast<uintptr_t>(list[k])) == lang) { at = k; break; }
            if (at < 0) list[n++] = raw[i];
            else if (standard) list[at] = raw[i];
        }
        const auto cur = reinterpret_cast<HKL>(text_layout());
        if (n > 1)
        {
            int idx = -1;
            for (int i = 0; i < n; ++i)
                if (LOWORD(reinterpret_cast<uintptr_t>(list[i])) == LOWORD(reinterpret_cast<uintptr_t>(cur)))
                { idx = i; break; }
            const HKL next = list[(idx + 1) % n];
            text_layout_set(next);
            switched = true;
            spdlog::info("[search] layout -> 0x{:X} (pick {} of {} languages, {} installed)",
                         reinterpret_cast<uintptr_t>(next), (idx + 1) % n, n, nraw);
        }
    }
    prev = combo;
    return switched;
}

void *goblin::overlay::native_hover_row() { return native_hover_row_impl(); }

bool goblin::overlay::gamepad_mask_down(uint16_t mask)
{
    return g_pad_ok && mask != 0 && (g_pad.wButtons & mask) == mask;
}

uint16_t goblin::overlay::gamepad_buttons()
{
    // Whatever is held right now, for callers that do not know the mask in advance - the menu's
    // rebind page has to LEARN a combo. It must come from this cache and not from a fresh
    // XInputGetState: we hook that function, so a direct call returns our own injected buttons
    // and, while our menu is open, reports the pad as disconnected outright. poll_gamepad reads
    // the real device through the trampoline; this just publishes what it saw.
    return g_pad_ok ? static_cast<uint16_t>(g_pad.wButtons) : uint16_t{0};
}

void goblin::overlay::setup()
{
    if (!goblin::config::menuEnabled)
    {
        spdlog::info("[OVERLAY] disabled via ini (menu_enabled = false)");
        return;
    }
    if (!goblin::config::overlay_menu_enabled())
    {
        // menu_render_mode = native: no window, no device, no ImGui, no input hooks - the menu the
        // player opens is the game's own screen. One thing still has to run: the pad state that
        // gamepad_mask_down() reports is polled HERE, and the marker-hide / master-toggle hotkeys and
        // the in-game menu's own pad combo all read it. So a poll-only thread stands in for the
        // overlay thread - it touches nothing but XInput.
        spdlog::info("[OVERLAY] not created: menu_render_mode = native (the in-game menu is the "
                     "one on the hotkey). Polling the pad only.");
        if (g_running.exchange(true))
            return;
        std::thread([] {
            // LoadLibraryA on every candidate, first hit wins: on the statically imported
            // xinput1_4 this only adds OUR reference (same HMODULE back), so the module our
            // pointer and hook live in cannot be unloaded while we hold it (report 42).
            const char *xdlls[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
            for (const char *d : xdlls)
                if (HMODULE h = LoadLibraryA(d))
                {
                    if (auto fn = reinterpret_cast<XInputGetState_t>(
                            GetProcAddress(h, "XInputGetState")))
                    {
                        pXInputGetState = fn;
                        g_xinput_module = h;
                        spdlog::info("[OVERLAY] XInputGetState from {} at 0x{:X}", d,
                                     reinterpret_cast<uintptr_t>(fn));
                        break;
                    }
                    FreeLibrary(h); // no export: hand the reference back, try the next name
                }
            if (!pXInputGetState)
            {
                spdlog::info("[OVERLAY] no XInput: gamepad hotkeys are keyboard-only this run");
                return;
            }
            // The comment above this branch used to promise "no input hooks" in this mode, and that
            // was the whole problem: with nothing between the pad and the game, our open-combo's
            // buttons reach the game as ordinary presses. On the map, where both members of the
            // default Y+R3 are live actions, that makes the combo unusable. So this mode hooks
            // XInputGetState too - not to blank the pad the way the ImGui backend does (our screen
            // IS a game screen and needs the game's own input to navigate), but purely to hide the
            // combo's own bits while the chord is held. poll_gamepad keeps reading the REAL device
            // through the trampoline, so our own detection is unaffected by the hiding.
            try
            {
                modutils::hook(reinterpret_cast<void *>(pXInputGetState),
                               reinterpret_cast<void *>(&hk_XInputGetState),
                               reinterpret_cast<void **>(&o_XInputGetState));
                modutils::enable_hooks();
                spdlog::info("[OVERLAY] pad filter ready: the open-combo's buttons are held back "
                             "from the game while the chord is down");
            }
            catch (const std::exception &e)
            {
                spdlog::warn("[OVERLAY] pad filter unavailable ({}); the combo will also trigger "
                             "its buttons' game actions", e.what());
            }
            while (true)
            {
                poll_gamepad();
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
        }).detach();
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
        hook_api("SetCursor", reinterpret_cast<void *>(&hk_SetCursor),
                 reinterpret_cast<void **>(&o_SetCursor));
    }
}
