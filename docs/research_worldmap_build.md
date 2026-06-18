# World-map marker build path (static RE)

Goal: understand why the map rebuilds all markers on every open, measure the cost,
and assess a hook-based cache so all categories can render without an open-time hang.

Tooling: static disassembly of `eldenring.exe` with pefile + capstone (the on-disk
exe is NOT encrypted - .text entropy ~6.4 - so static RE works). Script:
`scratch/re_map.py`. RVAs below are for the exe version in
`G:\Steam\steamapps\common\ELDEN RING\Game\eldenring.exe` (ImageBase 0x140000000);
re-resolve by AOB on other versions.

## RTTI walk: CSWorldMapPointIns (the marker widget instance)

- RTTI name `.?AVCSWorldMapPointIns@CS@@` (namespace `CS::`) @ .data file-off 0x3ce8470
- TypeDescriptor VA 0x143ce9660 (.data)
- Complete Object Locator candidates 0x143336ae0 / 0x143336b2c
- **vtable VA 0x142b487a8** (.rdata)
- vtable-store sites (inside ctor): 0x140a81213, 0x140a812ed
- **ctor start = 0x140a811e0** (from .pdata bounds)
- ctor's only call xref: 0x140a82d09 (inside the build fn below)

Parent class string `.?AVCSWorldMapPoint@CS@@` @ 0x3ce8440 (not yet walked).

## Build function = 0x140a82a80  (.pdata bounds 0x140a82a80 - 0x140a82ea6)

Entry AOB (24 bytes): `40 55 53 56 57 41 54 41 56 41 57 48 8B EC 48 83 EC 60 48 C7 45 D0 FE FF`
(prologue: push rbp/rbx/rsi/rdi/r12/r14/r15; mov rbp,rsp; sub rsp,0x60).

Shape:
- rcx (r15) = the map-UI owner object; rdx = context (reads +0x34, +0x48).
- r12 = [rdx+0x48] = data source. `call 0x140cf6300` with edx=0x26 then 0x27
  returns a COUNT (two collections enumerated -> two build loops).
- Per element: `call 0x141eb9ed0` (alloc), `call 0x140a7f990`/`0x140a854a0`
  (get/iterate row), then **virtual predicate `mov rax,[rdi]; call [rax+8]`**
  returning bool = the visibility/filter check (dispMask / flags). `test al; je skip`.
- Visible -> builds a small key {dword from rdx+0x34, ptr rdi} on stack and calls
  `0x140a81830` (insert/lookup, rcx=r15) + `0x140a81b50`, then `0x140a84450`
  (loop 1) - these add into containers on r15 (e.g. [r15+0x398]).
- Tail creates the CSWorldMapPointIns instance (ctor call @ 0x140a82d09).

**Callers of the build fn (the map-open / refresh paths):**
- 0x14063d799 (in func 0x14063d400)
- 0x140663b77 (in func 0x1406632f0)

## Why it rebuilds every open (hypothesis, not yet RE-confirmed)

The map is a transient CSMenu screen: its widget/marker containers are constructed
on open and torn down on close, so the build fn runs each open. dispMask/iconId are
decided at build; textEnable/textDisable flags are re-checked live each frame
(that's why hide_killed_bosses is live but show_* is not). Vanilla marker counts are
small so the rebuild was never a bottleneck; our ~7000+ injected markers expose it.

## Next steps

1. **Measure** (decides whether a cache is worth it): hook build fn 0x140a82a80
   (AOB above) and log entry->exit duration; user opens the map with stock ini vs
   the all-on preset. NB the all-on preset already ships & works, so the all-markers
   build cost is already exercised in the field.
2. If the build hitch is the real cost: identify the container that holds the built
   markers on r15 + its create/destroy in the two caller funcs, and whether we can
   keep it alive and skip the rebuild (cache) - needs live memory verification.
3. Live icon swap still blocked separately (iconId bound at build).
