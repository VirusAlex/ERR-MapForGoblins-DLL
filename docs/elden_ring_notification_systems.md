# Elden Ring on-screen notification systems - research notes

A working notes document on Elden Ring's various message / notification / toast
systems, the result of reverse-engineering and live testing while trying to
find the right channel for a small custom-text toast in MapForGoblins.

All RVAs/offsets here are for the eldenring.exe build that ships with
**ERR 2.2.1.2** (app **~2.6.1.0 WW**, ImageBase 0x140000000). Anything that
depends on a fixed offset will drift after a game patch - prefer AOBs and
re-verify when versions change.

> **UPDATE (captured pre-2026-05-29-update):** the `.text` RVAs in this doc
> were captured **pre-2026-05-29-update** and are **SHIFTED** on current
> builds - resolve by AOB. The trampoline moved **0x80DA50 -> 0x80D960**. By
> contrast, `.data` singleton slots (e.g. CSMenuMan / CSFeMan) and `.rdata`
> RTTI vtables did **NOT** move.
>
> **Re-verified on the 2026-05-29 build** (functions/signatures/struct offsets all
> still correct; only the `.text` RVAs shifted - re-resolved by AOB):
> | function | doc RVA (pre-patch) | current RVA |
> |---|---|---|
> | FeSystemAnnounce enqueue | 0x841C50 | **0x841B60** |
> | Summon msg ctor | 0x843860 | **0x843770** |
> | FullScreenMessage `display_status_message` | 0x766460 | **0x766370** |
> | FullScreenMessage setter (writes `[CSFeMan+0x3760]`) | 0x76E480 | **0x76E390** |
> | blinking-message setter helper | 0x76E340 | **0x76E25B** |
> | Item-Get add function | 0x779C70 | **0x779B80** |
> | ShowTutorialPopup trampoline | 0x80DA50 | **0x80D960** |
>
> `.data` slots confirmed UNCHANGED: CSFeMan `0x3D6B880`, CSItemGetMenuMan `0x3D6C3B0`.
> Struct offsets (CSFeMan `+0x3760` / `+0x6548` / `+0x654C` / `+0x59A4` / `+0x3720`,
> SummonMsgData `+0x08`/`+0x0A`/`+0x10`/`+0x48`) confirmed live. EMEVD class-2007
> instruction indices confirmed against `er-common.emedf.json`.

Throughout, each fact is labelled:

- **Confirmed** - verified by disassembly **and**, where applicable, by live
  in-game testing.
- **Inferred** - derived from structure layouts in `vswarte/fromsoftware-rs`
  or by cross-referencing two confirmed facts. Plausible but not directly
  observed.
- **Guessed** - best-effort interpretation. Could be wrong; flagged so future
  work knows where to be skeptical.

## What MapForGoblins actually ships

The shipping toast channel is the **AOB-resolved ShowTutorialPopup
trampoline** (inner 0x7EF5B0 / outer 0x7EE630, popup singleton at
CSFeMan_slot+0x80 - see the TUTORIAL_PARAM_ST section), driven by:

- **injected `TUTORIAL_PARAM_ST` rows** with `menuType=0` (via
  `inject_tutorial_popup_rows`), and
- **4 static `TutorialBody.fmg` entries** (ids **9004251..9004254** in
  MsgRepository slot **208**).

These power two banners:

- the **F10** (or **Y+R3**) "Map icons: ON/OFF" personal show/hide banner, and
- the **F9** "Markers dumped" / "Marker dump failed" confirmation banner.

Note: the old map-state auto-hide was **removed** - the table stays
**EXPANDED always**.

All other channels documented below (FeSystemAnnounce, Summon, blinking
message, etc.) are research notes; only the trampoline path above ships.

## Singleton roots

Most of these systems hang off two singletons.

- **CSMenuMan** - instance-pointer slot at **RVA 0x3D6B7B0**. AOB:
  `48 8B 05 ?? ?? ?? ?? 33 DB 48 89 74 24` (3,7 RIP-relative). Confirmed.
  Struct: `CSMenuManImp` in `fromsoftware-rs/crates/eldenring/src/cs/menu_man.rs`,
  size 0x8A0.
- **CSFeMan** - instance-pointer slot at **RVA 0x3D6B880**. AOB:
  `48 8B 05 ?? ?? ?? ?? 48 85 C0 74 11 8B 80 3C 65 00 00` (3,7 RIP-relative).
  Confirmed. Struct: `CSFeManImp` in `cs/fe_man.rs`, size 0x8420.
  `CSFeMan+0x18` is a back-pointer to CSMenuMan (sanity check).
- **CSItemGetMenuMan** - slot at **RVA 0x3D6C3B0** (libER
  `GLOBAL_CSItemGetMenuMan` = decimal 64406448). Confirmed via libER symbols.
- **MENU_COMMON_PARAM_ST** - a single-row param holding global menu/HUD
  timing constants. Confirmed structure in
  `fromsoftware-rs/.../param/generated.rs`.

## Helper primitives (very useful for any custom-text path)

Two functions and a layout you'll keep reusing.

- **Get the game's default allocator: RVA 0x763360**. Two-instruction stub:
  `mov rax,[rip+global]; ret`. Returns a `void*` that's a valid
  `DLAllocatorRef` for the game's heap. Confirmed.
- **Assign a wide string into a `DLString`: RVA 0x11A3E0**. Signature
  `void(DLString* dst, const wchar_t* text, void* allocator)`. Sets up SSO
  (capacity=7) or heap backing, copies the chars, null-terminates. This is
  the function the working `FeSystemAnnounce` element ctor uses internally.
  Confirmed by disassembly **and** live use in our build.
- **`DLString` layout (size 0x30)**, used inside `MenuString` and
  `SummonMsgData`:
  - +0x00 allocator pointer
  - +0x08 backing pointer (points to inplace SSO buffer or heap buffer)
  - +0x10..+0x18 inplace SSO buffer (16 bytes = 8 UTF-16 chars including null)
  - +0x18 length
  - +0x20 capacity (7 means SSO; >=8 means heap)
  - +0x28 unknown (possibly second allocator ref or pad)

  Inferred - derived from the construction code in 0x11A3E0; not verified by
  finding an explicit struct definition.

- **`MenuString` layout (size 0x38)**, confirmed in `cs/fe_man.rs`:
  - +0x00 `static_string: const wchar_t*`
  - +0x08 `allocated_string: DLString` (0x30 bytes)

  At render time, `allocated_string` is used if non-empty, otherwise
  `static_string`. (Verified empirically: the in-house Summon path that
  copies a built `allocated_string` shows our text.)

With these three primitives you can fabricate a valid `MenuString` from a
plain `const wchar_t*` and pass it into any system that expects a `MenuString`
or `DLString` argument.

## FeSystemAnnounce - wide top bar (working today)

**Style** (confirmed live): a narrow-height bar across the **full width of
the screen**, near the top, text left-aligned. No sound. Default duration is
long (we measured ~15 seconds for short text).

**View model** - `FeSystemAnnounceViewModel`, reached via
`*(CSMenuMan+0x860)` or equivalently `*(CSFeMan+0x30)`. Struct in
`cs/menu_man.rs`. Contains an internal `FeSystemAnnounceViewModelMessageQueue`
(a ring buffer of message elements).

**Enqueue function - RVA 0x841C50**. Signature:
`void(void* view_model, const wchar_t* text)`. Returns `bool` (`true` if
queued, `false` if the queue is full - queue cap = 10).

The function **copies** the wide string into the queue element (an
`std::wstring`-like via `0x11A3E0`), so a string literal is safe to pass and
the source buffer can be discarded immediately. Confirmed by disassembly and
live in-game use.

Unique AOB (count = 1 in the .text section, so robust against ASLR/version
drift):

```
48 89 54 24 10 48 83 EC 28 48 8B 41 38 48 FF C0
48 83 F8 0A 73 15 48 83 C1 10 48 8D 54 24 38 E8
```

**Calling convention from C++:**

```cpp
auto vm = *(void**)((uint8_t*)menu_man + 0x860);
((void(*)(void*, const wchar_t*))(er_base + 0x841C50))(vm, L"Map icons: ON");
```

**Knobs available:** only the text. The queue element has shape
`{ uint8_t active_flag@0, std::wstring text@+0x10 }` - no per-message duration
or styling. Width and position are baked into the FrontEnd Scaleform
(`menu/01_000_fe.gfx`).

**Duration is controlled globally** via
`MENU_COMMON_PARAM_ST.system_announce_no_scroll_wait_time` (f32 seconds, for
non-scrolling text). Inferred from naming and the agent's notes; we have
**not** confirmed this by patching the value and watching the duration
change.

**Reference implementation in the wild:** `github.com/metal-crow/ER-Blurbs-Mod`
calls this exact RVA the exact same way. That mod was the smoking gun that
proved the approach.

## Summon message - narrow center-bottom plaque (working, with a trick)

> **RESEARCHED BUT RETIRED in the shipping mod.** This channel worked, but it
> depended on five hardcoded `.text` RVAs (ctor 0x843860, enqueue 0x844060,
> dtor 0x843910, allocator 0x763360, DLString-assign 0x11A3E0) that game
> updates invalidate. The mod ships **only** the AOB-resolved
> ShowTutorialPopup trampoline (see "What MapForGoblins actually ships" and
> the TUTORIAL_PARAM_ST section). The notes below are kept for reference.

**Style** (confirmed live in MapForGoblins build): narrow, **horizontally
centered**, **slightly below screen center**. Our test text rendered cleanly,
no sound. Visible for roughly 5-7 seconds.

**Queue** - `SummonMsgQueue` (size 0x280) embedded in CSFeMan at
**+0x62B0**. Contains a `current: SummonMsgData` + `CSFixedList<SummonMsgData,4>`
(struct in `cs/fe_man.rs`).

**`SummonMsgData` layout (size 0x50)** - confirmed:
- +0x00 vftable
- +0x08 `priority: i16`
- +0x0A `force_play: bool`
- +0x0B..+0x10 padding
- +0x10 `text: MenuString` (size 0x38)
- +0x48 `unk48: bool`
- +0x49..+0x50 padding

**Three game functions involved:**

- **Constructor - RVA 0x843860.** Signature:
  `void(SummonMsgData* out, int16_t priority, bool force_play, MenuString* text_src, uint8_t unk48)`.
  Copies `text_src.static_string` into `out.text.static_string`, then deep-
  copies `text_src.allocated_string` into `out.text.allocated_string` via
  `0x117930` (DLString copy). Confirmed.
- **Enqueue - RVA 0x844060.** Signature:
  `void(SummonMsgQueue* queue /*=CSFeMan+0x62B0*/, SummonMsgData* data)`.
  Mutex-guarded (good for cross-thread calls), takes a deep copy of `data`
  into the queue. Confirmed.
- **Destructor - RVA 0x843910.** Destructs a local SummonMsgData (frees its
  copied DLString). Confirmed.

There are also two ready-made wrappers in the game at 0x76E3A7 and 0x76E400
that do ctor → enqueue → dtor with hardcoded `priority = 0x63 = 99`. We don't
call those (the priority is fixed), but they were useful for figuring out the
correct argument order.

**The "trick" you must know.** The constructor's deep-copy of
`allocated_string` (via `0x117930`) crashes if the source DLString is just
zeroed memory: it expects a valid (possibly empty) DLString with a real
allocator pointer and a valid backing pointer / SSO capacity. The fix is to
build the source DLString correctly using the helper pair from the section
above:

```cpp
void* allocator = ((void*(*)())(er_base + 0x763360))();
uint8_t src_menu_string[0x38] = {};            // MenuString
*(const wchar_t**)src_menu_string = nullptr;   // static_string unused
((void(*)(void*, const wchar_t*, void*))(er_base + 0x11A3E0))(
    src_menu_string + 8, L"Map icons: ON", allocator);

uint8_t summon[0x50] = {};
((void(*)(void*, int16_t, uint8_t, void*, uint8_t))(er_base + 0x843860))(
    summon, /*priority=*/1, /*force_play=*/1, src_menu_string, /*unk48=*/1);
((void(*)(void*, void*))(er_base + 0x844060))((uint8_t*)fe_man + 0x62B0, summon);
((void(*)(void*))(er_base + 0x843910))(summon);
// src_menu_string's DLString is leaked here - small (~tens of bytes per call).
// For production: either cache the src once per (text) and reuse, or find
// the standalone DLString destructor.
```

**Empirical behavior of the parameters** (live test results, all variants
tested at this point):

- `priority` (`int16_t`): tried 1, 10, 99. No visible visual difference.
  Inferred to govern queue ordering when multiple summon messages are
  pending - not user-visible styling.
- `force_play` (`bool`): tried 0 and 1, including in combination with each
  unk48 value. **No effect on render visuals or blink.** Inferred to gate
  override behavior when a higher-priority message is already on screen
  (not observable in our solo tests).
- `unk48` (`uint8_t`, effectively boolean): **0 (or 2 and probably any
  non-1 value) → the plaque blinks ~3 times before disappearing. 1 → the
  plaque does not blink, stays steady, disappears cleanly after ~5 s.**
  Confirmed empirically.

**Not tested:** whether this view renders during multiplayer-only states only
(initial worry - turned out to render in solo just fine after the DLString
fix).

**What we don't know:**
- Exactly what `priority` modulates visually (suspect it's not visible at all
  outside of queue collisions).
- Whether `force_play` has any visible effect at all (likely not, in solo).
- How to free a standalone `DLString` cleanly without going through a
  containing object's destructor.

## proc_status_messages and the six FrontEndView MenuString slots - not working from a DLL (yet)

This is the cluster we have NOT cracked. There's a small status-line render
path in the FrontEnd that uses MenuString slots embedded in
`FrontEndViewValues`. Writing into them directly does not produce visible
text in our tests - there's apparently another trigger we haven't located.

**The ring buffer**: `CSFeMan+0x59A4..+0x59BC` holds six `i32` slots
(`proc_status_messages: [i32;6]`), plus a read index at +0x59BC and a write
index at +0x59C0. Confirmed in `cs/fe_man.rs`.

**The consumer at RVA 0x7711F4** reads the current id from the ring, calls a
**templated resolver at RVA 0x760A60** (which delegates to 0x7634C0 with two
format-descriptor pointers), and writes the result into
`FrontEndViewValues.proc_status_message: MenuString` at **CSFeMan+0x3720**.
The render then displays this string while
`proc_status_message_timer` at **CSFeMan+0x3758** is below ~3.0s. Confirmed
by disassembly.

The templated resolver means **the id → text mapping is not a plain FMG
lookup**: you can't just inject a row into an FMG and trigger it with that
id. The resolver expects ids it knows about and formats text via internal
descriptors. Confirmed by disassembly; this is why the FMG-injection approach
that worked for `subarea_name_popup_message_id` does **not** work here.

**Six FrontEndView MenuString slots - purpose only partially understood.**
According to `cs/fe_man.rs` there are six `MenuString` fields clustered in
this region, each 0x38 bytes:

- CSFeMan + **0x36D8** - `FrontEndViewValues.unk3658`
- CSFeMan + **0x3720** - `FrontEndViewValues.proc_status_message` (confirmed)
- CSFeMan + **0x3768** - `FrontEndViewValues.unk36e8`
- CSFeMan + **0x37A0** - `FrontEndViewValues.unk3720`
- CSFeMan + **0x37D8** - `FrontEndViewValues.unk3758`
- CSFeMan + **0x3810** - `FrontEndViewValues.unk3790`
- CSFeMan + **0x3848** - `FrontEndViewValues.unk37c8`

A previous research agent claimed (Guessed) these are a 6-deep ring of small
"proc-status" lines, each pushed to a distinct Scaleform text element each
frame, and that writing the MenuString plus bumping the shared timer would
render text. **This hypothesis has now been refuted by live testing.**

We wrote our own UTF-16 text into the `allocated_string` of each of the six
slots (using the correct DLString construction path: get-allocator at
0x763360 → assign at 0x11A3E0) and reset `proc_status_message_timer` at
CSFeMan+0x3758. **None of the six slots rendered any visible text.**
Confirmed across all of: +0x36D8, +0x3720, +0x3768, +0x37A0, +0x37D8,
+0x3810, +0x3848.

So:
1. The render is gated on something we don't write - likely an "active"
   flag, a per-slot timer/counter, or being driven exclusively by the
   ring-buffer consumer that we cannot feed with custom text because of
   the templated resolver. **Most likely explanation.**
2. The slots beyond `proc_status_message` serve a different purpose
   entirely (HUD subtitles for area names, multiplayer role tags, summon
   countdown, etc. - the names `unk36e8` / `unk3720` etc. give no hints
   and we haven't traced their actual readers).
3. Some additional initialization is needed (maybe the slot's MenuString
   must already have been "owned" by the render task - direct overwrite
   from outside might be ignored).

**Status: closed for "use as a custom-text channel."** Not a viable path
from a DLL given current understanding. Re-opening would require finding
the specific render code that reads each slot and traces what gates its
visibility - meaningful disassembly work with uncertain payoff.

**What we know for sure:** `display_status_message(menu_man, 41)` (the
FullScreenMessage::MenuText path) does *something* with this MenuString
slot - it writes 41 into +0x3760 and the render is supposed to pick up
`proc_status_message` - but in practice that path produces the death/grace
sound with no visible text in our experiments. Not understood.

## FullScreenMessage - big center banner (confirmed, but always plays a sound)

**Style** (confirmed via existing enum values): huge centered banner. Each
enum value has its own fixed text **and its own sound**, looked up from a
resource table at runtime.

- `display_status_message` - RVA **0x766460**. Signature:
  `bool(CSMenuManImp* /*=CSMenuMan instance*/, int32_t message)`.
- Internally tail-calls the one-line setter at **RVA 0x76E480** which does
  `mov [CSFeMan+0x3760], edx; ret`. So the public function effectively just
  writes the request id into `FrontEndViewValues.full_screen_message_request_id`
  at **CSFeMan+0x3760** and returns.
- The render task (consumer at **RVA 0x8C8EC5**) reads `[fev+0x36E0]` =
  CSFeMan+0x3760 each frame and dispatches the banner through a table lookup
  at **RVA 0xD29660** (image-relative; the absolute VA is 0x140D29660) keyed
  by message id. The table record provides the banner's FMG entry, layout,
  and **the sound**. Confirmed by disassembly. (Image-relative form added to
  the pre-patch / re-verify set - see "How to re-verify after a game patch".)

`FullScreenMessage` enum (defined in `cs/fe_man.rs`):

| Value | Name | Notes |
|-------|------|-------|
| -1 | None | sentinel (no banner) |
| 1 | DemigodFelled | boss-kill jingle |
| 2 | LegendFelled | |
| 3 | GreatEnemyFelled | field-boss |
| 4 | EnemyFelled | minor boss |
| 5 | YouDied | death sting |
| 7 | HostVanquished | PvP |
| 8 | BloodFingerVanquished | |
| 9 | DutyFullFilled | |
| 11 | LostGraceDiscovered | grace chime |
| 13 | Commence | arena |
| 14-16 | Victory / Stalemate / Defeat | arena |
| 17 | MapFound | soft chime |
| 21-25 | rune restored / god slain / vanquished | various |
| 26-39 | covenant rank-advanced family | rank-up jingles |
| 40 | HeartStolen | |
| **41** | **MenuText** | the only one with dynamic text - supposedly renders the current `proc_status_message` MenuString. In our experiments this fires the sound but no visible text appears, which is one of the open questions noted above. |
| 42 | YouDiedWithFade | death + screen fade |

**Sound is data-driven per enum value** via the table at 0xD29660
(image-relative; absolute VA 0x140D29660) - it's
**not** overridable at the call site. So you cannot mute YouDied/LostGrace
banners by zeroing a field; if you fire id 5 or 11 you will hear the sound.

**Custom-text feasibility:** only via id 41 (MenuText), and we have not made
that work in practice.

## subarea / blinking center messages (FMG-id driven)

These two CSFeMan-field paths render center text from an FMG by id. Both
were tested live and both render but in styles we ended up not wanting.

**Subarea welcome message** (the "Limgrave" / "Звёздные пустоши" area-name
slide-in). Setter fields:
- `subarea_name_popup_message_id: i32` at **CSFeMan+0x6534**
- `area_welcome_message_request: bool` at **CSFeMan+0x6554** (set to 1 to
  trigger)

Confirmed in `cs/fe_man.rs` and tested live. Driven by EMEVD
`DisplaySubareaWelcomeMessage` 2007[13]. **Style: huge, center-screen, with
a soft sound**. In our live test the in-game render actually showed the
current area's name (not our injected FMG text) - the path may resolve the
id against an internal table before falling back to FMG, or our injected id
wasn't in the FMG it actually reads. Open question.

**Blinking message:**
- `blinking_message_id: i32` at **CSFeMan+0x6548**
- `blinking_message_priority: u8` at **CSFeMan+0x654C** (we wrote 1 to
  trigger; other values untested)

Driven by EMEVD `DisplayBlinkingMessage` 2007[04] and
`DisplayBlinkingMessageWithPriority` 2007[12]. **Style: center, smaller font
than the area banner, with a light sound - and our injected FMG text DID
render in our live test.** This is the second working custom-text channel
alongside FeSystemAnnounce and Summon.

The text comes from **EventTextForMap** FMG. To use it, inject your strings
into MsgRepositoryImp slot 34 at a reserved id (we used 9101000/9101001) -
the existing `patch_fmg_in_memory` machinery handles this fine.

There are also two CSFeMan helper functions that wrap these field writes,
should you want a single-call API:
- 0x76E2E0 writes `[CSFeMan+0x6550] = edx` (some related subarea field)
- 0x76E340 writes `[CSFeMan+0x6548] = edx; [CSFeMan+0x654C] = r8b` -
  effectively the blinking-message setter. Inferred.

## Item-Get Log - bottom-left "Acquired X" stack (text via item-name FMG)

The classic Souls bottom-left toast that stacks when you pick up multiple
items.

- Manager: `CSItemGetMenuMan` (slot RVA 0x3D6C3B0, see Singletons section).
- Add function: **RVA 0x779C70**. Argument shape (Inferred from one observed
  caller site at RVA 0x255422):
  - rcx = the manager
  - rdx = a pointer to a stack-local (purpose unknown)
  - r8d = an integer (probably the item id - Guessed from the calling
    pattern)
  - r9 = another pointer to a stack-local
  - five additional bytes/ints on the stack: byte@+0x20, byte@+0x28, int@+0x30
    (set to 0xFFFFFFFF/-1 by the observed caller), byte@+0x38, byte@+0x40
    (set to 1)
- Text source: the item's **GoodsName** (or weapon/armor/etc.) entry in the
  appropriate FMG, looked up by `item_id`. So for **custom text** you'd
  need to override an item's name in its FMG (something MapForGoblins
  already does for icon labels via `patch_fmg_in_memory`) and pass that
  item's id to the add function.
- Not tested in MapForGoblins. The argument shape isn't fully decoded yet
  (the flags), so calling this safely requires more disassembly than the
  Summon or FeSystemAnnounce paths.

This is one of the more promising paths for "small stacked notifications"
if you're willing to repurpose an item entry.

## EMEVD instructions and what they actually do

A quick map of the in-game EMEVD instructions that talk to message systems.
Useful when reading ERR's `common.emevd.dcx` or vanilla scripts to figure
out which system shows what. Names and bank/instruction indices come from
`er-common.emedf.json` and the soulsmods EMEDF; usage counts are from
decompiling ERR's `common.emevd.dcx`.

- **`DisplayBanner` 2007[02]** `(TextBannerType)` - fixed predefined center
  banner. Takes an enum value, not an FMG id. ERR uses it ~9 times. Confirmed.
- **`DisplayStatusMessage` 2007[03]** `(MessageID, padState)` - described in
  references as "bottom status line"; we haven't pinned the underlying
  function or confirmed the rendering location. Not used in
  ERR's common.emevd. Worth investigating if "small bottom toast" is what
  you want.
- **`DisplayBlinkingMessage` 2007[04]**, **`...WithPriority` 2007[12]** -
  see the blinking-message section above. Center, smaller, EventTextForMap.
  ERR uses ~25 times.
- **`DisplaySubareaWelcomeMessage` 2007[13]**, **`DisplayAreaWelcomeMessage`
  2007[14]** - the big center area-name banner. ERR uses 77 times.
- **`ShowTutorialPopup` 2007[15]** `(TutorialParamID, b, b)` - data-driven
  popup keyed by a `TUTORIAL_PARAM_ST` row. Visual style depends on the row's
  `menuType` column: `menuType=0` → small upper-left **toast** (the ERR
  Codex / medal style - what we want); higher values → larger / centered
  modal cards. Renders via the Scaleform widget `menu/01_060_caption.gfx`.
  ERR's most-used message instruction by far: ~88 calls in common.emevd, all
  using `menuType=0`. The underlying game-function path is now pinned and
  shipped (AOB-resolved trampoline; inner 0x7EF5B0 / outer 0x7EE630, popup
  singleton at CSFeMan_slot+0x80) - see the dedicated TUTORIAL_PARAM_ST
  section below. Confirmed.
- **`DisplayNetworkMessage` 2007[16]** `(NetworkMsgParamID, b)` - MP banner,
  reads `NETWORK_MSG_PARAM_ST` (see below). ERR uses ~9 times.
- **`DisplayGenericDialog` 2007[01]** and 2007[10] - interactive modal yes/no
  dialog. ERR uses ~14+6 times. Out of scope for "toast", but listed for
  completeness.

The exact game functions behind 2007[02]/[03]/[04]/[10]/[15]/[16] are not
all pinned to RVAs in this document - most of them are reached through a
dispatch table indexed by bank/instruction, so static analysis needs an extra
step. Open work.

## NETWORK_MSG_PARAM_ST - MP role-based messages

The param row backing `DisplayNetworkMessage`. Listed for reference - useful
if you ever want a multiplayer-context-aware notification, but **not useful
for arbitrary custom text** because the strings are fixed FMG entries.

Param structure (Confirmed from `param/generated.rs`, INDEX 87):

- `priority: u16`
- `force_play: u8`
- ~45 `i32` columns, each an **FMG text id in the NetworkMessage FMG**,
  one per network role / context. The exact column the engine picks at
  runtime is determined by the player's current network state (host /
  invader / co-op covenant / NPC invasion / etc.). Column names include
  `normal_white`, `umbasa_white`, `berserker_white`, `sinner_hero_white`,
  `normal_black`, several `force_join_*` variants, `sinner_hunter_visitor`,
  `red_hunter_visitor`, `guardian_of_boss/forest/anolis_visitor`,
  `rosalia_black`, `red_hunter_visitor2`, `npc1`..`npc21`, `battle_royal`,
  several more `force_join_*_npc` variants, `normal_white_npc`.

- There is **no** duration / color / icon column - those are baked into the
  network-message UI widget and not data-driven.

Custom text via this path would require either editing the NetworkMessage
FMG entries the row references, or pointing one of the columns at a custom
FMG entry we inject. Not implemented; not particularly useful for a
solo-context toast.

## TUTORIAL_PARAM_ST - the system ERR's Codex actually uses (small upper-left plaque)

**This is the system that produces the "short narrow upper-left plaque" we
spent so much effort trying to find.** ERR's Codex / medal notifications all
use it, and they are exactly the visual style we want. Confirmed by
decompiling ERR's `common.emevd.dcx` and inspecting its FMG / param data.

**The key insight that the original research missed:** `TUTORIAL_PARAM_ST`
has a `menuType` column. With `menuType = 100` (or other high values) the
game renders a large centered modal card - the "old-style tutorial" look I
documented earlier. **With `menuType = 0` it renders a small toast at the
upper-left** - the codex/medal style. Confirmed live in ERR (it uses
`menuType=0` for every codex row).

**How ERR uses it (from `mod/event/common.emevd.dcx` decompile):**

- Param rows in IDs **1010004..1010290** (~51 rows for codex/medal entries).
- Each row has: `menuType=0`, `triggerType=0`, `repeatType=1`,
  `imageId=0`, `unlockEventFlagId=0`, default `displayMinTime=1s`,
  `displayTime=3s`. The `textId` follows the pattern `<rowId>0` (e.g. row
  1010060 → textId 10100060).
- ERR's EMEVD calls `ShowTutorialPopup(<rowId>, TRUE, TRUE)` from event
  handlers gated on item pickups, kill flags, etc. Master gate flag 9964
  disables the whole codex system when off.
- Sample event 1041603000 → `ShowTutorialPopup(1010010, TRUE, TRUE)` →
  "First Steps Medal I".
- Tested via runtime - the plaque is the upper-left "caption" widget,
  Scaleform movie at `mod/menu/01_060_caption.gfx`. Visible for `displayTime`
  seconds, no sound (or very subtle), one-line, narrow.

**FMG text source** (confirmed by enumerating `mod/msg/engus/menu_dlc02.msgbnd.dcx`):
- **TutorialTitle.fmg** (MsgRepositoryImp slot 207) - header line. ERR uses
  ids like 402760 / 402800 ("Medals", "Starlight Tokens"), with `<img>`
  prefix tags for the medal icon.
- **TutorialBody.fmg** (MsgRepositoryImp slot 208) - body text. ERR uses ids
  10100010..10100290 with `<font color="#beac8b">...</font>` styling for the
  highlighted parts.

**Calling it from a C++ DLL - RESOLVED, and this is what the mod ships.**
The exact game-function path **is** now pinned, and MapForGoblins pins and
ships it. The trampoline is resolved at **RUNTIME via `modutils::scan`** with
the AOB

```
48 8B 05 ?? ?? ?? ?? 8B D1 48 85 C0 74 17 48 8B 88 80 00 00 00 48 85 C9
```

and cached. From it:

- **inner = 0x7EF5B0** - `void(CSPopupMenu*, int, bool, bool)`
- **outer = 0x7EE630** - `void(CSPopupMenu*, int, bool)`
- the **CSPopupMenu singleton** is at **CSFeMan_slot + 0x80**.

This AOB-resolved trampoline is the path the mod actually ships for its
toasts. (RVAs above are pre-2026-05-29-update - resolve via the AOB; see the
update banner near the top of this doc.)

**Implementation as shipped from the DLL:** (a) resolve the trampoline via
the AOB above and grab the CSPopupMenu singleton from CSFeMan_slot+0x80,
(b) inject our own `TUTORIAL_PARAM_ST` rows at runtime via
`inject_tutorial_popup_rows` - the row-injection machinery from
`WorldMapPointParam` in MapForGoblins is reusable, (c) inject our title/body
strings into TutorialTitle / TutorialBody FMGs (the FMG injector already
reads slot 207 and writes slot 208 - see the FMG section below), (d) call
the trampoline with our paramId. Sample row config to mimic ERR:
`menuType=0`, `triggerType=0`, `repeatType=1`, `imageId=0`,
`unlockEventFlagId=0`, `textId=<our_fmg_id>`, `displayMinTime=1.0`,
`displayTime=3.0`.

**Helper DLLs in ERR (not what we want).** `dll/online/erquestlog.dll` is a
separate ERR mod for the quest-tracker UI, not the codex notification. It
hooks `MsgRepositoryImp::LookupEntry` and talk-scripts to inject quest-log
text from `questlog_lang\<lang>.lang`. Standard MSVC PE, not Themida-packed,
but it does NOT call `ShowTutorialPopup` for the codex plaque.

---

Below - what we knew about `TUTORIAL_PARAM_ST` from the param definition
itself, before the ERR codex investigation. Confirmed structure but the
visual implications were misunderstood (assumed centered modal - wrong;
`menuType=0` gives a corner toast).

The richest message-display API the game exposes via params. `TUTORIAL_PARAM_ST`
columns (Confirmed from `param/generated.rs`, INDEX 212):

- `disable_param_nt: bool` (bit 0 of `bits_0`)
- `menu_type: u8` - **Confirmed**: `menuType=0` → small **upper-left toast**
  (the codex/medal style we ship). Higher values render larger / centered
  modal cards.
- `trigger_type: u8`
- `repeat_type: u8` - probably "show once" / "show always" - Guessed
- `image_id: u16` - id of the tutorial illustration (lookup in the tutorial
  texture set)
- `unlock_event_flag_id: u32` - engine sets this flag when the popup is
  acknowledged; supports the EMEVD `IF Tutorial Seen` 2007[46] check
- `text_id: i32` - the body text id (looks up in **TutorialBody** FMG)
- `display_min_time: f32` - minimum seconds the card stays on screen
- `display_time: f32` - total seconds; **the only system with explicit per-
  call duration**

**Concrete 32-byte row layout the mod uses** (row size 32):

- +0x04 `u8 menuType` (0 = upper-left toast)
- +0x05 `u8 triggerType`
- +0x06 `u8 repeatType`
- +0x0C `u32 unlockEventFlagId`
- +0x10 `s32 textId`
- +0x14 `f32 dispMinTime`
- +0x18 `f32 dispTime`

Title comes from **TutorialTitle** FMG (MsgRepositoryImp slot 207, which
MapForGoblins already populates) keyed by the same id as the body. The body
text uses **TutorialBody**.

**To use this from a DLL with custom text:** add a new `TUTORIAL_PARAM_ST`
row at runtime with your chosen `text_id`, inject custom strings into
TutorialTitle / TutorialBody FMGs at that id, and call the underlying
display function (the AOB-resolved ShowTutorialPopup trampoline - see the
"Calling it from a C++ DLL" subsection above; inner 0x7EF5B0 / outer
0x7EE630). This is now pinned and shipped. The FMG injector already reads
slot 207 (TutorialTitle) and writes new entries into slot 208 (TutorialBody)
for the shipped codex banners. Adding a param row at runtime is non-trivial
but done (MapForGoblins does this for WorldMapPointParam, so the machinery
exists).

Use case: notifications that should genuinely look like a tutorial card,
with an image and an explicit duration. Probably overkill for a simple
toast.

## What we tested in MapForGoblins and what visually happened

This is the empirical record from the cycler build (F11 cycles methods,
F10 fires the current one):

| # | Method | Result |
|---|--------|--------|
| 0 | FeSystemAnnounce | ✅ Top of screen, full-width bar, text on the left. **No sound.** Visible for ~15 seconds. |
| 1 | Summon `priority=10 force_play=0 unk48=0` | ✅ Narrow plaque, **horizontally centered**, slightly below screen center. Visible ~6-7 s. **Blinks 3 times** before disappearing. No sound. |
| 2 | Summon `priority=99 force_play=1 unk48=0` | ✅ Same position. Visually identical, possibly less transparent background. Still blinks. No sound. |
| 3 | Summon `priority=1 force_play=1 unk48=1` | ✅ Same position. **Does NOT blink.** ~5 s. **Our text. No sound.** Best Summon variant so far. |
| 4 | proc_status_message direct write (+0x3720 + timer reset) | ❌ Nothing rendered (tested both before and after building a valid DLString via 0x11A3E0). |
| 5 | FullScreenMessage MenuText=41 | ❌ Death/grace sting plays. No visible text. |
| 6 | subarea_name_popup_message_id + welcome_request | ⚠️ Huge center area-name banner - but the text shown was the current area name ("Звёздные пустоши"), NOT our injected FMG text. Soft sound. |
| 7 | blinking_message_id + priority | ✅ Center, smaller font than #6, light sound, **our text DID render**. Looks decent in isolation, just placed at screen center. |
| 8 | Summon `priority=1 force_play=0 unk48=1` | ✅ Identical to #3 (no blink, ~5 s, our text). Confirms `force_play` does not affect visual output. |
| 9 | Summon `priority=1 force_play=1 unk48=2` | ✅ Identical to #1/#2 (blinks 3×). Confirms `unk48` behaves as a boolean: `0` and any non-`1` value blink; only `unk48=1` is "steady". |
| 10-15 | Six MenuString slots direct write (+0x36D8, +0x3768, +0x37A0, +0x37D8, +0x3810, +0x3848) | ❌ All six produced nothing. Writing a valid DLString into any of these slots and bumping the proc-status timer does not render visible text. Confirms the slots-render-directly hypothesis is wrong. |

## Confirmed dead ends and open questions

**Dead ends - don't repeat:**
- `proc_status_messages` ring buffer with a custom id: resolver is templated,
  cannot inject custom text by FMG.
- `display_status_message(menu_man, N)` with a small enum N: each id has a
  baked-in sound played from the resource table.
- Showing the FeSystemAnnounce text in a narrower bar: the bar geometry is
  in the FrontEnd Scaleform movie, not in any param or struct field.

**Open questions worth chasing:**
- Why `FullScreenMessage::MenuText=41` plays a sound but doesn't render
  text from `proc_status_message`. Likely we're missing a write somewhere.
- What the 6 FrontEndView MenuString slots actually do at render time, and
  whether any of them can be triggered from a DLL.
- The exact meaning of Summon `priority` and the full range of `unk48`
  values.
- A clean way to free a standalone game `DLString` (so the Summon path's
  per-call mini-leak goes away).
- The RVA of the game function behind `DisplayStatusMessage` 2007[03] (the
  "bottom status line" EMEVD instruction) - could be a small bottom toast
  if the description holds up.
- Whether the `CSItemGetMenuMan` add function (0x779C70) is callable from a
  DLL with a forged item-name FMG entry, and the full meaning of its stack
  arguments.
- ERR's "small upper-left plaques" - what we set out to find but didn't
  pin to one of these systems. Either ERR uses one of the systems above
  with non-default styling we haven't reproduced, an EMEVD instruction we
  haven't dispatched, or - Guessed - a Scaleform side effect of a path we
  don't yet recognize.

## How to re-verify after a game patch

1. Re-run the AOB scans for CSMenuMan / CSFeMan / FeSystemAnnounce-enqueue
   to confirm the singletons and the announce function still resolve. The
   announce AOB is long and unique; the CSMenuMan / CSFeMan ones are 13-17
   bytes with 4 wildcards.
2. Read `display_status_message` (0x766460) and confirm its tail-call sets
   `[CSFeMan+0x3760]` - if the offset changed, every other CSFeMan offset
   in this document needs to be re-derived from the rust struct.
3. For Summon, confirm 0x843860 still has the
   `mov [rcx], rax; mov [rcx+8], dx; mov [rcx+0xA], r8b; ...` prologue
   pattern. If yes, the calling convention is intact. If no, re-find the
   ctor via callers of the enqueue.
4. For `MENU_COMMON_PARAM_ST` field offsets, check `param/generated.rs` in
   the latest `fromsoftware-rs` - paramdef changes are tracked there.
5. Re-verify the FullScreenMessage dispatch table at **0xD29660**
   (image-relative; absolute VA 0x140D29660) - it lives in the `.text` /
   read-only data that shifts on game updates, so re-derive it by AOB along
   with the other shifted RVAs.
6. Re-run the ShowTutorialPopup trampoline AOB
   (`48 8B 05 ?? ?? ?? ?? 8B D1 48 85 C0 74 17 48 8B 88 80 00 00 00 48 85 C9`)
   and confirm it still resolves (inner 0x7EF5B0 / outer 0x7EE630, popup
   singleton at CSFeMan_slot+0x80). This is the shipped toast path.

## References

- `vswarte/fromsoftware-rs` - `crates/eldenring/src/cs/fe_man.rs`,
  `cs/menu_man.rs`, `param/generated.rs`. The single most useful reference;
  every offset in this document was cross-checked against it.
- `libER` (in `modding_tools/libER`) - `symbols/singletons.csv` and
  `symbols/*.csv` for cross-referencing manager singletons.
- `github.com/metal-crow/ER-Blurbs-Mod` - the only public mod I'm aware of
  that calls `FeSystemAnnounce` enqueue from a DLL with the same offsets.
- `er-common.emedf.json` / `soulsmods` EMEDF - for EMEVD instruction
  signatures.
- Diagnostic / disassembly helper scripts from this research live (gitignored)
  under `scratch/scripts/`: `find_announce.py`, `trace_announce.py`, `disfn.py`,
  `aob.py`, `callers.py`, `_xref3.py`, `_fast.py`.
