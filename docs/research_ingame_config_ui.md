# Research: In-Game Config UI for Map for Goblins

Goal: let users change mod settings (the ~60 `show_*` category toggles + a few
numerics) **from inside the running game**, not only by editing `MapForGoblins.ini`
with the game closed. (Research run 2026-06, multi-agent workflow.)

## TL;DR

- **Recommended: a Dear ImGui overlay on a DX12 `Present` hook, compiled into the
  existing single DLL.** It is the only surveyed approach that actually delivers
  the win — flip ~60 toggles + a few numerics from one gamepad-navigable screen —
  and it is proven on our exact deployment shape (single offline DLL, EAC off,
  Steam Deck/Proton) by **EROverlay** (MIT, ER-specific) and **veeenu/hudhook**
  (confirmed on Deck under Proton/VKD3D).
- **Ship a toast-cycle fallback FIRST** (days of work, zero new deps): extend the
  existing F10 / Y+R3 hotkey loop into a "config mode" that cycles sections/toggles
  and shows state via the existing `ShowTutorialPopup` toast. Guarantees an in-game
  toggle exists immediately, de-risks the live-apply + ini plumbing the overlay also
  needs, and stays as the permanent fallback if the DX12 backend is unstable on Deck.
- **Reject** native Scaleform interactive menus (unmapped AS↔C++ bridge, no ER/Souls
  precedent, multi-week RE gamble, 4× per-profile gfx maintenance).
- **Don't over-promise** the native generic-dialog route: in our codebase its premise
  is false (we own only the one-arg `ShowTutorialPopup` trampoline; `OpenGenericDialog`
  / `GetGenericDialogButtonResult` are unpinned, the result is async, and deriving the
  `CSPopupMenu` instance failed live). Even if pinned, it is a one-question-at-a-time
  modal chain, not a 60-toggle panel. Possible v2 nicety, not the answer.

## Why ImGui wins (vs our constraints)

- **Single DLL:** ImGui is a clean CMake FetchContent add (we already pull
  minhook/pattern16/spdlog/mINI). Compiles into the one DLL, font atlas embedded,
  no second redistributable. **Profile-agnostic** — no per-profile gfx work.
- **Offline/EAC:** non-issue; this is already our model.
- **Steam Deck/Proton:** confirmed working via hudhook — **but AMD RDNA2 is exactly
  where DX12 overlays die** (imgui #7207), so the hand-written backend MUST be tested
  on real Deck hardware.
- **Gamepad:** ImGui has first-class `NavEnableGamepad`; **we must also gate the
  game's XInput** while the menu is open so the pad drives the menu, not the player.
- **Maintenance:** the swapchain COM-vtable hook is *more* patch-stable than our
  `.text` AOBs. Cost = vendored imgui pin + the mod's FIRST per-frame code path
  (game-wide crash blast radius if the DX12 backend is wrong).
- **The win is 90% wired already:** schema-driven `config::show*` bools,
  `is_category_enabled()` as the single filter, `menu_auto_toggle_loop` as sole owner
  of param state. Widgets just flip a bool and re-filter.

## Effort (honest)

- Toast-cycle MVP: **1–3 days** (almost all reused code).
- ImGui overlay clean-PC happy path: ~2–4 days (copyable from EROverlay/UniversalHookX).
- ImGui overlay realistic for THIS audience: **~1–2 weeks** — the costly must-test
  items are (1) AMD/Deck device-removed debugging on the DX12 backend and (2) XInput
  gating under Proton, plus matrix testing across 4 loaders + 4 profiles.

## Implementation plan (ordered)

0. **Ship the fallback first.** Extend `toggle_hotkey_loop` into a config mode that
   cycles the 11 schema sections / ~60 toggles, shows state via the existing
   `ShowTutorialPopup` trampoline (inject more TutorialParam rows + TutorialBody FMG
   via `inject_tutorial_popup_rows`/`setup_messages`), writes back to the ini.
1. **DX12 hook spike (gated decision point).** FetchContent imgui +
   `imgui_impl_dx12` + `imgui_impl_win32` (pin a tag). Hook `IDXGISwapChain::Present`
   (vtable idx 8), `ID3D12CommandQueue::ExecuteCommandLists` (idx ~10, to capture the
   queue — mandatory in DX12), `IDXGISwapChain::ResizeBuffers` (idx 13). Lazy-init on
   first Present (device, BufferCount, SRV font heap, one RTV per back buffer,
   per-backbuffer allocator/list, RENDER_TARGET↔PRESENT barriers). **Exit criteria:**
   a test window renders and survives alt-tab/resize with **no device-removed crash
   on AMD.**
2. **Input + WndProc.** `SetWindowLongPtrW` detour → `ImGui_ImplWin32_WndProcHandler`;
   swallow mouse/keyboard when `WantCapture*`. Set `NavEnableGamepad` + `HasGamepad`.
   **Hook `XInputGetState` and mask the report to the game while the menu is open.**
   Reuse F10/Y+R3 to open/close.
3. **Wire the UI.** Generate widgets from `build_schema()` (11 sections, ~60 bools +
   3 numerics, comment-string tooltips). Add Save-to-ini / Reload / Reset-to-defaults.
   Honor the ERR-only gate.
4. **Live-apply correctness.** Re-filter only the changed category's cached row subset
   via `is_category_enabled()` (don't re-run `inject_map_entries()` over ~7000 rows).
   Route all param-state changes through `menu_auto_toggle_loop` ownership; add an
   atomic/ordering guard between the F10 watcher and UI mutation; re-sync config after
   an ERSC hosting revert. Decide scope on the static `live_loot_*` toggles.
5. **Harden + matrix-test.** SEH/null-guard the per-frame path. Test on real Steam
   Deck (AMD RDNA2): borderless/fullscreen, resolution change, alt-tab, GE-Proton/VKD3D
   versions, cursor/focus, XInput gating. Smoke-test all 4 loaders + 4 profiles.
   Document Steam-overlay/RTSS/GeForce-Experience DX-hook conflicts. Keep toast-cycle
   as a config-selectable fallback.
6. **Ship.** Pin imgui in CMake; add the new hook/vtable capture to
   `tools/_check_aobs_new_build.py`.

## Risks

- First per-frame path = game-wide crash blast radius (today: degrade-not-crash).
- DX12 device-removed on AMD/Deck (imgui #7207) — most likely real failure; test on Deck.
- Gamepad arbitration — must gate the game's XInput; mandatory for our audience.
- Proton focus/cursor edge cases; DX-hook conflicts (Steam overlay/RTSS/GFE).
- Vendored imgui pin + binary size; config/param race + ERSC staleness.
- Optimistic effort estimate (realistic ~1–2 weeks).

## Open questions to spike before committing

1. Capture the DX12 queue/swapchain vtables reliably under VMProtect — transient dummy
   swapchain vs hooking `CreateDXGIFactory`?
2. Does an `XInputGetState` hook actually suppress player input under Proton, or does
   ER read the pad another way (raw input / different XInput DLL)?
3. Does the hand-written DX12 backend survive `ResizeBuffers`/device-removed on AMD
   RDNA2 (Deck)?
4. Which imgui tag pins cleanly with our `/MT` + `_ITERATOR_DEBUG_LEVEL=0` build and
   matches the queue-capture pattern we copy?
5. Can `live_loot_labels`/`live_loot_icons` be made re-callable at runtime, or stay
   launch-only in v1?
6. Is per-toggle re-filter of the changed category cheap enough over ~7000 rows, or do
   we debounce?
7. Do all four loaders inject early enough and coexist with a Present hook without their
   own DXGI conflict?

## Key references

- **EROverlay** — MIT, Elden Ring-specific ImGui+DX12 overlay; near drop-in reference
  for the hook/init/render layer.
- **veeenu/hudhook** — DX12 ImGui overlay framework; evidence it renders + takes
  controller input under Proton/VKD3D on Steam Deck.
- **UniversalHookX** — generic swapchain hook reference (Present idx 8,
  ExecuteCommandLists, ResizeBuffers idx 13).
- imgui issue #7207 — DX12 device-removed on AMD (the Deck failure mode to test).
