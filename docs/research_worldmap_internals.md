# Elden Ring World-Map Marker Engine: Internals and Patch Points

ImageBase 0x140000000. All addresses are file VAs unless prefixed "+0x" (a profiler
runtime-RIP-minus-module-base value, i.e. file VA = 0x140000000 + that value). The exe is
not packed and was analyzed statically (.text entropy ~6.4). The game was never run for this
analysis. Targets and game files are read-only; no game file was modified.

## Executive summary

Opening the world map with the MapForGoblins marker set (~5000-7000 markers) produces three
distinct stalls. The FIRST open costs ~957ms, of which ~722ms is a per-marker widget-tree
dirty/relayout walk (refresh fn 0x1410d97b0) called 5712 times from one per-marker case
dispatcher (0x1410dfcb0, virtual slot disp 0x228). After the map is closed the framework
DESTROYS the WorldMapDialog (confirmed: the create callback is a std::function factory that
unconditionally heap-allocates a fresh 0x3ED0 object each open, and the close path runs the
deleting-dtor plus operator delete), which causes a ~208-209ms deferred-teardown micro-freeze
(per-marker unregister via a typed-find walk) and a ~373ms cold rebuild on the next open. The
linchpin is the teardown: because the dialog is always recreated, a simple extra reference
cannot preserve the same tree, so the practical levers are (1) suppress or detach/re-adopt the
teardown so the marker objects survive, and (2) cut the first-open per-marker refresh cost. The
manager singleton (global at 0x143d6e9b0) is a separate red-black map that outlives the dialog
and is the better survival anchor than the dialog-side array.

## Call-chain diagram (open / build / refresh / close / teardown)

```
OPEN (screen creation)
  menu framework
    -> std::function invoke-slot _Do_call            0x1407fd4b0  (factory, 0 E8 callers)
       -> WorldMapDialog ctor-wrapper                0x1409cf940  (alloc 0x3ED0 via 0x141eb9ed0)
          -> WorldMapDialog ctor                     0x1409cef10  (vtable 0x142b2d7e0)
                                                                  (marker container lazy, not inited here)

BUILD (per-frame, virtual slot 30 on world-block-resource)
  open_handler_A (WorldLodBlockRes, full world)      0x14063d400  (gate cmp [this+0x40],0)
  open_handler_B (WorldBlockRes, sub-area)           0x1406632f0
    -> buildMarkers (build_loop)                     0x140a82a80  (this=mgr singleton 0x143d6e9b0)
       sites 0x14063d799 / 0x140663b77
       - kind 0x21 only: alloc 0x110 CSWorldMapPointIns 0x141eb9ed0 + ctor 0x140a811e0
       - RB-insert node into manager map  [mgr+0x398]   (key=node+0x20, value smartptr=node+0x28)
       cost measured ~0ms

REFRESH / per-marker update (the 722ms first-open cost)
  outer per-marker iterator (vtable +0x228, driver not singled out)
    -> per-marker case dispatcher                   0x1410dfcb0  (true entry; tail 0x1410dfd1a is continuation)
       -> refresh fn (dirty/relayout walk)          0x1410d97b0  (site 0x1410dfd4c, return 0x1410dfd51 = 5712x / 722ms)
          leaf 0x1410d9860
       -> ce390 ChildList_VisitAll  x3 (edx=0,1,3)  0x1410ce390  (sites 0x1410dfd56/63/70)
          -> live flag re-eval (textEnable/textDisableFlagId)

RENDER TAIL (~180ms after each open)
  non-eldenring DLL leaf                             +0x6CB7900F0 (GPU driver / Scaleform texture upload)

CLOSE -> DEFER
  WorldMapDialog::Update (slot 2)                    0x1409c32f0  (advances close ease; frees nothing)
  framework pops screen, drops last DLReferencePointer (dec via 0x141eba200)
    -> deleting-dtor (WMD)                           0x1409cf8f0  (slot0; or base thunk 0x1409c1c10)

TEARDOWN (the ~208-209ms micro-freeze)
  -> WorldMapDialogBase dtor (real body)            0x1409c1080  (vtable 0x142b2b468)
     per-entry loop                                  0x1409c1170  (count [r12+8], base [rsi+0x3950], stride 8)
       per entry:
         call [marker.vtbl+0] slot0                 0x140a812d0  (finalize/detach in place, bDelete=false)
         call [*0x143d87350 -> +0x68](marker)       manager unregister-by-pointer
           -> typed-find walk                       0x14113feb0  (continuation 0x14113feda; hot loop 0x14113ff50)
              -> AddRef 0x1411800d0 (leaf +0x11800F0) / Release 0x141176280
  then operator delete size 0x3ED0                   0x1424faa14

REOPEN-AFTER-TEARDOWN (373ms cold rebuild)
  buildMarkers re-creates all markers; cold per-type result cache in 0x14113feb0
  -> O(N) linear walk per query, leaf +0x11800F0 + 0x113FFxx family
```

## Confirmed facts (survived verification or already VERIFIED)

- Refresh fn (per-marker widget-tree dirty/relayout walk): VA 0x1410d97b0. Body hot at
  0x1410d9860. SHARED across UI screens; blanket entry-hook hangs boot. Map per-marker call
  site = 0x1410dfd4c, return address 0x1410dfd51 (the profiler's "+0x10DFD51", 5712x / 722ms).
  VERIFIED. Note (corrected from R3): this fn is a tree dirty/relayout walk (linked list
  [rbx+0x90] / alt [rbx+0xa8], sets dirty bit "or byte[rbx+rax*4+0x30],2", calls virtual
  [rax+0x360] then 0x1410ca3c0). It is NOT icon-quad drawing/batching.

- The values 0x1000015 and 0x1000016 inside the refresh fn are HASHMAP LOOKUP KEYS, not a
  draw-scope begin/end pair. 0x1410cd160 is an open-addressing hashmap-find (key in edx, map in
  rcx), called on the widget container at [rsi+0x190]. VERIFIED (corrected from R3). There is no
  per-marker draw scope in this path.

- Per-marker case dispatcher: VA 0x1410dfcb0. VIRTUAL method, 0 E8 callers, stored at vtable
  slot 0x142cbc970 (disp 0x228) of the marker-screen widget class (vtable 0x142cbc748). True
  entry has prologue "push rdi; sub rsp,0x20"; .pdata tail 0x1410dfd1a is a CONTINUATION region,
  not a function entry; do NOT hook the tail. Sibling cases: 0x1410dfbe3 (site 0x1410dfc51),
  0x1410d9710 (site 0x1410d9781). VERIFIED. Body calls refresh once (0x1410dfd4c) then ce390
  three times (edx=0,1,3 at 0x1410dfd56/63/70).

- ce390 ChildList_VisitAll: VA 0x1410ce390. Called 3x per marker. Selects a child collection,
  iterates via 0x1410d8510, body 0x1410cd570 -> visitor 0x1410b2a90. Live flag re-eval of
  textEnableFlagId/textDisableFlagId happens here per frame. dispMask00 / iconId read once at
  build in ctor blocks +0x90/+0xd0. VERIFIED.

- buildMarkers (build_loop): VA 0x140a82a80. this = manager singleton (slot 0x143d6e9b0, ctor
  0x140a81cd0, size 0x3a8). Only kind 0x21 allocates a 0x110-byte CSWorldMapPointIns (alloc
  0x141eb9ed0 + ctor 0x140a811e0, single call site 0x140a82d09). RB-tree-inserts into the
  manager map at [mgr+0x398] (key node+0x20, value smartptr node+0x28). VERIFIED. Build cost
  measured ~0ms. Exactly two call sites: 0x14063d799 and 0x140663b77.

- CSWorldMapPointIns: RTTI ".?AVCSWorldMapPointIns@CS@@", vtable 0x142b487a8, ctor 0x140a811e0,
  size 0x110. Fields: +0 vtable; +0x18 param subobject (WorldMapPointParam, SoloParam type
  0x8d, via 0x140d298c0); +0x70 resolver/cache slot; +0x90 and +0xd0 = two 0x40-byte text/icon
  display blocks (param floats +0x2b0 / +0x2b4). VERIFIED.

- WorldMapDialog is recreated, not reused (R1 SUPPORTED). Object vtable 0x142b2d7e0;
  RTTI ".?AVWorldMapDialog@CS@@". The create callback _Do_call 0x1407fd4b0 is a std::function
  invoke-slot (its VA sits at 0x142abb910 slot +0x10 in a _Func_impl fn-ptr table; 0 E8
  callers). It unconditionally heap-allocates size 0x3ED0 (via wrapper 0x1409cf940 -> alloc
  0x141eb9ed0) and runs ctor 0x1409cef10 EVERY open. No cached/pooled dialog pointer is read or
  written anywhere in the factory path. On close the deleting-dtor 0x1409cf8f0 (slot0) runs real
  dtor 0x1409c1080 then operator delete of size 0x3ED0 (0x1424faa14). VERIFIED. Implication:
  an extra reference on the dialog CANNOT preserve the same tree across open/close; it would only
  block the free path and leak one 0x3ED0 dialog per open while the next open still builds a fresh
  tree.

- Teardown loop and leaf source (R4 SUPPORTED). Real dtor 0x1409c1080 is WorldMapDialogBase
  dtor (vtable 0x142b2b468, RTTI ".?AVWorldMapDialogBase@CS@@"). Per-entry loop 0x1409c1170:
  count via call [r12+8] (r12 = rsi+0x3940), base [rsi+0x3950], stride 8. Per live entry: (a)
  call [marker.vtbl+0] = marker primary slot0 0x140a812d0, a finalize/detach in place (resets
  +0x70/+0x98/+0xd8; reaches neither find nor refcount); (b) call [*0x143d87350 -> +0x68] with
  rcx=mgr, rdx=marker = manager unregister-by-pointer (bytes ff 50 68). VERIFIED. The
  +0x113FFxx / +0x11800F0 micro-freeze leaf is produced by the typed-find walk (entry
  0x14113feb0, continuation 0x14113feda-0x14113ffdf, hot loop 0x14113ff50), which directly calls
  AddRef 0x1411800d0 (leaf +0x11800F0) and Release 0x141176280. The marker dtor 0x14087be40
  (slot0 of a SEPARATE vtable 0x142ad6688) reaches none of find/refcount/release (depth-4 search
  = None), so the cost is per-marker UNREGISTER, not the dtor. The find-walk is reached via
  manager virtual unregister methods (three consecutive vtable slots: 0x1410dbb70 / 0x1410dbea0
  / 0x1410dc260). Therefore patches targeting the find DO reduce the micro-freeze.

- Typed-find walk semantics (R5 corrected). VA 0x14113feb0 is a type-lookup-with-result-cache
  over a typed container: base [r15], count [r15+8] DWORD guard at [r15+0x20] also doubles as
  the cache slot, stride 0x10; query type at [r13]; mode bool r8b. The stock engine ALREADY
  revalidates the cache on every call by re-comparing the cached entry type [cached]==[r13]
  (0x14113ff18 / 0x14114001e); the fast path "mov rax,[r15+0x20]; ret" is at 0x14113ffb8. The
  [entry+0x6b]&1 enable bit is consulted ONLY inside the linear search loop (0x14113ff5c /
  0x14114006c) to skip disabled entries WHILE searching, never on the cache fast-path. The cache
  is a per-TYPE result singleton in a generic container, with 8 heterogeneous E8 callers passing
  different r13 query types and different r8b modes (two derive r8b live from a flag byte via
  cmp byte[+0x128],6 + seta). TRUE entry for hooking = 0x14113feb0 (prologue push
  rbx/r13/r15 then sub rsp,0x30); NEVER hook 0x14113feda (chained continuation).

- AddRef / smartptr helper: VA 0x1411800d0 (size 0x56, leaf +0x11800F0), 43 direct E8 callers.
  Release/destruct helper 0x141176280. Allocator/TLS helper 0x14117ec20. These are FromSoftware
  DLReferenceCountPtr lock/upgrade primitives operating on lock records (target refcount at
  object+0x18), NOT on the marker tree itself. VERIFIED.

- Per-widget type-cache vector container = at marker-widget +0x18/+0x20 then +0xd8 (base [r15],
  count [r15+8], cache slot [r15+0x20]). Entry layout: [entry] = type/vtable qword (compared to
  [r13]), [entry+0x6b]&1 = enable/visible gate (search-only), [entry+0x6a] bit10 = visibility,
  [entry+0x6d] = slot index. VERIFIED.

## Still uncertain (refuted, unresolved, or low confidence)

- Dialog array vs manager map ownership (R2 REFUTED as previously stated). WorldMapDialog
  +0x3940/+0x3950 (array of element pointers, walked by dtor 0x1409c1080) and the manager RB-map
  at [mgr+0x398] (populated only by buildMarkers 0x140a82a80) are two structurally independent
  containers touched by DISJOINT code. Static disasm does NOT establish they hold the same
  object pointers, and it positively shows neither container OWNS the marker objects: both
  teardown paths destruct each entry via slot0 dtor with bDelete=false then hand the object back
  to a SEPARATE owning allocator (dialog: singleton *0x143d87350; map reject branch: address-
  range arena via 0x140e1a210), using the same return-to-owner idiom call [[owner]+0x68].
  Consequence: detaching one container while the object is freed/handed-back risks double-free
  or dangling. Part of the map erase/store path thunks into relocated stubs >0x145000000 and
  could not be statically traced. VA: 0x140a82a80 (insert) / 0x1409c1170 (dialog walk) /
  0x140e1a210 (arena lookup).

- No per-marker draw scope exists (R3 REFUTED). The premise that hoisting refresh once-per-frame
  preserves a draw-ordering scope is unsupported: 0x1410cd160 is a hashmap-find and
  0x1000015/0x1000016 are lookup keys, not begin/end. A hoist (if attempted) must be justified
  against layout/dirty-propagation semantics, not a draw scope. The "all 5712 calls in one
  frame" and "ordering-independent" sub-claims remain INFERRED, not verified.

- Manager vtable slot +0x68 identity (R4 unresolved sub-point). The teardown calls
  [*0x143d87350 -> +0x68](marker), and the find-walk is provably reached via manager virtual
  unregister methods, but that slot +0x68 specifically targets one of the find-walk callers was
  NOT confirmed statically (the manager singleton is runtime-allocated; *0x143d87350 is null in
  the file). Runtime vtable read needed. Does not change the conclusion that the cost is
  per-marker unregister. VA: 0x1409c119b / 0x143d87350.

- Outer per-marker iterator (the driver that calls 0x1410dfcb0 5712x) was NOT singled out
  (candidates 0x14065a930 / 0x14038e380 / 0x14042fa10 / 0x140454340, MEDIUM confidence). A clean
  frame boundary between the 5712 calls is therefore INFERRED, not proven.

- Whether the manager RB-map [mgr+0x398] survives close intact and is safely reusable by a
  guarded buildMarkers. The dialog dtor's unregister calls suggest the dialog drives manager
  state, so the map may be emptied during teardown. Needs runtime confirmation. VA: 0x140a82a80
  / [mgr+0x398].

## Patch points (ranked)

Patch 1 (extra dialog reference) from the original plan is INVALID per R1 and is dropped:
the dialog is recreated every open, so an extra ref cannot keep the same tree and leaks one
dialog per open. Do not use it.

### Patch A (PREFERRED) - suppress the teardown trigger and re-adopt the marker set
- Hook: WorldMapDialogBase dtor body 0x1409c1080 (clean entry; .pdata 0x1409c1080-0x1409c191d)
  OR the deleting-dtor thunk 0x1409cf8f0.
- Mechanism: when a "keep map resident" flag is on, DETACH the live marker set before the
  per-entry loop at 0x1409c1170 (move the dialog's container pointer dialog+0x3940/+0x3950 and,
  critically, the manager map ownership into a static cache; zero the dialog's copy) so the dtor
  walks an empty container and frees nothing live. Re-adopt at the next ctor 0x1409cef10 /
  build entry. Because the marker objects are owned by an EXTERNAL allocator (R2), you must keep
  BOTH the dialog-side reference AND the manager registration consistent, or skip the unregister
  while guaranteeing no other path frees the object.
- Expected effect: eliminates the ~208ms micro-freeze AND the ~373ms cold rebuild on reopen
  (markers re-adopted, not rebuilt).
- Risk: HIGH if done naively. The per-entry loop both destructs each marker in place (bDelete
  =false) AND unregisters it from the manager *0x143d87350. Skipping the loop while leaving
  registrations live = dangling registrations -> crash on next build. Detach-and-readopt of a
  coherent set is the only safe form, and R2 shows the ownership boundary is not the container,
  so this requires runtime validation of the external owner's state. Double-free at real shutdown
  if the static cache is not released exactly once.
- Next-step experiment: at runtime, log from a non-modifying hook on 0x1409c1080: dialog+0x3940
  count, [mgr+0x398] size before and after the loop, and the resolved target of
  [*0x143d87350 -> +0x68]. Confirm whether the manager map is emptied by the loop. Only then
  decide detach vs skip.

### Patch B - typed-find fast path on the rebuild / unregister hot loop
- Hook: 0x14113feb0 (TRUE entry; prologue push rbx/r13/r15 + sub rsp,0x30; safe for a 5-byte
  jmp). NEVER hook 0x14113feda (continuation).
- Mechanism (CORRECTED per R5): do NOT blanket-force the [r15+0x20] cache fast-path. The stock
  engine already self-revalidates the type-match every call, and the function serves 8
  heterogeneous callers with different r13 types and r8b modes on shared containers, so an
  unconditional "return cached" would return wrong-type entries (type-aliasing break). If used,
  scope strictly to ONE call site with a known-fixed r13 and r8b, AND preserve the [cached]==
  [r13] revalidation. The realistic win here is collapsing the cold-cache O(N) linear walk on
  rebuild, not removing the per-call recheck.
- Expected effect: reduces the dominant +0x11800F0 / +0x113FFxx cost of the 373ms rebuild and
  some of the 208ms unregister, without changing lifetime.
- Risk: MEDIUM. Wrong scoping returns stale/wrong-type entries (vanished or mis-typed markers).
- Next-step experiment: at runtime, capture rcx (container), rdx (r13 -> [r13] type), and r8b at
  each of the 8 call sites; confirm which site(s) belong to our marker container and whether r8b
  is stable. Only patch a confirmed-single-type site.

### Patch C - guard buildMarkers on an already-populated manager map
- Hook: buildMarkers 0x140a82a80 (clean non-virtual E8 entry; exactly 2 call sites).
- Mechanism: skip rebuild when [mgr+0x398] is already populated (size [mgr+0x3a0] matches
  expected) AND area-id key [rdx+0x34] matches the cached area. Must be paired with Patch A so
  the map actually survives close.
- Expected effect: eliminates re-allocation of 5000-7000 CSWorldMapPointIns on reopen (the
  373ms) IF the manager map survived.
- Risk: showing the wrong region's markers if the area-id gate is wrong; gate strictly on
  [rdx+0x34]. By itself does NOT stop teardown.
- Next-step experiment: confirm (Patch A experiment) that [mgr+0x398] is non-empty after close;
  log [rdx+0x34] across opens of different regions to validate the area-id key.

### Patch D (current shipped hook, KEEP as baseline) - return-address skip at 0x1410dfd51
- Hook: refresh fn 0x1410d97b0 entry; if return address == 0x1410dfd51, return immediately.
- Mechanism/effect: skips refresh only on the per-marker map path (return 0x1410dfd51). Boot and
  other screens reach refresh via different return addresses (0x1410dfc51 / 0x1410d9781), so they
  are unaffected. Profiler-confirmed 5712 -> ~27 calls. Boot-safe.
- Risk: LOW (already validated in shipping). Does not address rebuild or teardown.
- Next-step experiment: none required; this is the proven baseline. Verify in-game that all icons
  still render after Patches A-C are layered on top.

### Patch E (research only) - first-open refresh reduction
- Status: the original "hoist refresh to once-per-frame around a draw scope" (Patch 4) is NOT
  justified by the bytes (R3): there is no per-marker draw scope. Any reduction of the 722ms must
  be argued against the per-marker dirty/relayout semantics of 0x1410d97b0 (linked-list walk,
  dirty bit [+0x30], virtual [rax+0x360]).
- Possible mechanism: with Patch A keeping the tree resident, the first cold open is paid once
  and never repeated; that is the more reliable path to "no repeat freeze" than trying to make
  the cold relayout itself cheap. Treat in-frame refresh reduction as a later spike only.
- Next-step experiment: instrument 0x1410d97b0 to log per-call duration and the node count it
  walks; determine whether intermediate per-marker calls are redundant (same dirty state) before
  attempting any skip.

## DO-NOT list (verified hazards)

- Do NOT MinHook 0x1410d97b0 entry unconditionally (shared across UI; hangs boot).
- Do NOT MinHook 0x1410dfd1a as a function entry (.pdata continuation) - hook 0x1410dfcb0.
- Do NOT MinHook 0x14113feda (chained continuation) - hook 0x14113feb0.
- Do NOT blanket-force the 0x14113feb0 cache fast-path across all callers (type-aliasing break,
  R5) - scope to a single known-type call site and keep the type recheck.
- Do NOT skip just the teardown loop (0x1409c1170) while freeing the container - leaves dangling
  manager registrations -> crash. Detach-and-readopt only.
- Do NOT rely on an extra dialog reference to keep the tree alive (R1) - dialog is recreated
  every open; this leaks and does not preserve the tree.
- Do NOT blanket-skip ce390 0x1410ce390 (builds child sub-lists; blanks markers).

## Recommended order

1. Runtime instrumentation pass: non-modifying hooks on 0x1409c1080 (confirm 209ms frame; log
   dialog+0x3940 count, [mgr+0x398] size before/after loop, resolved [*0x143d87350 -> +0x68]
   target) and on the 8 call sites of 0x14113feb0 (log container / r13 type / r8b).
2. Keep Patch D (current) as the first-open baseline.
3. Build Patch A (detach/re-adopt) once the runtime data shows whether the manager map survives
   and who the external owner is.
4. Layer Patch C (guarded build) on top of Patch A.
5. Add Patch B (scoped find fast-path) only if a single-type call site is confirmed and a rebuild
   path still remains.
6. Treat Patch E (in-frame refresh reduction) as a later spike; prefer keeping the tree resident.

## Runtime validation and corrections (2026-06-18)

Verified live with a render-thread sampling profiler (suspend + RIP + a .text-filtered
stack scan = poor-man backtrace) plus a one-time loaded-module dump and a non-modifying
hook on the close dtor. This section OVERRIDES static claims where they conflict.

Save tested had 7375 markers. Three stalls, each with its driver chain (offsets are
runtime-RIP minus eldenring base; file VA = 0x140000000 + offset):

- FIRST open = ~1577 ms. 1228 ms is the per-marker relayout refresh fn 0x1410d97b0
  (leaf 0x10d9860) called 7375x. Clean call chain: 0x74a810 -> 0x1ebc320 ->
  0x10dfd50 (the per-marker dispatcher 0x1410dfcb0 return site).
- REOPEN after a committed close = ~589 ms. The refresh fn is correctly SKIPPED by the
  shipped return-address hook (Patch D; ~56 calls). The remaining cost is the COLD
  per-type lookup: typed-find 0x14113feb0 (leaf family 0x113ff60/90) doing O(N) linear
  scans with an AddRef/Release (0x1411800d0 leaf 0x11800f0 / 0x141176280) per element,
  for ~8 query types. Clean chain: 0x74a810 -> 0x1124770 -> 0x2552310 -> 0x1ebe0a0 ->
  0x13db60 -> 0xd7fa30 -> 0x2520900 -> 0x10dc010 (the last in fn 0x1410dbea0, one of the
  three map-widget per-marker virtual methods 0x1410dbb70/0x1410dbea0/0x1410dc260).
- MICRO-FREEZE ~1 s after close = ~155 ms, 96% in module-mapped address that resolves to
  ntdll.dll + 0x1600f0 (heap free / coalesce). It is the deferred free of the ~7375 pin
  allocations, NOT GPU and NOT the refcount unregister. Killed only by not freeing.

0x74a810 is the common top driver for both opens (the map per-frame update/build entry).

Container identity (resolves the "where do the markers live" question): the marker
container is `WorldMapPinDataList<WorldMapPointPinData>` (vtable 0x142b2b3e0), EMBEDDED in
WorldMapDialog at +0x3940 (it is a sub-object; its first qword is the class vtable in
.rdata, which is why the probe saw a constant address across closes - that was the vtable,
not a persistent object). The 7375-pin list lives and dies WITH the dialog.

Corrections to the static synthesis:
- The close dtor 0x1409c1080 fires IMMEDIATELY at close, not ~1 s later; it is not the
  deferred free. The deferred ntdll heap free is the ~1 s-later micro-freeze.
- The manager singleton anchor [0x143d6e9b0] + 0x398/0x3a0 read size 0 -> 0 every close;
  the live markers are NOT in that RB-map (or that offset is wrong). buildMarkers
  0x140a82a80 still inserts on its `this` (rcx), but that is not the survival anchor we
  assumed. The external-owner chain [*0x143d87350]+0x68 resolved to garbage at runtime.
- typed-find 0x14113feb0 has 0 direct E8 callers (it is virtual-dispatched); the doc's
  "8 E8 callers" was wrong. Hook it by address if needed, but trace callers via vtables.
- There is NO cheap keep-a-reference fix: the dialog AND its embedded pin list are fully
  recreated on every open (R1 confirmed). "Instant reopen before teardown" is catching the
  close before it commits, not a framework reuse window - a committed close always rebuilds.

Implementation options being A/B tested via a runtime overlay switch (map optimization
mode, read only at map-open while the map is closed):
1. Amortize the per-marker build across frames (soft-populate). Safe, main-thread; fixes
   the perceived freeze for first open and reopen. Does not remove the 155 ms ntdll free.
2. Resident dialog: hijack factory 0x1407fd4b0 to return a cached dialog + prevent the
   delete (deleting-dtor 0x1409cf8f0) + re-init per-open state ourselves. Removes the
   589 ms reopen AND the 155 ms free (instant reopen), but invasive and crash/stale-state
   prone; first open still paid once.
3. Cut only the 589 ms reopen: a scoped fast-path/cache for the cold per-type lookups on
   our container. Narrower; first open and teardown unchanged.

Threading the game build is infeasible (single-threaded widget tree + thread-local
allocator 0x14117ec20 + shared refcount 0x1411800d0; we can only hook entries, not add
locks in callees). Frame-amortization is the safe form of "spread the cost"; threads are
the unsafe form.

## Final shipped optimization (fast_map_open)

After the full research above, what actually ships is a single, conservative option
`fast_map_open` (ini [Goblin] section, default ON; renamed from the earlier
fast_map_reopen + smooth_first_open pair). It does two things, both "do less / spread
the cost", no memory or lifecycle hacks:

1. Skip the redundant relayout on RE-open. The game rebuilds the marker widget tree on
   every open; after the first build (latched by g_built in on_present, independent of
   the dev profiler) the per-marker relayout (refresh fn 0x1410d97b0) + the child-list
   passes (ce390 0x1410ce390) are skipped at the map's build call site. Reopen no longer
   re-pays the biggest cost.
2. Amortize the FIRST open. On the first build the per-marker relayout calls are queued
   and returned immediately (the open frame stays cheap), then replayed a budget per
   frame (AMRT_PER_FRAME=500) from on_present, so markers settle in over ~0.5s instead
   of one long freeze. Only the relayout is deferred - ce390 (structure) runs
   synchronously, because deferring it left the map a black frame.

Safety / lifecycle: the WorldMapDialogBase dtor (0x1409c1080) is hooked solely to CLEAR
the replay queue on close, so a close mid-replay never replays into freed markers.

Patch-resilience: every hook is AOB-resolved, NOT hardcoded RVA. The refresh / ce390 /
dtor function entries are AOB. The per-marker call sites that the skip/amortize gate on
(return addresses of the refresh + 3 ce390 calls in dispatcher 0x1410dfcb0) are resolved
by a wildcarded AOB over the call region (call E8 rel32 targets are `??`-wildcarded, so a
game update that moves functions does not break the match); the return addresses are
fixed offsets from the match (M+0xC / +0x16 / +0x23 / +0x30). If the AOB ever fails to
match, g_map_callsite stays 0 and the optimization safely no-ops (game runs vanilla-slow,
no crash).

Tried and REMOVED (kept here as the record, not in code):
- Resident dialog (reuse the dialog across opens): crashes - the pin list is embedded in
  the dialog (+0x3940) and the framework teardown can't be skipped (null-vtable child
  cleanup, dump eldenring.exe.16088.dmp).
- Memoized typed-find lookup: 0% hit rate (each marker queries its own container, so the
  lookups are genuinely distinct) and the per-call lock added overhead.
- Background pre-build / threading: infeasible (single-threaded UI + thread-local
  allocator).

Residual cost (the hard floor, accepted): per-marker pin construction (typed-find +
refcount, ~600ms with ~7000 markers) runs once per open and could not be cut safely. The
small black flash on open is the game's own native map transition, not ours.

## World->screen projection: transform fields (STATIC RE, 2026-06-20)

Pure static disasm (capstone) of the WorldMapDialog code cluster [0x1409B0000,0x1409E8000].
Resolves the live-RE "which field is zoom" ambiguity. All file VAs. Evidence = exact
instruction sites. Scripts: scratch/scripts/_proj_0*.py (_proj_05_scoped.py is the
authoritative scan; byte-window scans over-/under-match SSE and are unreliable).

### Field roles on WorldMapDialog (CONFIRMED by instruction width + dataflow)
- +0x0A38 / +0x0A3C = view CENTER (map-space Vec2, float). Read as float in Update
  (0x1409C337B movss xmm0,[rbx+0xa38]; 0x1409C3397 [rbx+0xa3c]); written as a qword pair
  (0x1409C33F6 mov [rbx+0xa38],rax) from the eased target. CONFIRMED view center.
- +0x0A40 = CURRENT (eased) zoom, float. Written 32-bit from target (0x1409C361F
  mov [rbx+0xa40],eax  <- eax = [rbx+0x2b58]); read as float (0x1409C365A movss
  xmm0,[rbx+0xa40]; compared to target at 0x1409C3662 ucomiss xmm0,[rbx+0x2b58]).
- +0x2B58 = TARGET zoom, float. The value +0xA40 eases toward. The per-frame broadcast
  (fn 0x1409C38D0) uses +0x2B58 DIRECTLY to derive per-layer scale: 0x1409C3B4A
  movss xmm2,[rdi+0x2b58]; 0x1409C3B56 divss xmm0(=1.0),xmm2  => layer_scale = 1.0/zoom.
  0x1409C3BC9 divss xmm1, [const 2.25]  => one layer slot (+0x32c8) = zoom/2.25.
  THE LIVE ZOOM SCALAR IS +0x2B58 (target) / +0x0A40 (eased current). For drawing in
  lockstep with the game's rendered frame use +0x0A40 (what the layers were built from);
  +0x2B58 is the value they converge to.
- +0x0A44/+0x0A45/+0x0A46 = dirty/changed flags (byte/word stores 0/1/0x101), NOT data.
- +0x2EAC..+0x2EB8 = center pan-ease triple (tgt0 +0x2EAC, tgt1 +0x2EB4), qword Vec2 each;
  tracks the center (this is the "+0x2EAC stored 3x" the live RE saw).
- +0x2F24 ease_t, +0x2F28 ease_dur, +0x2F2C ease_a, +0x2F30 ease_b: smoothstep/ease params
  fed to helper 0x1409E6650 (a clamp+interp, returns eased scalar). +0x2F28 is the ease
  DURATION denominator (divss [2f24]/[2f28] = progress fraction), NOT zoom. This is why the
  live read of "+0x2F28 = 0.5" did not change on zoom. The 2.25 the live RE saw at +0x2F2C
  was an ease source value, coincidental.
- +0x0D88..+0x0D94 = LOGICAL viewport rect, written wholesale from a const block
  (0x1409BFE04 movups [rsi+0xd88], xmmword [rip] -> const @0x142B2BFB0 = {0,0,1920,768}).
  So the UI logical canvas is 1920x768 (FromSoft menu space), NOT 1920x1080. +0x0D94 holds
  a runtime aspect/scale (clamped with a NaN guard at 0x1409BFE38/0x1409BFE94). The
  on-screen pixel size = logical(1920x768) * device render-scale; the "2.25" question is a
  UI render scale (real_res/logical), and 768 is the logical letterbox height. CONFIRMED.

### The 2.25 constant: it is a CONSTANT, not a field
.rdata float 2.25 @0x142B2BFA0, used at 0x1409C3BC9 as a fixed divisor (zoom/2.25 for the
icon/text layer). The 1.0 @0x14329E678 is the 1/zoom numerator. The 0.5 @0x14329E660 is the
rect-center factor in the pan-clamp (0x1409CD2EE). None of these are live dialog state.

### Projection architecture (why there is no single closed-form line in the dialog)
The dialog does NOT itself map each pin map-coord to a pixel. Per frame it:
1. eases center (+0xA38) and zoom (+0xA40) toward targets (+0x2EB4 / +0x2B58) [Update
   0x1409C32F0],
2. broadcasts scale = 1.0/zoom (and zoom/2.25 for icons) to ~10 child layer widgets
   (+0x30D8,+0x31D0,+0x32C8,+0x33C0,+0x34B8,+0x35B0,+0x36A8,+0x3898 ...) via
   vtable call [layer+0x38] [broadcast fn 0x1409C38D0],
3. pushes center+scale into a MapView camera sub-object with a view-rect at +0x340/+0x344
   (min) and +0x348/+0x34c (max) and a scale at +0x380; pan stored at +0x378
   [apply fn 0x1409CD1C0; rect-transform leaf 0x1409CE190].
The per-pin pixel is then produced inside the generic Scaleform/UI widget layout from
(map_coord, layer_scale, layer_origin) - distributed, not a single instruction.

### Practical world->screen formula for the overlay (to verify live, HIGH-conf on fields)
Read from the live WorldMapDialog:
  cx,cz   = float[dialog+0x0A38], float[dialog+0x0A3C]   // view center (map-space)
  zoom    = float[dialog+0x0A40]                          // eased current zoom (use this)
  vp      = float4[dialog+0x0D88] = {0,0,1920,768}        // logical viewport
  scale_dev = device_pixels / 768.0  (UI render scale; or read +0x0D94 family at runtime)
Then, matching the camera's (map-center)*scale form (scale = 1.0/zoom in the broadcast):
  logical_x = (pin_mapX - cx) / zoom + 1920/2
  logical_z = (pin_mapZ - cz) / zoom +  768/2
  screen_px_x = logical_x * scale_dev
  screen_px_y = logical_z * scale_dev
Axis order is X then Z (the +0x10 pin pair is (X,Z)); no Z flip is applied at the dialog
level (the camera rect min/max already encode orientation). The exact unit of "zoom" (world
units per logical px vs its reciprocal) and any Y/Z sign MUST be confirmed with ONE live
pin: dump dialog +0xA38/+0xA40, a known pin +0x10, and that pin's on-screen pixel, then fit
divide-vs-multiply and the sign. The fields and the 1/zoom relationship are static-confirmed;
the single scalar calibration (and +0x0D94 device-scale read) is the only live step left.

### Key function RVAs
- WorldMapDialog::Update (ease cur->tgt)            0x1409C32F0
- Per-frame transform broadcast (scale=1/zoom)      0x1409C38D0   (uses +0x2B58)
- Transform apply / pan-clamp (camera +0x340..0x380) 0x1409CD1C0
- Rect-transform leaf (NaN-guarded)                 0x1409CE190
- Ease/smoothstep helper                            0x1409E6650
- Sibling per-frame apply (same divss [2f24]/[2f28]) 0x1409BFC.. (site 0x1409BFC9B)
- Setter "snap center+zoom+flags" (init)            site 0x1409BE677-0x1409BE68E
- Logical viewport const {0,0,1920,768}             .rdata 0x142B2BFB0 (written @0x1409BFE04)

### Open / needs-live
- The mouse-WHEEL handler that writes target zoom +0x2B58 is NOT in the dialog code range
  (no disp32 write to +0x2B58 anywhere; it is set through a pointer). Target zoom flows from
  a config/state object at [dialog+0x0A48] (e.g. [cfg+0x34] -> dialog+0xA94 at 0x1409C39E4).
  The wheel itself mutates that input/config subsystem. Not required to READ live zoom.
- Device render-scale exact source (+0x0D94 vs a global UI-scale) - read live.

## Path-A GFX live-icon injection: 3-task RE verdict (2026-06-20)

Live target PID 28508, eldenring base 0x7FF6FFB90000 (file VA = base + (liveVA -
0x140000000)). Game was running our MODIFIED worldmap gfx via the mod loader (loose
.tga + modified 02_120_worldmap served from the mod dir; on-disk game menu/ is stock
vanilla 348-frame). READ-ONLY (OpenProcess VM_READ). Scripts scratch/re_gfx_live11..24.

### Resolving the live worldmap MovieDataDef (handle array moves; def persists)
Prior session's CSMD handle array (0x7ff2e7ea0700) had shifted; the GFx MovieDataDef
itself persists (cached/refcounted) across map close. Re-resolve EACH session:
1. scan committed RW for CSScaleformMovieDef vtable (file 0x142bbb310).
2. for each instance, payload = [handle+0x10]; pick the one whose OWN url buffer
   (hop<=1, not deep BFS) contains '02_120_worldmap' - deep BFS gives false hits
   because all menu movies cross-link a shared atlas/context (8 candidates resolved
   the url at hop 3; only the true def at hop 1).
This session: handle 0x7ff2e7f8bca0, payload (MovieDataDef) 0x7ff2dffd60a0,
vtable exe+0x2CC0B80. Its url buffer @ 0x7ff2dffeb0ab = "02_120_worldmap.gfx".

### TASK 3 (the blocker) - VERDICT: must REGISTER new resources; mechanism = the
### engine already does it for us via DefineExternalImage2-by-path. CONFIDENCE HIGH.
- The stock 02_120_worldmap.gfx (348 frames) imports NONE of the textures our frames
  need: MENU_MAP_MemoCursor, MENU_Tab_* , MENU_ItemIcon_* are ALL absent from stock
  (verified by parsing the on-disk stock gfx: 302 MENU_ names, none of ours). They
  belong to the inventory/menu movies, not the map. So "reuse existing charIds" is
  FALSE for the overlay/background layer.
- HOWEVER all 24 needed texture NAMES are present + RESOLVABLE process-wide. In the
  live (our-gfx) movie the worldmap def's OWN DefineExternalImage2 records are fully
  resolved: each record holds the virtual path
  "...\Game\menu\MENU_Tab_00.tga", a creator vtable ptr (exe+0x2C85288 family),
  flag 0xd, and the loaded texture size a0 00 a0 00 = 160x160 (cluster dumps at
  0x7ff2e00acc00 / 0x7ff2e00af100). The textures are loose .tga the mod loader serves;
  the engine resolves DefineExternalImage2 BY NAME/PATH at load and builds a real
  CSTextureImage (RTTI .?AVCSTextureImage@CS@@ x72, ImageResource@GFx x23 reachable
  from the def).
- Net: the only thing Path A must add that the stock movie lacks is the
  DefineExternalImage2 RESOURCE ENTRIES (name-refs) for the ~17 distinct textures our
  appended frames reference (the 9 vanilla charIds 10/21/53/57/105/109/112/113/162 the
  frames also use ARE already in the stock map dict - reuse those). The texture bits
  themselves never need shipping: the loader already serves the .tga, and the engine's
  own DefineExternalImage2 path loads+registers them. So "inject frames" reduces to
  "inject DefineExternalImage2 defs + frames", with NO manual texture upload and NO
  ScaleformTexRepository poke.

### TASK 2 - PO tag object model (byte map). CONFIDENCE MEDIUM-HIGH on layout, the
### exact present-flag-dependent field offsets need a clone (not hand-encode).
- This build does NOT use the assumed flat Array<ExecuteTag*> {ptr,count,cap} per
  frame. Tags are heap objects ~0x20 stride, intrusively linked. Real tag vtables live:
  PO2 = exe+0x2C65CC0 (the "alt"; the 0x142cc9360 variant count=0 - ignore),
  PO3 = exe+0x2C65D20. 165941 PO2 + 5304 PO3 objects in the movie arena.
- The Execute method (PO2 vtable slot6 = 0x1411bcce0) decodes the body:
    flags = byte[tag+8]; base = (flags signed <0) ? 9 : 1
    depth = u16 at [tag + 8 + base]   (so depth at tag+0x10/0x11 for base=1)
    flags bit0 (cl) and bit1 (al) select action: place-new vs modify-existing
  It binary-searches the live display list ([rbp+0x28]=data,[rbp+0x30]=count) by depth
  ([dispobj+0x14]) and, for place-new, copies the tag (rbx) into [dispobj+0x18..0x68]
  and sets [dispobj+0x10]=charDef base frame index; for modify it calls 0x1411bfd50
  (apply matrix/cxform). Observed live bodies show the depth/flag bytes exactly:
  e.g. tag @0x7ff2de6232a8 body "...0601000100..." = flags 0x06, depth 1;
  next "...0601000200..." depth 1 alt; "...2601000300004661" flags 0x26 + name "Fa".
- Implication: clone-append is far safer than hand-encoding. To add a frame, deep-copy
  existing tag OBJECTS (allocate via the movie heap, memcpy 0x20-ish, fix depth/charId/
  matrix/cxform in place, relink), rather than synthesize the variable body.

### TASK 1 - SpriteDef cid=171 this. CONFIDENCE LOW (not pinned this pass).
- The frame count is NOT stored as a literal u32 441/440 reachable from the def in the
  movie arena (0 hits in 0x7ff2d0..0x7ff2e1; the prior pass's 441 hits were the GFx JIT
  arena 0x7fff0... = false). So the sprite cannot be found by a frameCount value scan.
- The resource dictionary (charId->resource) was not cleanly walked: payload+0x20
  (0x7ff2dffeb010, vtable exe+0x2CBF030) is the GFx context resource-binding (file
  opener/image creator singletons), NOT the per-movie charId table; payload+0x108
  region is a name->id export table (held an export "font"->13). The per-movie
  charId->SpriteDef map node was not located. NEXT: trace it from the AddDisplayObject
  path (0x140f01b10) or PlaceObject loader (0x1411e23b0) which resolve charId->resource
  at runtime, capturing rcx/rdx live; or read charDef+0x3b8 (base frame index) on a
  display object to back into the sprite. This is the one genuine remaining live poke.

### Build-state note
Running game = our modified gfx (440-frame _new variant) via mod loader. Stock=348,
_new=440, _vanilla/_err=441 (extra anon "?" frame). For a STOCK-gfx Path-A target the
inject is 348 -> 440 (92 frames) + the DefineExternalImage2 defs above.
