# ERSC Hosting Crash & the Map Auto-Hide Mechanism

This documents (1) the real root cause of the Seamless Co-op (ERSC) hosting
crash and how it was fixed, and (2) the **map-state auto-hide mechanism** that
was built as a workaround *before* the root cause was found - preserved here so
the technique isn't lost even though the code is being removed.

## 1. Root cause of the hosting crash (found 2026-05-28)

For a long time MapForGoblins crashed the game when **hosting** a Seamless
Co-op session. The working theory was that ERSC validates `WorldMapPointParam`
at host-start by row count and rejects our expanded buffer. That theory was
**wrong**.

The real cause was a **param-buffer alignment bug**, identical to a separate
crash we hit when injecting `TutorialParam` rows for the codex toast:

- Our param injection lays out the buffer as
  `[wrapper header (0x10)] [param header (0x40)] [row locators] [row data] [type string] [wrapper_row_locators]`.
- We placed the trailing `wrapper_row_locator` array at a **4-byte-aligned**
  offset: `wrapper_row_loc_start = (after_type_str + 3) & ~3`.
- The game's **param lookup-by-id** engine
  (`LookupTutorialParam` @ `eldenring.exe+0xD51BA0` - *pre-2026-05-29 build;
  .text shifted on that update - re-resolve before trusting*; the same code
  shape applies to any param) reads `wrapper_row_loc_start` out of the wrapper
  header and
  **rounds it UP to 16** (`(x + 0xF) & ~0xF`) before using it as the base of
  its binary search over `wrapper_row_locators`.
- If the array isn't actually 16-aligned, the engine starts reading 4-12 bytes
  *past* it → garbage row ids → out-of-range index → OOB read of row data →
  `ACCESS_VIOLATION`.

**Why it only bit certain params:**
- `WorldMapPointParam` is only ever **iterated** (to render map icons), never
  looked up by id during normal play - so 4-align was harmless in solo.
- **But ERSC's host-start validation performs an id lookup on
  `WorldMapPointParam`.** That lookup hit the misaligned array → crash. That's
  why it only crashed when hosting.
- `TutorialParam` IS looked up by id (both by `ShowTutorialPopup` and during
  **save loading**), so expanding it crashed save-load with the same signature.

**The fix (one line, applied in both injectors):**
```cpp
// was: (after_type_str + 3) & ~3   // 4-align - WRONG for id-looked-up params
size_t wrapper_row_loc_start = (after_type_str + 0xf) & ~(size_t)0xf;  // 16-align
```
Locations: `inject_map_entries` (WorldMapPointParam) and
`inject_tutorial_popup_rows` (TutorialParam) in `goblin_inject.cpp`.

**How it was diagnosed:**
1. `tools/_parse_minidump.py` on the crash dump → faulting RVA `0xD51CC5`
   (an `ACCESS_VIOLATION read`) - *pre-2026-05-29 build; .text shifted on that
   update - re-resolve before trusting*.
2. An ad-hoc disassembly of `LookupTutorialParam` revealed the
   `mov eax,[rdx-0x10]; add eax,0xf; and ...,~0xf` align-up of the wrapper
   offset, followed by `mov rbx,[rdx + (index+3)*24]` (the faulting row-locator
   read).

Note: the RVAs above (`0xD51BA0`, `0xD51CC5`) are .text addresses captured on
the pre-2026-05-29 build. The 2026-05-29 game+ERR update shifted .text RVAs
(e.g. the `ShowTutorialPopup` trampoline moved `0x80DA50` -> `0x80D960`), so
re-resolve them before trusting. They appear only in this descriptive comment,
not in runtime code, so this is doc drift, not a bug. The align-up mechanism
itself - `(x + 0xf) & ~0xf` of the wrapper offset before the binary search -
remains correct regardless of the RVA.

**Consequence:** with 16-align, the expanded `WorldMapPointParam` can stay live
**during hosting** - ERSC's id lookup now lands on the correct array. The map
auto-hide workaround (section 2) is therefore **no longer required**.

### HeapAlloc vs VirtualAlloc (unchanged)
Param/FMG buffers are allocated with `HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, ...)`,
**not** `VirtualAlloc`. ERSC's `game_memory_unlimiter` expects param memory to
come from the process heap; a dedicated VirtualAlloc'd page region caused a
separate crash when hosting. This is independent of the alignment bug and is
kept. (Buffers are a few hundred KB to a few MB; HeapAlloc is also a better fit
than VirtualAlloc's 64 KB granularity.)

## 2. Map-state auto-hide mechanism (workaround - now removed)

Before the root cause was found, the crash was dodged with an **inverse,
crash-proof design**: keep `WorldMapPointParam` **vanilla by default** and
expand it **only while the world map is open** - the only time the icons render.
ERSC's host-start validation always runs while the map is closed, so it always
saw the vanilla table and could never trip the (then-unknown) bug.

### The signal: CSMenuMan world-map-open byte
- A byte in CSMenuMan's per-screen menu-state array at **`CSMenuMan + 0xCD`**.
- Verified live across every state: gameplay = 0, ESC menu = 0, inventory = 0,
  item popup = 0, **ERSC host dialog = 0**, **world map open = 7** (transitions
  `0 → 3` (loading) → `7` (open), occasionally via `1`).
- It is the only field exclusive to the world map: not shared with other menus,
  not sticky (returns to 0 on close). Other menus write their "active" marker to
  their own index (e.g. ESC/inventory use `+0xAF`).
- `0xCD` is an undocumented offset for this build (inside `CSMenuManImp`'s
  `unk90` array) - **re-verify after a game/ERR patch**.
- CSMenuMan singleton via AOB `48 8B 05 ?? ?? ?? ?? 33 DB 48 89 74 24`
  (`{{3,7}}` RIP-relative), slot RVA `0x3D6B7B0`.

### The logic (10 ms poll)
```
userDisabled (F10 master-off)        -> VANILLA
else autoActive (enableMenuAutoToggle && (ersc.dll || !requireErsc))
                                     -> EXPANDED only while map_state != 0
else (no co-op / forced)             -> EXPANDED always
```
Critical detail: expand on the **first non-zero** (the `0 → 3` edge), **not**
`== 7`. The game builds the map-icon list during the loading phase (~2 s before
reaching 7) and never re-reads it; expanding only at 7 leaves the list built
from the vanilla table → no icons.

### Tradeoff (why we're glad to drop it)
Icons showed **only on the world map** (vanilla everywhere else), and there was
a risk of a one-frame-late expand on first map open. With the alignment fix the
table stays expanded everywhere with no hosting risk, so this is strictly better.

### Dead ends explored (do not repeat)
- `CSSessionManager.lobby_state` (u32 @ instance +0x0C; 0=solo, 3=Host): a clean
  multiplayer signal, but ERSC validates params **before** the native
  `lobby_state` transitions, so it's too late to gate the crash.
- Popup/menu signals (`CSMenuMan+0x80` popup_menu → top job vtable, or
  `CSFeMan.hud_state==0`): the world map itself is byte-identical to the ERSC
  dialog through these → reverts exactly when you open the map. Unfixable there.
- Invented "vtable/visible" bytes on CSPopupMenu, string-scans for session
  strings (matched recycled heap slabs) - all false fires.

## 3. What replaced it

- 16-align fix in both param injectors → expanded table is safe during hosting.
- `WorldMapPointParam` stays **EXPANDED always**.
- The hotkey (F10 / gamepad `Y+R3`) becomes a pure **personal show/hide toggle**
  (`g_icons_user_disabled`), not a hosting workaround.
- Config `enable_menu_auto_toggle` / `menu_auto_toggle_require_ersc` are retired
  (the map-state revert path is removed from `menu_auto_toggle_loop`).

If a future game/ERR patch ever reintroduces a hosting crash that the alignment
fix doesn't cover, the section-2 mechanism can be restored from this document.
